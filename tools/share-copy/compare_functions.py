#!/usr/bin/env python3
"""Compares the compiled code of two builds of libVivify.so.

    compare_functions.py <first .so> <second .so> [--objdump <llvm-objdump>]

First the loaded sections are compared byte for byte (.text, .rodata,
.data, .data.rel.ro, .init_array and the dynamic symbol table). When they are
all the same, the two libraries run the same code. Otherwise every function
is disassembled, addresses are masked (branch targets, page addresses,
literal-pool loads), and functions whose remaining instructions differ are
listed with the first difference.

Exit status 0 when every function is the same.
"""
import os
import re
import subprocess
import sys
import tempfile

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


ADDRESS = re.compile(r"0x[0-9a-f]+")


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
        elif current is not None and ":\t" in line:
            instruction = line.split(":\t", 1)[1]
            # addresses differ between two links of the same code; the symbol
            # a branch names (<name+offset>) is kept
            instruction = ADDRESS.sub("ADDR", instruction)
            body.append(instruction)
    if current is not None:
        out[current] = body
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

    fa, fb = functions(objdump, first), functions(objdump, second)
    names = sorted(set(fa) | set(fb))
    differ = [n for n in names if fa.get(n) != fb.get(n)]
    print("functions: %d in the first, %d in the second, %d identical after masking addresses, %d differ"
          % (len(fa), len(fb), len(names) - len(differ), len(differ)))
    for name in differ[:40]:
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
    if len(differ) > 40:
        print("  ... and %d more" % (len(differ) - 40))
    print("RESULT: %s" % ("the same code" if same_sections and not differ else
                          "same functions, sections differ" if not differ else "DIFFERENT"))
    return 0 if not differ else 1


if __name__ == "__main__":
    sys.exit(main())
