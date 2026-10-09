# Sony's psp2cgc (a Windows x86 program from the Vita SDK host tools) under
# Wine, for scripts/vita-shader-bank.py and the other shader tools: they run
# `docker run ... -v $PSP2CGC:/psp2cgc/psp2cgc.exe:ro psp2cgc-wine wine /psp2cgc/psp2cgc.exe`.
# The compiler is mounted at run time, never baked into the image. On an arm64
# host the i386 image runs through qemu-user binfmt; QEMU_CPU=core2duo avoids
# psp2cgc crashes under qemu's default CPU model.
#   docker build -t psp2cgc-wine -f docker/psp2cgc-wine.Dockerfile docker
FROM --platform=linux/386 debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends wine wine32 && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEPREFIX=/wine
WORKDIR /work
