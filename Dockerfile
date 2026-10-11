# syntax=docker/dockerfile:1.17@sha256:38387523653efa0039f8e1c89bb74a30504e76ee9f565e25c9a09841f9427b05
#
# Rebuilds ZATACKA.EXE from zatacka.cpp with Borland C++ 3.1 in headless DOSBox and fails unless the result is byte-identical to the original.
#
# Usage:
#
#   docker build -o <outdir> .
#
# For modified source, add `--build-arg ALLOW_MISMATCH=1` to keep the build from failing when the EXE differs.
#
# <outdir> gets ZATACKA.EXE plus EGAVGA.BGI and TSCR.CHR so the game runs from <outdir>.
# Add `--build-arg INCLUDE_MAP_AND_ASM=1` to also get ZATACKA.MAP (linker map) and ZATACKA.ASM (BCC -S listing).
#
# The compiler comes from the BC++ 3.1 floppy images in borland/.

FROM debian:trixie-20261005-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f AS build-stage

# Pin the archives the base image itself names in debian.sources, so apt-installed packages match the base image's archive state:
COPY <<EOF /etc/apt/sources.list.d/debian.sources
Types: deb
URIs: http://snapshot.debian.org/archive/debian/20261005T000000Z
Suites: trixie trixie-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
Check-Valid-Until: no

Types: deb
URIs: http://snapshot.debian.org/archive/debian-security/20261005T000000Z
Suites: trixie-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
Check-Valid-Until: no
EOF
RUN apt-get update \
 && apt-get install -y --no-install-recommends dosbox=0.74-3-5+b1 python3.13=3.13.5-2+deb13u5 \
 && rm -rf /var/lib/apt/lists/*
ENV TZ=UTC LANG=C.UTF-8 LC_ALL=C.UTF-8
WORKDIR /src
COPY original-game/ original-game/
COPY borland/ borland/
COPY build.py .
COPY zatacka.cpp .
ARG ALLOW_MISMATCH
ARG INCLUDE_MAP_AND_ASM
RUN python3.13 -I build.py /out

FROM scratch
COPY --from=build-stage /out/ /
