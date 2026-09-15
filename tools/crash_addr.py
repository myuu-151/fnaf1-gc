"""Turns GameCube crash addresses into function names and source lines.

Usage:
  python tools/crash_addr.py 0x802890e4 0x802890a8 ...

Paste the addresses from Dolphin's "Invalid read from ..., PC = ..." warning, or the PC, LR
and STACK DUMP lines from the red crash screen. Uses FNAF1/Build/GCN/FNAF1.elf, so it only
matches the DOL built from the same source.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "FNAF1", "Build", "GCN", "FNAF1.elf")

def find_addr2line():
    # DEVKITPPC can be an msys-style path (/opt/devkitpro/...) that Windows can't open.
    for base in (os.environ.get("DEVKITPPC", ""), r"C:\devkitPro\devkitPPC"):
        for name in ("powerpc-eabi-addr2line.exe", "powerpc-eabi-addr2line"):
            path = os.path.join(base, "bin", name)
            if base and os.path.exists(path):
                return path
    sys.exit("powerpc-eabi-addr2line not found (install devkitPPC or set DEVKITPPC)")


def main():
    text = " ".join(sys.argv[1:])
    addresses = re.findall(r"0x8[0-9a-fA-F]{7}|\b8[0-9a-fA-F]{7}\b", text)
    if not addresses:
        sys.exit(__doc__)
    if not os.path.exists(ELF):
        sys.exit("missing %s (build the game first)" % ELF)

    addresses = [a if a.lower().startswith("0x") else "0x" + a for a in addresses]
    out = subprocess.run([find_addr2line(), "-f", "-C", "-i", "-e", ELF] + addresses,
                         capture_output=True, text=True, check=True).stdout.splitlines()

    # addr2line prints a function line then a file:line for each address (more with inlining).
    i = 0
    for address in addresses:
        function = out[i] if i < len(out) else "?"
        location = out[i + 1] if i + 1 < len(out) else "?"
        print("%s  %s\n            %s" % (address, function, location))
        i += 2


if __name__ == "__main__":
    main()
