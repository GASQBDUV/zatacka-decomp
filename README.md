# Zatacka Decomp

C++ source-code reconstruction of **_Achtung, die Kurve!_ for MS-DOS** (`ZATACKA.EXE`) with byte-identical output.

## Getting Started

You need [Docker] to build and [DOSBox] or similar to play.

    docker build . -o compiled

The build will only succeed if the compiled `ZATACKA.EXE` matches the original one exactly.

To run the compiled game:

    dosbox compiled/ZATACKA.EXE

## Modding

If you've modified the source code, then skip the equality check:

    docker build . -o compiled --build-arg ALLOW_MISMATCH=1

## Diagnostics

To also output linker map (`.MAP`) and assembly (`.ASM`) files, add `--build-arg INCLUDE_MAP_AND_ASM=1`.

> [!IMPORTANT]
> Subsequent builds without `INCLUDE_MAP_AND_ASM` won't remove existing `.MAP` and `.ASM` files. Use e.g. `rm -r ./compiled/` to remove all build output manually.

## Background Story

A rough overview of how we achieved this:

  1. We asked Claude Code (Sonnet 5 and later Opus 5.5) to reverse-engineer the original `ZATACKA.EXE` using [Ghidra] and other tools.
  1. When Claude had produced a decompiled C file without any equivalence proof, we asked if it could reproduce the original executable exactly using an original Borland C++ compiler.
  1. After experimenting with different versions, compiler flags and source-code iterations, Claude managed to produce a byte-identical executable.
  1. We went through Claude's C++ source code manually to improve naming, DRY up magic numbers etc, using the Docker build setup as equivalence proof.

For details, see [Claude's detailed notes](docs/CLAUDE-NOTES.md).

[Docker]: https://www.docker.com/
[DOSBox]: https://www.dosbox.com/
[Ghidra]: https://github.com/nationalsecurityagency/ghidra
