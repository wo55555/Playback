#!/usr/bin/env bash
# build-deps.sh — 在 MSYS2 内、MSVC 工具链下构建 FFmpeg 的外部依赖（静态库）。
#
# 用法: build-deps.sh <prefix> <jobs> <vs-target> <src-dir>
#   必须在继承 VS x64 环境变量的 MSYS2 bash 中运行（由 build-ffmpeg.ps1 启动）。
#   产物: $PREFIX/{include,lib,pkgconfig} 中的 x264/x265/libvpx/libopus/zlib 静态库。
set -euo pipefail

PREFIX="${1:?usage: build-deps.sh <prefix> <jobs> <vs-target> <src-dir> <cmake-exe> <ninja-exe>}"
JOBS="${2:-$(nproc)}"
VS_TARGET="${3:-vs17}"
SRC_DIR="${4:?src dir required}"
CMAKE_BIN="${5:?windows cmake exe required}"
NINJA_BIN="${6:-ninja}"

echo "[deps] prefix=$PREFIX jobs=$JOBS vs-target=$VS_TARGET src=$SRC_DIR"

command -v cl  >/dev/null 2>&1 || { echo "[deps] ERROR: cl.exe not found. Run from VS x64 Native Tools environment."; exit 1; }
command -v nasm >/dev/null 2>&1 || { echo "[deps] ERROR: nasm not found. Run: pacman -S --needed nasm make pkg-config"; exit 1; }
command -v diff >/dev/null 2>&1 || { echo "[deps] ERROR: diff not found (libvpx configure 需要). Run: pacman -S --needed diffutils"; exit 1; }
command -v "$CMAKE_BIN" >/dev/null 2>&1 || { echo "[deps] ERROR: cmake not found on PATH."; exit 1; }

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
    "$CMAKE_BIN" -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=cl \
        -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DZLIB_BUILD_EXAMPLES=OFF -DZLIB_BUILD_TESTING=OFF \
        -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON ..
    "$CMAKE_BIN" --build . -j "$JOBS"
    "$CMAKE_BIN" --install .
    # 纯静态构建时只生成 zlibstatic.lib；FFmpeg 的 pkg-config 检测用 -lz -> zlib.lib，
    # 统一把静态库复制为 zlib.lib（覆盖旧导入库），避免链接到 zlib.dll 导入库造成运行时 DLL 依赖。
    if [ -f "$PREFIX/lib/zlibstatic.lib" ]; then
        cp "$PREFIX/lib/zlibstatic.lib" "$PREFIX/lib/zlib.lib"
        echo "[deps] copied zlibstatic.lib -> zlib.lib (static, no zlib.dll dep)"
    fi
    # FFmpeg 的 config.h 会以 "#define HAVE_UNISTD_H 0"（值为 0 但已定义）的形式输出，
    # 而 zconf.h 用 #ifdef（只看是否定义）判断，导致误 include <unistd.h> 使 MSVC 编译失败。
    # 改为同时检查值，消除误触发（MSVC 下 unistd.h 不存在）。
    if [ -f "$PREFIX/include/zconf.h" ]; then
        sed -i 's|^#ifdef HAVE_UNISTD_H|#if defined(HAVE_UNISTD_H) \&\& HAVE_UNISTD_H|' "$PREFIX/include/zconf.h"
        echo "[deps] patched zconf.h: HAVE_UNISTD_H 需为真值才 include <unistd.h> (MSVC compat)"
    fi
}

build_x264() {
    echo "[deps] == x264 =="
    cd "$SRC_DIR/x264"
    # x264 configure 只在 WinRT 分支添加 -MD，桌面 MSVC 默认 /MT（LIBCMT）；
    # 必须显式 -MD 与 FFmpeg/opus/x265/vpx 的 /MD（MSVCRT）运行时保持一致，否则链接时 CRT 冲突。
    CC=cl ./configure --prefix="$PREFIX" --enable-static --disable-cli \
        --disable-opencl --extra-cflags="$EXTRA_CFLAGS -MD" --extra-ldflags="$EXTRA_LDFLAGS"
    make -j"$JOBS"
    make install
    # MSVC 下 x264 安装的库名为 libx264.lib，统一为 x264.lib 供 FFmpeg 链接。
    if [ -f "$PREFIX/lib/libx264.lib" ]; then
        cp "$PREFIX/lib/libx264.lib" "$PREFIX/lib/x264.lib"
    fi
    # x264 的 configure 会把 MSYS 前缀(/D/...)原样写进 x264.pc，pkgconf 输出 -I/-L 时无法被 cl/link 识别，
    # 改写为 D:/... 形式（与 x265/opus 的 .pc 保持一致）。
    if [ -f "$PREFIX/lib/pkgconfig/x264.pc" ]; then
        sed -i "s|^prefix=.*|prefix=$(cygpath -m "$PREFIX")|" "$PREFIX/lib/pkgconfig/x264.pc"
        echo "[deps] patched x264.pc prefix -> $(cygpath -m "$PREFIX")"
    fi
}

build_x265() {
    echo "[deps] == x265 =="
    # CMake >= 4.0 已移除 CMP0025/CMP0054 的 OLD 行为，而 x265 3.6 的 CMakeLists 仍显式设置 OLD，
    # 直接导致 configure 失败。改为 NEW（在 Windows/MSVC 下行为等价，且为 CMake 官方推荐路径）。
    local xcmake="$SRC_DIR/x265/source/CMakeLists.txt"
    if grep -q "cmake_policy(SET CMP0025 OLD)" "$xcmake"; then
        sed -i 's/cmake_policy(SET CMP0025 OLD)/cmake_policy(SET CMP0025 NEW)/; s/cmake_policy(SET CMP0054 OLD)/cmake_policy(SET CMP0054 NEW)/' "$xcmake"
        echo "[deps] patched x265 CMakeLists.txt: CMP0025/CMP0054 OLD -> NEW (CMake >= 4 compat)"
    fi
    cd "$SRC_DIR/x265/source"
    rm -rf build && mkdir build && cd build
    "$CMAKE_BIN" -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_TESTS=OFF \
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl \
        -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
        -DCMAKE_ASM_NASM_COMPILER=nasm \
        -DYASM_EXECUTABLE=nasm ..
    "$CMAKE_BIN" --build . -j "$JOBS"
    "$CMAKE_BIN" --install .
    # MSVC 下静态库名为 x265-static.lib，统一为 x265.lib 供 FFmpeg 链接。
    if [ -f "$PREFIX/lib/x265-static.lib" ]; then
        cp "$PREFIX/lib/x265-static.lib" "$PREFIX/lib/x265.lib"
    fi
}

build_libvpx() {
    echo "[deps] == libvpx =="
    # libvpx 自包含，无外部依赖（已禁用 webm_io/libyuv），无需 extra-cflags/ldflags；
    # MSYS 风格路径传给 cl 时也不可转换，故一律不传。
    cd "$SRC_DIR/libvpx"
    ./configure --target="x86_64-win64-$VS_TARGET" --prefix="$PREFIX" \
        --enable-static --disable-shared \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-webm-io --disable-libyuv
    make -j"$JOBS"
    make install
    # /MD 运行库下库名为 vpxmd.lib；vs17 目标会安装到 lib/x64 子目录，两种路径都兼容，统一为 vpx.lib 供 FFmpeg 链接。
    if [ -f "$PREFIX/lib/vpxmd.lib" ]; then
        cp "$PREFIX/lib/vpxmd.lib" "$PREFIX/lib/vpx.lib"
    elif [ -f "$PREFIX/lib/x64/vpxmd.lib" ]; then
        cp "$PREFIX/lib/x64/vpxmd.lib" "$PREFIX/lib/vpx.lib"
    fi
    # vpx.pc 在 libvpx 的 make install 中可能不生成（受 config.mk 影响），FFmpeg 检测 libvpx 会退化到
    # check_lib 回退，且库模板的 -lm 在 MSVC 下会被 FFmpeg 转成 m.lib 导致链接失败，故统一生成无 -lm 的 vpx.pc。
    # prefix 使用 D:/... 形式（MSYS 的 /D/... 形式 pkgconf 输出后无法被 cl/link 识别）。
    local vpxpc="$PREFIX/lib/pkgconfig/vpx.pc"
    if [ -f "$PREFIX/include/vpx/vpx_codec.h" ]; then
        mkdir -p "$PREFIX/lib/pkgconfig"
        {
            echo "# pkg-config file from libvpx (generated by build-deps.sh)"
            echo "prefix=$(cygpath -m "$PREFIX")"
            echo "exec_prefix=\${prefix}"
            echo "libdir=\${prefix}/lib"
            echo "includedir=\${prefix}/include"
            echo ""
            echo "Name: vpx"
            echo "Description: WebM Project VPx codec implementation"
            echo "Version: 1.15.0"
            echo "Requires:"
            echo "Conflicts:"
            echo "Libs: -L\${libdir} -lvpx"
            echo "Libs.private:"
            echo "Cflags: -I\${includedir}"
        } > "$vpxpc"
        echo "[deps] generated $vpxpc (without -lm for MSVC)"
    fi
}

build_libopus() {
    echo "[deps] == libopus =="
    cd "$SRC_DIR/opus"
    rm -rf build && mkdir build && cd build
    "$CMAKE_BIN" -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=cl \
        -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_SHARED_LIBRARY=OFF ..
    "$CMAKE_BIN" --build . -j "$JOBS"
    "$CMAKE_BIN" --install .
}

build_zlib
build_x264
build_x265
build_libvpx
build_libopus

echo "[deps] done. Libraries installed to $PREFIX"
ls -1 "$PREFIX/lib"/*.lib 2>/dev/null || true
