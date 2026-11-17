# PuebloCTR's build environment: one image for the local builds and for CI (the owner's choice 2026-11-17: what was
# tested is what is released). devkitPro's image pinned (devkitARM r68, libctru 2.7.0, citro3d 1.7.1, picasso 2.7.2,
# 3dstools 1.3.1), LLVM 22 (the game for the 3DS and the Linux build that plans the link), the armhf cross libraries
# (the Linux build), cmake / ninja / python3; for the .cia: makerom and bannertool built from source.
FROM devkitpro/devkitarm:20260610

ARG LLVM=22
# the exact LLVM the console builds were tested with: apt.llvm.org serves only its latest 22.x, so a newer one stops the
# image here (test the new one on the console, then raise this)
ARG LLVM_VERSION=22.1.8
RUN apt-get update && apt-get install -y --no-install-recommends wget gnupg ca-certificates lsb-release \
 && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-${LLVM} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends \
      clang-${LLVM} lld-${LLVM} llvm-${LLVM} \
      cmake ninja-build python3 git make g++ \
      libc6-dev-armhf-cross libstdc++-12-dev-armhf-cross libgcc-12-dev-armhf-cross binutils-arm-linux-gnueabihf \
 && for t in clang clang++ ld.lld llvm-objcopy llvm-nm llvm-readelf llvm-ar; do \
      ln -sf /usr/bin/$t-${LLVM} /usr/local/bin/$t; done \
 && { clang --version | grep -q "clang version ${LLVM_VERSION} " \
      || { echo "LLVM is $(clang --version | head -1), the tested one is ${LLVM_VERSION}"; exit 1; }; } \
 && rm -rf /var/lib/apt/lists/*

# the .cia tools (Project_CTR makerom 0.19.0, 3ds-bannertool 1.2.3), from source
RUN git clone -q --depth 1 --branch makerom-v0.19.0 https://github.com/3DSGuy/Project_CTR.git /tmp/ctr \
 && make -C /tmp/ctr/makerom deps -j"$(nproc)" >/dev/null && make -C /tmp/ctr/makerom -j"$(nproc)" >/dev/null \
 && install -m 755 /tmp/ctr/makerom/bin/makerom /usr/local/bin/makerom \
 && git clone -q --depth 1 --branch 1.2.3 https://github.com/carstene1ns/3ds-bannertool.git /tmp/bt \
 && cmake -S /tmp/bt -B /tmp/bt/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ >/dev/null \
 && cmake --build /tmp/bt/build -j"$(nproc)" >/dev/null \
 && install -m 755 /tmp/bt/build/bannertool /usr/local/bin/bannertool \
 && rm -rf /tmp/ctr /tmp/bt

ENV DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
WORKDIR /src
