#!/usr/bin/env bash
# build-ffmpeg.sh — 在 MSYS2 内、MSVC 工具链下构建静态 FFmpeg 8.1.2（GPL，软编全量）。
#
# 用法: build-ffmpeg.sh <prefix> <jobs> <ffmpeg-source-dir>
#   必须在继承 VS x64 环境变量的 MSYS2 bash 中运行（由 build-ffmpeg.ps1 启动）。
#   依赖构建见 build-deps.sh。
set -euo pipefail

PREFIX="${1:?usage: build-ffmpeg.sh <prefix> <jobs> <ffmpeg-source-dir>}"
JOBS="${2:-$(nproc)}"
FFSRC="${3:?ffmpeg source dir required}"

echo "[ffmpeg] prefix=$PREFIX jobs=$JOBS src=$FFSRC"
command -v cl >/dev/null 2>&1 || { echo "[ffmpeg] ERROR: cl.exe not found. Run from VS x64 Native Tools environment."; exit 1; }
command -v nasm >/dev/null 2>&1 || { echo "[ffmpeg] ERROR: nasm not found."; exit 1; }

cd "$FFSRC"

# 固定、可复现、GPL 静态配置。库依赖直接链接（禁用 pkg-config），
# 各静态库名已由 build-deps.sh 统一为 x264.lib / x265.lib / vpx.lib / opus.lib / zlib.lib。
./configure \
    --prefix="$PREFIX" \
    --target-os=win64 --arch=x86_64 --toolchain=msvc \
    --enable-static --disable-shared --enable-pic \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-postproc --disable-network \
    --pkg-config=false \
    --enable-gpl \
    --enable-libx264 --enable-libx265 --enable-libvpx --enable-libopus --enable-zlib \
    --enable-swscale --enable-swresample \
    --enable-avformat --enable-avcodec --enable-avutil \
    --enable-encoder=aac,alac,apng,bmp,ffv1,ffvhuff,flac,gif,libopus,libvpx_vp8,libvpx_vp9,libx264,libx265,mjpeg,mp3,pcm_f32le,pcm_s16le,pcm_s24le,png,prores,rawvideo,tiff,vorbis,webp \
    --enable-decoder=aac,alac,apng,bmp,flac,gif,h264,hevc,jpeg2000,mjpeg,mp3,opus,pcm_f32le,pcm_s16le,pcm_s24le,png,prores,rawvideo,vorbis,vp8,vp9,webp \
    --enable-muxer=apng,avi,flac,gif,image2,image2pipe,matroska,mjpeg,mov,mp4,ogg,png,wav,webm,webp \
    --enable-demuxer=apng,avi,flac,gif,image2,image2pipe,matroska,mjpeg,mov,mp3,ogg,png,wav,webm,webp \
    --enable-protocol=file \
    --extra-cflags="-I$PREFIX/include" \
    --extra-ldflags="-LIBPATH:$PREFIX/lib" \
    --extra-libs="x264.lib x265.lib vpx.lib opus.lib zlib.lib ws2_32.lib bcrypt.lib secur32.lib avrt.lib user32.lib ole32.lib"

make -j"$JOBS"
make install

echo "[ffmpeg] done. Installed to $PREFIX"
