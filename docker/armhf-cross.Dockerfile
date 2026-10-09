# Cross-compilation environment for arm32 (armhf, ARMv7-A hard-float) Linux,
# tuned for Cortex-A9. Runs natively on an arm64 or x86-64 build host; target
# libraries come from Debian's armhf multiarch packages, matching Raspberry Pi
# OS / Debian trixie armhf. The compiler is Clang 21 from apt.llvm.org.
FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/trixie/ llvm-toolchain-trixie-21 main" \
       > /etc/apt/sources.list.d/llvm-21.list \
    && dpkg --add-architecture armhf && apt-get update && apt-get install -y --no-install-recommends \
      git python3 python3-yaml pkg-config cmake ninja-build file \
      clang-21 lld-21 llvm-21 \
      libc6-dev:armhf libstdc++-14-dev:armhf libgcc-14-dev:armhf \
      libvulkan-dev:armhf libx11-dev:armhf libxext-dev:armhf libxrandr-dev:armhf \
      libxcursor-dev:armhf libxi-dev:armhf libxss-dev:armhf libxtst-dev:armhf \
      libxfixes-dev:armhf libx11-xcb-dev:armhf libxcb1-dev:armhf libwayland-dev:armhf \
      libxkbcommon-dev:armhf libdrm-dev:armhf libgbm-dev:armhf libegl-dev:armhf \
      libasound2-dev:armhf libpulse-dev:armhf libudev-dev:armhf libdbus-1-dev:armhf \
      wayland-protocols libwayland-bin \
    && ln -sf /usr/bin/clang-21 /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-21 /usr/local/bin/clang++ \
    && ln -sf /usr/bin/ld.lld-21 /usr/local/bin/ld.lld \
    && rm -rf /var/lib/apt/lists/*
