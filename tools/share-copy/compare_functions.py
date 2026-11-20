#!/usr/bin/env python3
"""Compares the compiled code of two builds of libVivify.so.

    compare_functions.py <first .so> <second .so> [--objdump <llvm-objdump>]

First the loaded sections are compared byte for byte (.text, .rodata,
.data, .data.rel.ro, .init_array and the dynamic symbol table). When they are
all the same, the two libraries run the same code: that is the proof, and
tools/make-share-copy.sh --check builds the copy so that it can hold.
Otherwise, as a guide to where two builds differ, every function
is disassembled, link-dependent addresses are hidden (see normalize), and
functions whose remaining instructions differ are listed with the first
difference. Local symbols the compiler numbers (name.1234) are matched
without their number.

Exit status 0 when the loaded sections are the same byte for byte; 1 when
they are not (the function list then says where); 2 when nothing could be
read.
"""
import os
import re
import subprocess
import sys
import tempfile

LIMIT = int(os.environ.get("COMPARE_LIMIT", "40"))
SECTIONS = (".text", ".rodata", ".data", ".data.rel.ro", ".init_array", ".dynsym", ".dynstr")


def tool(name, objdump):
    folder = os.path.dirname(objdump)
    candidate = os.path.join(folder, name)
    return candidate if os.path.exists(candidate) else name


def section_bytes(objcopy, lib, section):
    with tempfile.TemporaryDirectory() as folder:
        out = os.path.join(folder, "section.bin")
        result = subprocess.run([objcopy, "--dump-section", "%s=%s" % (section, out), lib,
                                 os.path.join(folder, "rest.so")], capture_output=True)
        if result.returncode != 0 or not os.path.exists(out):
            return None
        with open(out, "rb") as f:
            return f.read()


# "  331818:      \tb.eq\t0x332a6c <name+0x1294>": the address, a colon,
# spaces and a tab, then the instruction. (Splitting on ":\t" never matched
# llvm-objdump's spacing, which left every body empty and every function
# "identical".)
INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s+(\S.*)$")


TARGET = re.compile(r"0x[0-9a-f]+ <([^>]*)>")
LOCAL_NUMBER = re.compile(r"\.\d+(?=[+>]|$)")
REGISTER = re.compile(r"\b[xw](\d+)\b")
MEMORY = re.compile(r"\[(x\d+)(?:, #(-?0x[0-9a-f]+))?\]")
# Instructions that do not write their first register.
NO_DESTINATION = {"str", "strb", "strh", "stp", "stur", "sturb", "sturh", "stlr", "stlrb", "cmp", "cmn", "tst",
                  "tbz", "tbnz", "cbz", "cbnz", "b", "br", "blr", "bl", "ret", "nop", "prfm", "fcmp", "ccmp"}


def normalize(body):
    """Hides only what depends on where the linker put things.

    Branches, calls and literal loads keep the symbol they name, without its
    address and without the number the compiler gives local symbols
    (name.1234). adrp is dropped (its page, and the nearest symbol objdump
    names for it, depend on the layout), and so is nop (the linker turns some
    adrp into nop, and the add after it into adr, which is compared as that
    add). A register an adrp set, or an add derived from one, holds an
    address until it is written again or a call may change it; the offset an
    add or a load or store applies to it is hidden. Every other immediate (stack offsets,
    constants, characters) is compared as it is.
    """
    out = []
    pages = set()  # registers an adrp set, by number
    for instruction in body:
        op, _, args = instruction.partition("\t")
        args = args.split("//")[0].strip()
        if op == "nop":
            continue
        if op == "adrp":
            first = REGISTER.search(args)
            if first:
                pages.add(first.group(1))
            continue
        if op == "adr":
            # the linker relaxes adrp + add into nop + adr: compare it as the add
            first = REGISTER.search(args)
            if first:
                out.append("add\tx%s, x%s, #PAGEOFF" % (first.group(1), first.group(1)))
                pages.add(first.group(1))
            continue
        else:
            args = TARGET.sub(lambda m: "<%s>" % LOCAL_NUMBER.sub("", m.group(1)), args)
        registers = REGISTER.findall(args)
        derived = False
        if op in ("add", "sub") and len(registers) >= 2 and registers[1] in pages and "#" in args:
            args = re.sub(r"#0x[0-9a-f]+", "#PAGEOFF", args)
            derived = True  # still an address that depends on the layout
        else:
            memory = MEMORY.search(args)
            if memory and memory.group(1)[1:] in pages:
                args = args.replace(memory.group(0), "[%s, #PAGEOFF]" % memory.group(1))
        out.append(op + "\t" + args)
        if op.startswith("bl"):
            pages -= {str(n) for n in range(19)}
        elif derived:
            pages.add(registers[0])
        elif op not in NO_DESTINATION and not op.startswith("b.") and registers:
            pages.discard(registers[0])
            if op == "ldp" and len(registers) >= 2:
                pages.discard(registers[1])
    return out


def functions(objdump, lib):
    result = subprocess.run([objdump, "-d", "--no-show-raw-insn", lib], capture_output=True, text=True,
                            errors="replace")
    current, body, out = None, [], {}
    for line in result.stdout.splitlines():
        if line.endswith(">:") and " <" in line:
            if current is not None:
                out[current] = body
            current = line.split(" <", 1)[1][:-2]
            body = []
        elif current is not None and INSTRUCTION.match(line):
            body.append(INSTRUCTION.match(line).group(1))
    if current is not None:
        out[current] = body
    return {name: normalize(body) for name, body in out.items()}


def local_names(found):
    """name.1234 -> name; several of one name are kept in a stable order."""
    out = {}
    grouped = {}
    for name, body in found.items():
        grouped.setdefault(re.sub(r"\.\d+$", "", name), []).append(body)
    for name, bodies in grouped.items():
        bodies.sort()
        for index, body in enumerate(bodies):
            out[name if index == 0 else "%s#%d" % (name, index + 1)] = body
    return out


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    first, second = args[0], args[1]
    objdump = args[args.index("--objdump") + 1] if "--objdump" in args else "llvm-objdump"
    objcopy = tool("llvm-objcopy", objdump)

    same_sections = True
    for section in SECTIONS:
        a, b = section_bytes(objcopy, first, section), section_bytes(objcopy, second, section)
        if a is None and b is None:
            continue
        equal = a == b
        same_sections &= equal
        print("%-14s %s (%s / %s bytes)" % (section, "same" if equal else "DIFFERENT",
                                             len(a) if a is not None else "-", len(b) if b is not None else "-"))

    fa, fb = local_names(functions(objdump, first)), local_names(functions(objdump, second))
    instructions = sum(len(body) for body in fa.values())
    if instructions == 0:
        print("RESULT: no instructions read from the disassembly; nothing was compared")
        return 2
    names = sorted(set(fa) | set(fb))
    differ = [n for n in names if fa.get(n) != fb.get(n)]
    print("functions: %d in the first, %d in the second, %d identical after masking addresses, %d differ"
          % (len(fa), len(fb), len(names) - len(differ), len(differ)))
    for name in differ[:LIMIT]:
        a, b = fa.get(name), fb.get(name)
        if a is None or b is None:
            print("  only in the %s: %s" % ("second" if a is None else "first", name))
            continue
        for index, (x, y) in enumerate(zip(a, b)):
            if x != y:
                print("  %s: instruction %d: %s  |  %s" % (name, index, x, y))
                break
        else:
            print("  %s: %d vs %d instructions" % (name, len(a), len(b)))
    if len(differ) > LIMIT:
        print("  ... and %d more" % (len(differ) - LIMIT))
    print("RESULT: %s" % ("the same code" if same_sections and not differ else
                          "same functions, sections differ" if not differ else "DIFFERENT"))
    return 0 if same_sections else 1


if __name__ == "__main__":
    sys.exit(main())
