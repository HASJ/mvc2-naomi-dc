"""The PSP's assembler as gcc runs it (-B): a division by 0 gives quotient and remainder 0, as SHC's code did, in place
of the break that would stop the program.

usage: divzero_as.py AS ARGS...    (the real assembler, then gcc's arguments for it)

gcc's check (-mcheck-zero-division, -mdivide-breaks) is one insn, never in a delay slot:
    bne R,$0,1f / div $0,A,R / break 7 / 1:
It becomes
    mthi $0 / beq R,$0,1f / mtlo $0 / div $0,A,R / 1:
"""
import os
import re
import subprocess
import sys
import tempfile

CHECK = re.compile(r"^\tbne\t(\$\w+),\$0,1f\n\t(divu?)\t\$0,(\$\w+),\1\n\tbreak\t7\n", re.M)


def fix(text):
    return CHECK.sub(lambda m: "\tmthi\t$0\n\tbeq\t%s,$0,1f\n\tmtlo\t$0\n\t%s\t$0,%s,%s\n" % (m[1], m[2], m[3], m[1]),
                     text)


def main():
    real, args = sys.argv[1], sys.argv[2:]
    inputs = [i for i, a in enumerate(args) if (a == "-" or a.endswith((".s", ".S"))) and (i == 0 or args[i - 1] != "-o")]
    if inputs:
        i = inputs[-1]
        text = sys.stdin.read() if args[i] == "-" else open(args[i]).read()
    else:
        i, text = None, sys.stdin.read()
    fd, path = tempfile.mkstemp(suffix=".s")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(fix(text))
        if i is None:
            args.append(path)
        else:
            args[i] = path
        return subprocess.call([real] + args)
    finally:
        os.unlink(path)


if __name__ == "__main__":
    sys.exit(main())
