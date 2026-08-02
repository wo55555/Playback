#!/usr/bin/env bash
# build-deps.sh — 在 MSYS2 内、MSVC 工具链下构建 FFmpeg 的外部依赖（静态库）。
#
# 用法: build-deps.sh <prefix> <jobs> [vs-target]
#   必须在继承 VS x64 环境变量的 MSYS2 bash 中运行（由 build-ffmpeg.ps1 启动）。
#   产物: $PREFIX/{include,lib,pkgconfig} 中的 x264/x265/libvpx/libopus/zlib 静态库。
set -euo pipefail

PREFIX="${1:?usage: build-deps.sh <prefix> <jobs> [vs-target]}"
JOBS="${2:-$(nproc)}"
VS_TARGET="${3:-vs17}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[deps] prefix=$PREFIX jobs=$JOBS vs-target=$VS_TARGET src=$SRC_DIR"

command -v cl  >/dev/null 2>&1 || { echo "[deps] ERROR: cl.exe not found. Run from VS x64 Native Tools environment."; exit 1; }
command -v nasm >/dev/null 2>&1 || { echo "[deps] ERROR: nasm not found. Run: pacman -S --needed nasm make pkg-config"; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "[deps] ERROR: cmake not found on PATH."; exit 1; }

# MSYS2 自带 link.exe 会与 MSVC 的 link.exe 冲突，临时移开。
if [ -f /usr/bin/link.exe ]; then
    mv /usr/bin/link.exe /usr/bin/link.exe.msys.bak
    trap 'mv /usr/bin/link.exe.msys.bak /usr/bin/link.exe 2>/dev/null || true' EXIT
fi

mkdir -p "$PREFIX"

export EXTRA_CFLAGS="-I$PREFIX/include"
export EXTRA_LDFLAGS="-LIBPATH:$PREFIX/lib"

build_zlib() {
    echo "[deps] == zlib =="
    cd "$SRC_DIR/zlib"
    rm -rf build && mkdir build && cd build
    cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DZLIB_BUILD_EXAMPLES=OFF -DZLIB_BUILD_TESTING=OFF ..
    cmake --build . --config Release
    cmake --install .
}

build_x264() {
    echo "[deps] == x264 =="
    cd "$SRC_DIR/x264"
    CC=cl ./configure --prefix="$PREFIX" --enable-static --disable-cli \
        --disable-opencl --extra-cflags="$EXTRA_CFLAGS" --extra-ldflags="$EXTRA_LDFLAGS"
    make -j"$JOBS"
    make install
}

build_x265() {
    echo "[deps] == x265 =="
    cd "$SRC_DIR/x265/source"
    rm -rf build && mkdir build && cd build
    cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_TESTS=OFF \
        -DCMAKE_ASM_NASM_COMPILER=nasm \
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl \
        -DCMAKE_ASM_COMPILER=nasm ..
    cmake --build . --config Release
    cmake --install .
    # MSVC 下静态库名为 x265-static.lib，统一为 x265.lib 供 FFmpeg 链接。
    if [ -f "$PREFIX/lib/x265-static.lib" ]; then
        cp "$PREFIX/lib/x265-static.lib" "$PREFIX/lib/x265.lib"
    fi
}

build_libvpx() {
    echo "[deps] == libvpx =="
    cd "$SRC_DIR/libvpx"
    ./configure --target="x86_64-win64-$VS_TARGET" --prefix="$PREFIX" \
        --enable-static --disable-shared \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-webm-io --disable-libyuv \
        --extra-cflags="$EXTRA_CFLAGS" --extra-ldflags="$EXTRA_LDFLAGS"
    make -j"$JOBS"
    make install
    # /MD 运行库下库名为 vpxmd.lib，统一为 vpx.lib 供 FFmpeg 链接。
    if [ -f "$PREFIX/lib/vpxmd.lib" ]; then
        cp "$PREFIX/lib/vpxmd.lib" "$PREFIX/lib/vpx.lib"
    fi
}

build_libopus() {
    echo "[deps] == libopus =="
    cd "$SRC_DIR/opus"
    rm -rf build && mkdir build && cd build
    cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_SHARED_LIBRARY=OFF \
        -DCMAKE_C_COMPILER=cl ..
    cmake --build . --config Release
    cmake --install .
}

build_zlib
build_x264
build_x265
build_libvpx
build_libopus

echo "[deps] done. Libraries installed to $PREFIX"
ls -1 "$PREFIX/lib"/*.lib 2>/dev/null || true
