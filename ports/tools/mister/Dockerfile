# The MiSTer game's toolchain: Debian bullseye's armhf gcc, whose glibc (2.31) is the MiSTer's
#   docker build -t mvc2-bullseye-armhf tools/mister
FROM python:3.11-slim-bullseye@sha256:9e25f400253a5fa3191813d6a67eb801ca1e6f012b3bd2588fa6920b59e3eba6
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\ndeb http://archive.debian.org/debian bullseye-backports main\n' > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends gcc-arm-linux-gnueabihf libc6-dev-armhf-cross make ninja-build \
    && apt-get install -y --no-install-recommends -t bullseye-backports cmake \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
