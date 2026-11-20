#!/usr/bin/env python3
"""Cleans a staged copy of the source tree for sharing (tools/make-share-copy.sh).

    clean.py <stage dir> --clang <clang binary> [--report <file>]

Every step works on the staged copy only; the working repository is never
touched.

1. Comments are removed from this project's own source files, each language
   with a real tokenizer:
     C and C++    clang's raw lexer (clang -cc1 -dump-raw-tokens) says where
                  each comment starts; the comment's end follows from the
                  language (a block comment ends at the first */, a line
                  comment at the first newline not spliced by a backslash).
     Python       the standard library's tokenize module.
     PowerShell   PowerShell's own parser (pwsh).
     YAML         PyYAML's scanner: a # outside every scanned token.
     CMake        a lexer that follows the CMake language reference (quoted
                  and bracket arguments, line and bracket comments).
     Shell        a lexer for the quoting, expansions and here-documents the
                  scripts use, checked with bash itself (below).
     .gitignore   a line whose first character is # (git's rule).
   Strings are never changed. A comment is kept when it holds a licence or
   copyright notice or credits another project (KEEP below). In C and C++
   each removed comment leaves its line breaks behind, so every line keeps
   its number: the compiled code records source lines for its log messages
   and stays identical. In scripts, lines that held only a comment go.
2. Each cleaned file is checked: for C and C++ clang's token stream without
   comments is the same before and after; for Python the AST is the same;
   for YAML the loaded data is the same; for shell, bash prints the same
   code for both (declare -f drops comments); PowerShell's parser finds the
   same non-comment tokens. A file that fails the check stops the run.
3. rewording.json beside this script holds exact text replacements for docs
   written as working notes. Each "old" text must occur exactly once, or the
   run stops: a doc that changed since the rule was written needs the rule
   updated by hand.
4. Third-party and generated files are copied as they are (UNTOUCHED).
"""
import ast
import io
import json
import os
import subprocess
import sys
import tokenize

HERE = os.path.dirname(os.path.abspath(__file__))

# Copied unchanged: vendored projects, third-party code, binary assets,
# generated fixtures.
UNTOUCHED = ("extern/", "include/third_party/", "assets/", "tools/bundleconvert/fixtures/")

# A comment that mentions any of these is kept: licence and copyright
# notices, and credit to other projects and people.
KEEP = ("copyright", "licence", "license", "licences", "licenses", "licensed", "spdx", "aeroluna", "trail-b1az3r", "rayla", "axo-lotl",
        "gay-axolotl", "braxed", "lars27110", "webbs7524", "pattt160", "gamesbeash", "llealloo",
        "redbrumbler", "hlslcc", "bcdec", "iorange", "kudlai", "ported from", "port of", "adapted from",
        "credit", "credits", "thanks to")

CPP_EXT = (".cpp", ".hpp", ".h", ".cc", ".c", ".inl")


def keep(text):
    """True when the comment holds one of KEEP as a whole word or phrase."""
    lower = text.lower()
    for word in KEEP:
        start = lower.find(word)
        while start >= 0:
            end = start + len(word)
            before = lower[start - 1] if start > 0 else " "
            after = lower[end] if end < len(lower) else " "
            if not (before.isalnum() or before == "_") and not (after.isalnum() or after == "_"):
                return True
            start = lower.find(word, start + 1)
    return False


def blank_out(data, spans):
    """data with each [start, end) span replaced by the line breaks it held.
    A comment within one line between two non-blank characters becomes one
    space, as the languages themselves read it (a/**/b is two tokens)."""
    is_text = isinstance(data, str)
    newline, space, empty = ("\n", " ", "") if is_text else (b"\n", b" ", b"")
    blank = " \t\r\n" if is_text else b" \t\r\n"
    out = []
    pos = 0
    for start, end in sorted(spans):
        out.append(data[pos:start])
        breaks = data.count(newline, start, end)
        if breaks:
            out.append(newline * breaks)
        else:
            before = data[start - 1:start] if start > 0 else empty
            after = data[end:end + 1]
            touching = before and after and before not in blank and after not in blank
            out.append(space if touching else empty)
        pos = end
    out.append(data[pos:])
    return empty.join(out)


def strip_trailing(data, touched_lines, newline):
    """Removes trailing spaces and tabs on the lines a removal touched."""
    lines = data.split(newline)
    for index in touched_lines:
        if 0 <= index < len(lines):
            lines[index] = lines[index].rstrip(" \t" if isinstance(data, str) else b" \t")
    return newline.join(lines)


def touched(data, spans, newline):
    """Line indexes (0-based) where a span starts or ends."""
    result = set()
    for start, end in spans:
        result.add(data.count(newline, 0, start))
        result.add(data.count(newline, 0, end))
    return result


# --- C and C++ ------------------------------------------------------------------

def clang_tokens(clang, path):
    """[(kind, byte offset)] from clang's raw lexer."""
    with open(path, "rb") as f:
        data = f.read()
    # line starts, for turning line:column into a byte offset
    starts = [0]
    for i, byte in enumerate(data):
        if byte == 0x0A:
            starts.append(i + 1)
    result = subprocess.run([clang, "-cc1", "-dump-raw-tokens", "-x", "c++", "-std=c++20", path],
                            capture_output=True)
    text = result.stderr.decode("utf-8", "surrogateescape") + result.stdout.decode("utf-8", "surrogateescape")
    marker = "\tLoc=<" + path + ":"
    pieces = text.split(marker)
    tokens = []
    for i in range(1, len(pieces)):
        record = pieces[i - 1]
        # the record of this token starts after the previous Loc's "L:C>\n"
        if i > 1:
            record = record.split(">\n", 1)[1] if ">\n" in record else record
        kind = record.split(" ", 1)[0]
        loc = pieces[i].split(">", 1)[0]
        line, col = loc.split(":")[:2]
        tokens.append((kind, starts[int(line) - 1] + int(col) - 1))
    return data, tokens


def cpp_comment_end(data, start):
    if data.startswith(b"/*", start):
        end = data.find(b"*/", start + 2)
        if end < 0:
            raise ValueError("unterminated block comment at byte %d" % start)
        return end + 2
    pos = start
    while True:
        end = data.find(b"\n", pos)
        if end < 0:
            return len(data)
        # a backslash right before the newline (or before \r\n) splices the line
        back = end - 1
        if back >= 0 and data[back:back + 1] == b"\r":
            back -= 1
        if back >= start and data[back:back + 1] == b"\\":
            pos = end + 1
            continue
        return end


def clean_cpp(clang, path):
    data, tokens = clang_tokens(clang, path)
    spans, kept = [], 0
    for kind, offset in tokens:
        if kind != "comment":
            continue
        end = cpp_comment_end(data, offset)
        if keep(data[offset:end].decode("utf-8", "replace")):
            kept += 1
            continue
        spans.append((offset, end))
    if not spans:
        return None, 0, kept
    cleaned = blank_out(data, spans)
    cleaned = strip_trailing(cleaned, touched(data, spans, b"\n"), b"\n")
    return cleaned, len(spans), kept


def check_cpp(clang, original_path, cleaned_path):
    def significant(path):
        _, tokens = clang_tokens(clang, path)
        with open(path, "rb") as f:
            data = f.read()
        # kinds and spellings of every token but comments and whitespace
        result = []
        ordered = sorted(tokens, key=lambda t: t[1])
        for index, (kind, offset) in enumerate(ordered):
            if kind in ("comment", "unknown"):
                continue
            end = ordered[index + 1][1] if index + 1 < len(ordered) else len(data)
            result.append((kind, data[offset:end].rstrip()))
        return result
    return significant(original_path) == significant(cleaned_path)


# --- Python -------------------------------------------------------------------------

def clean_python(path):
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    offsets = [0]
    for line in text.split("\n"):
        offsets.append(offsets[-1] + len(line) + 1)
    spans, kept = [], 0
    for token in tokenize.tokenize(io.BytesIO(raw).readline):
        if token.type != tokenize.COMMENT:
            continue
        (srow, scol), (erow, ecol) = token.start, token.end
        if srow == 1 and token.string.startswith("#!"):
            kept += 1
            continue
        if srow <= 2 and "coding" in token.string:
            kept += 1
            continue
        if keep(token.string):
            kept += 1
            continue
        spans.append((offsets[srow - 1] + scol, offsets[erow - 1] + ecol))
    if not spans:
        return None, 0, kept
    cleaned = blank_out(text, spans)
    cleaned = strip_trailing(cleaned, touched(text, spans, "\n"), "\n")
    return cleaned.encode("utf-8"), len(spans), kept


def check_python(original, cleaned):
    return ast.dump(ast.parse(original)) == ast.dump(ast.parse(cleaned))


# --- PowerShell --------------------------------------------------------------------

PS_TOKENS = r"""
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$tokens, [ref]$errors)
$out = foreach ($t in $tokens) { ,@($t.Kind.ToString(), $t.Extent.StartOffset, $t.Extent.EndOffset, $t.Text) }
ConvertTo-Json -Compress -Depth 3 @($out)
"""


def ps_tokens(path):
    # -File with the path as an argument: with -Command, pwsh would read the
    # path as more of the command and run the script being cleaned.
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False) as f:
        f.write(PS_TOKENS)
        script = f.name
    try:
        result = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", script, path],
                                capture_output=True, text=True)
    finally:
        os.remove(script)
    if result.returncode != 0:
        raise RuntimeError("pwsh could not parse %s: %s" % (path, result.stderr))
    return json.loads(result.stdout or "[]")


def clean_powershell(path):
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        text = f.read()
    spans, kept = [], 0
    for kind, start, end, token_text in ps_tokens(path):
        if kind != "Comment":
            continue
        if token_text.lower().startswith("#requires") or keep(token_text):
            kept += 1
            continue
        spans.append((start, end))
    if not spans:
        return None, 0, kept
    newline = "\r\n" if "\r\n" in text else "\n"
    cleaned = blank_out(text, spans)
    cleaned = strip_trailing(cleaned, touched(text, spans, "\n"), "\n")
    return cleaned.encode("utf-8"), len(spans), kept


def check_powershell(original_path, cleaned_path):
    def significant(path):
        return [(k, t) for k, _, _, t in ps_tokens(path) if k not in ("Comment", "NewLine", "LineContinuation")]
    return significant(original_path) == significant(cleaned_path)


# --- YAML ---------------------------------------------------------------------------

def clean_yaml(path):
    import yaml  # PyYAML, from the venv make-share-copy.sh sets up
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    covered = []
    for token in yaml.scan(text, Loader=yaml.SafeLoader):
        if token.start_mark.index < token.end_mark.index:
            covered.append((token.start_mark.index, token.end_mark.index))
    covered.sort()

    def inside(pos):
        for start, end in covered:
            if start <= pos < end:
                return True
            if start > pos:
                return False
        return False

    spans, kept = [], 0
    pos = 0
    while True:
        pos = text.find("#", pos)
        if pos < 0:
            break
        before = text[pos - 1] if pos > 0 else "\n"
        if before in " \t\n" and not inside(pos):
            end = text.find("\n", pos)
            end = len(text) if end < 0 else end
            if keep(text[pos:end]):
                kept += 1
            else:
                spans.append((pos, end))
            pos = end
        else:
            pos += 1
    if not spans:
        return None, 0, kept
    cleaned = blank_out(text, spans)
    cleaned = strip_trailing(cleaned, touched(text, spans, "\n"), "\n")
    return cleaned.encode("utf-8"), len(spans), kept


def check_yaml(original, cleaned):
    import yaml
    return yaml.safe_load(original) == yaml.safe_load(cleaned)


# --- CMake --------------------------------------------------------------------------

def cmake_bracket_open(text, pos):
    """Length of a bracket opening [=*[ at pos, and its = count; (0, 0) if none."""
    if pos >= len(text) or text[pos] != "[":
        return 0, 0
    i = pos + 1
    while i < len(text) and text[i] == "=":
        i += 1
    if i < len(text) and text[i] == "[":
        return i - pos + 1, i - pos - 1
    return 0, 0


def clean_cmake(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    spans, kept = [], 0
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "\\":
            i += 2
            continue
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "[":
            length, equals = cmake_bracket_open(text, i)
            if length:
                close = "]" + "=" * equals + "]"
                end = text.find(close, i + length)
                i = n if end < 0 else end + len(close)
                continue
            i += 1
            continue
        if c == "#":
            length, equals = cmake_bracket_open(text, i + 1)
            if length:
                close = "]" + "=" * equals + "]"
                end = text.find(close, i + 1 + length)
                end = n if end < 0 else end + len(close)
            else:
                end = text.find("\n", i)
                end = n if end < 0 else end
            if keep(text[i:end]):
                kept += 1
            else:
                spans.append((i, end))
            i = end
            continue
        i += 1
    if not spans:
        return None, 0, kept
    cleaned = blank_out(text, spans)
    cleaned = strip_trailing(cleaned, touched(text, spans, "\n"), "\n")
    return cleaned.encode("utf-8"), len(spans), kept


# --- Shell --------------------------------------------------------------------------

def shell_comment_spans(text):
    """Comments in a bash script: a # that starts a word, outside quotes,
    expansions and here-document bodies."""
    spans = []
    i, n = 0, len(text)
    word_start = True
    pending_heredocs = []  # delimiters whose bodies start at the next newline

    def skip_dollar(i):
        # $( ... ), $(( ... )), ${ ... }: skip to the matching close
        if text.startswith("$((", i):
            depth, i = 2, i + 3
            while i < n and depth:
                depth += {"(": 1, ")": -1}.get(text[i], 0)
                i += 1
            return i
        if text.startswith("$(", i):
            return skip_group(i + 2, ")")
        if text.startswith("${", i):
            depth, i = 1, i + 2
            while i < n and depth:
                if text[i] == "\\":
                    i += 2
                    continue
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                i += 1
            return i
        return i + 2 if i + 1 < n else i + 1  # $#, $?, $1 ...

    def skip_double(i):
        i += 1
        while i < n and text[i] != '"':
            if text[i] == "\\":
                i += 2
            elif text[i] == "$":
                i = skip_dollar(i)
            elif text[i] == "`":
                end = text.find("`", i + 1)
                i = n if end < 0 else end + 1
            else:
                i += 1
        return i + 1

    def skip_group(i, close):
        # inside $( ... ): nested quotes, expansions and comments
        while i < n:
            c = text[i]
            if c == close:
                return i + 1
            if c == "\\":
                i += 2
            elif c == "'":
                end = text.find("'", i + 1)
                i = n if end < 0 else end + 1
            elif c == '"':
                i = skip_double(i)
            elif c == "$":
                i = skip_dollar(i)
            elif c == "(":
                i = skip_group(i + 1, ")")
            else:
                i += 1
        return i

    while i < n:
        c = text[i]
        if c == "\n":
            i += 1
            word_start = True
            for delimiter, strip_tabs in pending_heredocs:
                while i < n:
                    end = text.find("\n", i)
                    end = n if end < 0 else end
                    line = text[i:end]
                    i = end + 1
                    if (line.lstrip("\t") if strip_tabs else line) == delimiter:
                        break
            pending_heredocs = []
            continue
        if c in " \t;&|()":
            word_start = True
            i += 1
            continue
        if c == "#" and word_start:
            end = text.find("\n", i)
            end = n if end < 0 else end
            spans.append((i, end))
            i = end
            continue
        word_start = False
        if c == "\\":
            i += 2
        elif c == "'":
            end = text.find("'", i + 1)
            i = n if end < 0 else end + 1
        elif c == '"':
            i = skip_double(i)
        elif c == "$":
            i = skip_dollar(i)
        elif c == "`":
            end = text.find("`", i + 1)
            i = n if end < 0 else end + 1
        elif text.startswith("<<", i) and not text.startswith("<<<", i):
            j = i + 2
            strip_tabs = j < n and text[j] == "-"
            j += 1 if strip_tabs else 0
            while j < n and text[j] in " \t":
                j += 1
            k = j
            while k < n and text[k] not in " \t\n;&|<>()":
                k += 1
            word = text[j:k].replace("'", "").replace('"', "").replace("\\", "")
            if word:
                pending_heredocs.append((word, strip_tabs))
            i = k
        else:
            i += 1
    return spans


def clean_shell(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    spans, kept = [], 0
    for start, end in shell_comment_spans(text):
        comment = text[start:end]
        if (start == 0 and comment.startswith("#!")) or keep(comment):
            kept += 1
            continue
        spans.append((start, end))
    if not spans:
        return None, 0, kept
    cleaned = blank_out(text, spans)
    cleaned = strip_trailing(cleaned, touched(text, spans, "\n"), "\n")
    return cleaned.encode("utf-8"), len(spans), kept


def bash_canonical(text):
    # bash's own printer of a function body leaves comments out
    script = "__share_check() {\n" + text + "\n}\ndeclare -f __share_check\n"
    result = subprocess.run(["bash", "--noprofile", "--norc", "-n", "-c", script], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    result = subprocess.run(["bash", "--noprofile", "--norc", "-c", script], capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else None


def check_shell(original, cleaned):
    a, b = bash_canonical(original), bash_canonical(cleaned)
    return a is not None and a == b


# --- .gitignore ---------------------------------------------------------------------

def clean_gitignore(path):
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")
    removed = 0
    for index, line in enumerate(lines):
        if line.startswith("#"):
            lines[index] = ""
            removed += 1
    return ("\n".join(lines).encode("utf-8") if removed else None), removed, 0


# --- Driver -------------------------------------------------------------------------

def drop_emptied(original, cleaned):
    """Scripts only: drops the lines that held nothing but a comment. Line
    numbers matter only for compiled C and C++ (and stay there)."""
    old_lines = original.split(b"\n")
    new_lines = cleaned.split(b"\n")
    if len(old_lines) != len(new_lines):
        return cleaned
    kept = [new for old, new in zip(old_lines, new_lines) if not (old.strip() and not new.strip())]
    return b"\n".join(kept)


def language(rel):
    name = os.path.basename(rel)
    if rel.endswith(CPP_EXT):
        return "cpp"
    if rel.endswith(".py"):
        return "python"
    if rel.endswith(".ps1"):
        return "powershell"
    if rel.endswith((".yml", ".yaml")):
        return "yaml"
    if name == "CMakeLists.txt" or rel.endswith(".cmake"):
        return "cmake"
    if rel.endswith(".sh"):
        return "shell"
    if name == ".gitignore":
        return "gitignore"
    return None


def main():
    args = sys.argv[1:]
    if not args or "--clang" not in args:
        print(__doc__)
        return 2
    stage = os.path.abspath(args[0])
    clang = args[args.index("--clang") + 1]
    report_path = args[args.index("--report") + 1] if "--report" in args else None

    files = []
    for root, dirs, names in os.walk(stage):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in names:
            full = os.path.join(root, name)
            files.append(os.path.relpath(full, stage).replace(os.sep, "/"))
    files.sort()

    report = []
    totals = {"files": 0, "removed": 0, "kept": 0}
    failures = []
    for rel in files:
        if rel.startswith(UNTOUCHED):
            continue
        lang = language(rel)
        if lang is None:
            continue
        path = os.path.join(stage, rel)
        with open(path, "rb") as f:
            original = f.read()
        try:
            if lang == "cpp":
                cleaned, removed, kept = clean_cpp(clang, path)
            elif lang == "python":
                cleaned, removed, kept = clean_python(path)
            elif lang == "powershell":
                cleaned, removed, kept = clean_powershell(path)
            elif lang == "yaml":
                cleaned, removed, kept = clean_yaml(path)
            elif lang == "cmake":
                cleaned, removed, kept = clean_cmake(path)
            elif lang == "shell":
                cleaned, removed, kept = clean_shell(path)
            else:
                cleaned, removed, kept = clean_gitignore(path)
        except Exception as error:  # noqa: BLE001
            failures.append("%s: %s" % (rel, error))
            continue
        if cleaned is None:
            if kept:
                report.append("%-60s %-10s kept %d" % (rel, lang, kept))
                totals["kept"] += kept
            continue
        if cleaned.count(b"\n") != original.count(b"\n"):
            failures.append("%s: line count changed" % rel)
            continue
        if lang != "cpp":
            cleaned = drop_emptied(original, cleaned)
        ok = True
        if lang == "cpp":
            checked = path + ".share-check"
            with open(checked, "wb") as f:
                f.write(cleaned)
            ok = check_cpp(clang, path, checked)
            os.remove(checked)
        elif lang == "python":
            ok = check_python(original.decode("utf-8"), cleaned.decode("utf-8"))
        elif lang == "yaml":
            ok = check_yaml(original.decode("utf-8"), cleaned.decode("utf-8"))
        elif lang == "shell":
            ok = check_shell(original.decode("utf-8"), cleaned.decode("utf-8"))
        elif lang == "powershell":
            checked = path + ".share-check.ps1"
            with open(checked, "wb") as f:
                f.write(cleaned)
            ok = check_powershell(path, checked)
            os.remove(checked)
        if not ok:
            failures.append("%s: the cleaned file is not the same code (%s check failed)" % (rel, lang))
            continue
        with open(path, "wb") as f:
            f.write(cleaned)
        totals["files"] += 1
        totals["removed"] += removed
        totals["kept"] += kept
        report.append("%-60s %-10s removed %d, kept %d" % (rel, lang, removed, kept))

    # Docs: exact rewording of working-note phrases.
    with open(os.path.join(HERE, "rewording.json"), encoding="utf-8") as f:
        rules = json.load(f)
    reworded = 0
    for rule in rules:
        path = os.path.join(stage, rule["file"])
        if not os.path.exists(path):
            failures.append("rewording: %s is not in the copy" % rule["file"])
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        count = text.count(rule["old"])
        if count != 1:
            failures.append("rewording: %s: the text to replace occurs %d times (expected once): %r"
                            % (rule["file"], count, rule["old"][:80]))
            continue
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace(rule["old"], rule["new"]))
        reworded += 1
        report.append("%-60s reworded: %r" % (rule["file"], rule["old"][:70]))

    summary = ("comments: %d removed from %d files, %d kept (licence, copyright or credit); "
               "doc phrases reworded: %d of %d" % (totals["removed"], totals["files"], totals["kept"], reworded,
                                                  len(rules)))
    report.append("")
    report.append(summary)
    if report_path:
        with open(report_path, "w", encoding="utf-8") as f:
            f.write("\n".join(report) + "\n")
    print(summary)
    if failures:
        print("FAILED:")
        for failure in failures:
            print("  " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
