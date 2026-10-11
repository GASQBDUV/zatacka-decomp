"""Builds zatacka.cpp with Borland C++ 3.1 in headless DOSBox: the result is byte-identical to ZATACKA.EXE.

Usage:  [ALLOW_MISMATCH=1] [INCLUDE_MAP_AND_ASM=1] python build.py OUTDIR
  ALLOW_MISMATCH=1        exit successfully even if output does not match original ZATACKA.EXE (useful for deliberate modifications)
  INCLUDE_MAP_AND_ASM=1   also output ZATACKA.MAP (linker map) and ZATACKA.ASM (BCC -S listing)
  OUTDIR                  emptied, then gets ZATACKA.EXE + EGAVGA.BGI/TSCR.CHR to run it
Needs: dosbox (0.74).  The floppy images are FAT12; the installer's ZIPs use PKZIP 1.x
"implode", so they are unpacked with the UNZIP.EXE from disk 1 inside DOSBox.  BCC needs TASM for the inline asm.
"""

import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Small model, speed optimizations, inline functions out of line (random() is a call), compile via TASM (-B): the
# original went through single-pass TASM, which leaves "jg $+5 / jmp short / nop" for one forward branch in main.
OPTIONS = os.environ.get("BCC_OPTIONS", "-ms -O2 -vi- -B")
FIXED_MTIME = 700000000  # 1992-03-07
OUTPUTS = ["ZATACKA.EXE", "ZATACKA.MAP", "ZATACKA.ASM", "EGAVGA.BGI", "TSCR.CHR"]
# Unpacked BC++ installation (C: in DOSBox), reused across builds:
CACHE = os.path.join(HERE, ".cache")
DOSROOT = os.path.join(CACHE, "dosroot")
# the BC++ 3.1 installation floppies needed, from the 3.5" 1.44 MB set
DISKS = [
    "Disk01.img",
    "Disk02.img",
    "Disk03.img",
    "Disk07.img",
    "Disk08.img",
    "Disk09.img",
    "Disk13.img",
]
# zip (or split .CA* parts, concatenated) -> install directory
PARTS = [
    (["CMDLINE.CA1", "CMDLINE.CA2"], "BIN"),
    (["BIN.ZIP"], "BIN"),
    (["TASM.CA1", "TASM.CA2"], "BIN"),
    (["INCLUDE.ZIP"], "INCLUDE"),
    (["SLIB.ZIP"], "LIB"),
    (["XLIB.ZIP"], "LIB"),
    (["BGI.ZIP"], "BGI"),
]


def fat12_files(img):
    """{NAME.EXT: bytes} of the root directory of a FAT12 floppy image (the BC++ disks have no subdirectories)."""
    d = open(img, "rb").read()
    bps, spc, rsv, nfat, nroot, _, _, spf = struct.unpack_from("<HBHBHHBH", d, 11)
    fat = d[rsv * bps : (rsv + spf) * bps]
    root = (rsv + nfat * spf) * bps
    data = root + nroot * 32
    csize = spc * bps
    out = {}
    for i in range(root, data, 32):
        e = d[i : i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] & 0x18:
            continue
        name = e[0:8].decode("cp437").rstrip() + "." + e[8:11].decode("cp437").rstrip()
        c, size, buf = (
            struct.unpack_from("<H", e, 26)[0],
            struct.unpack_from("<I", e, 28)[0],
            b"",
        )
        while 2 <= c < 0xFF8 and len(buf) < size:
            buf += d[data + (c - 2) * csize : data + (c - 1) * csize]
            v = struct.unpack_from("<H", fat, c * 3 // 2)[0]
            c = v >> 4 if c & 1 else v & 0xFFF
        out[name.rstrip(".")] = buf[:size]
    return out


def dosbox(root, batch):
    """Run C:\\RUN.BAT (with root as C:) in headless DOSBox; returns C:\\OUT.TXT."""
    open(os.path.join(root, "RUN.BAT"), "w", newline="\r\n").write(batch)
    conf = os.path.join(root, "dosbox.conf")
    open(conf, "w").write(
        "[sdl]\noutput=surface\n[dosbox]\nmemsize=16\n[cpu]\ncore=dynamic\ncycles=max\n[mixer]\nnosound=true\n"
        f'[autoexec]\nmount c "{root}"\nc:\ncall RUN.BAT\nexit\n'
    )
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    subprocess.run(
        ["dosbox", "-conf", conf, "-noconsole"],
        env=env,
        capture_output=True,
        timeout=600,
    )
    p = os.path.join(root, "OUT.TXT")
    return (
        open(p, encoding="cp437").read().replace("\r", "") if os.path.exists(p) else ""
    )


def install(root):
    files = {}
    for disk in DISKS:
        files.update(fat12_files(os.path.join(HERE, "borland", disk)))
    os.makedirs(os.path.join(root, "ZIPS"))
    open(os.path.join(root, "UNZIP.EXE"), "wb").write(files["UNZIP.EXE"])
    lines = [f"mkdir BC\\{d}" for d in ("BIN", "INCLUDE", "LIB", "BGI")]
    lines[:0] = ["mkdir BC"]
    for i, (parts, dst) in enumerate(PARTS):
        open(os.path.join(root, "ZIPS", f"P{i}.ZIP"), "wb").write(
            b"".join(files[p] for p in parts)
        )
        lines.append(f"UNZIP ZIPS\\P{i}.ZIP BC\\{dst} >> C:\\OUT.TXT")
    log = dosbox(root, "\n".join(lines) + "\n")
    if not os.path.exists(os.path.join(root, "BC", "BIN", "BCC.EXE")):
        sys.exit("unpacking failed:\n" + log)


def main():
    allow_mismatch = os.environ.get("ALLOW_MISMATCH") == "1"
    include_map_and_asm = os.environ.get("INCLUDE_MAP_AND_ASM") == "1"
    outdir = sys.argv[1]
    # Empty OUTDIR, but only if it holds nothing but earlier build output (so a wrong path can't wipe anything else):
    if os.path.exists(outdir):
        unexpected = sorted(set(os.listdir(outdir)) - set(OUTPUTS))
        if unexpected:
            sys.exit(f"{outdir} contains files not produced by the build, so it won't be emptied: {unexpected}")
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    if not os.path.exists(os.path.join(DOSROOT, "BC", "BIN", "BCC.EXE")):
        shutil.rmtree(DOSROOT, ignore_errors=True)
        os.makedirs(CACHE, exist_ok=True)
        open(os.path.join(CACHE, ".gitignore"), "w").write("*\n")
        install(DOSROOT)
    w = os.path.join(DOSROOT, "W")
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    shutil.copy(
        os.path.join(HERE, "zatacka.cpp"), os.path.join(w, "ZATACKA.CPP")
    )
    for d, _, ns in os.walk(
        DOSROOT
    ):  # fixed file dates: BCC -S records them in ?debug lines
        for n in ns:
            os.utime(os.path.join(d, n), (FIXED_MTIME, FIXED_MTIME))
    batch = (
        "set PATH=C:\\BC\\BIN\ncd W\n"
        f"BCC {OPTIONS} -lm -IC:\\BC\\INCLUDE -LC:\\BC\\LIB ZATACKA.CPP GRAPHICS.LIB > C:\\OUT.TXT\n"
    )
    if include_map_and_asm:
        batch += f"BCC {OPTIONS.replace('-B', '')} -S -IC:\\BC\\INCLUDE ZATACKA.CPP >> C:\\OUT.TXT\n"
    log = dosbox(DOSROOT, batch)
    print(log.strip(), flush=True)
    if not os.path.exists(os.path.join(w, "ZATACKA.EXE")):
        sys.exit("build failed")
    shutil.copy(os.path.join(w, "ZATACKA.EXE"), outdir)
    if include_map_and_asm:
        for f in ("ZATACKA.MAP", "ZATACKA.ASM"):
            if os.path.exists(os.path.join(w, f)):
                shutil.copy(os.path.join(w, f), outdir)
    for f in ("EGAVGA.BGI", "TSCR.CHR"):  # loaded at runtime from the current directory
        shutil.copy(os.path.join(DOSROOT, "BC", "BGI", f), outdir)
    same = {}
    for f in ("ZATACKA.EXE", "EGAVGA.BGI", "TSCR.CHR"):
        same[f] = (
            open(os.path.join(outdir, f), "rb").read()
            == open(os.path.join(HERE, "original-game", f), "rb").read()
        )
        print(
            f"{f} is byte-identical to the original"
            if same[f]
            else f"{f} differs from the original"
        )
    ok = allow_mismatch or (
        same["ZATACKA.EXE"]
        and same["EGAVGA.BGI"]
        and same["TSCR.CHR"]
    )
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
