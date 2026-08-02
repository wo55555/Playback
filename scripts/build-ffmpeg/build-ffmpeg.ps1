# build-ffmpeg.ps1 — Playback 内置静态 FFmpeg 的一键构建入口（步骤 1 的执行脚本）。
#
# 功能：
#   1. 检查并定位 MSYS2 与 Visual Studio（MSVC 工具链）
#   2. 按固定版本下载/检出源码（见 versions.txt）
#   3. 在 VS 环境 + MSYS2 bash 中构建 x264/x265/libvpx/libopus/zlib（build-deps.sh）
#   4. 构建静态 FFmpeg 8.1.2（build-ffmpeg.sh，GPL 软编全量）
#   5. 收集产物到 third_party/ffmpeg，生成 ffmpeg-build-manifest.json
#   6. 拷贝各库许可证到 licenses/ffmpeg
#
# 前置条件：
#   - 已安装 MSYS2（https://www.msys2.org/），并已安装：pacman -S --needed nasm make pkg-config cmake git
#     （cmake 若已在 Windows PATH 也可；脚本依赖 MSYS2 bash 中的 make/nasm）
#   - 已安装 Visual Studio 2022（C++ 桌面工作负载）
#
# 用法：  powershell -ExecutionPolicy Bypass -File scripts/build-ffmpeg/build-ffmpeg.ps1
# 构建后：xmake f --playback_ffmpeg=y && xmake build playback

param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$Msys2Root = "C:\msys64",
    [string]$Prefix = "",                      # 产物目录，默认 <repo>/third_party/ffmpeg
    [string]$SrcDir = "",                      # 源码目录，默认 <repo>/third_party/ffmpeg-src
    [int]$Jobs = 0,                            # 0 = 自动（逻辑核数）
    [string]$VsTarget = "vs17",                # libvpx 目标：vs16=VS2019 / vs17=VS2022
    [string]$FfmpegVersion = "8.1.2",
    [string]$X264Commit = "",                  # 空 = 使用远端 HEAD 并把实际 commit 写入 manifest
    [string]$X265Tag = "3.6",
    [string]$LibvpxTag = "v1.15.0",
    [string]$LibopusTag = "v1.5.2",
    [string]$ZlibTag = "v1.3.1",
    [switch]$SkipDownload
)

$ErrorActionPreference = "Stop"

if (-not $Prefix) { $Prefix = Join-Path $RepoRoot "third_party\ffmpeg" }
if (-not $SrcDir) { $SrcDir  = Join-Path $RepoRoot "third_party\ffmpeg-src" }
if ($Jobs -le 0)  { $Jobs = [Environment]::ProcessorCount }

$msysBash  = Join-Path $Msys2Root "usr\bin\bash.exe"
$git       = (Get-Command git -ErrorAction SilentlyContinue).Source
$curl      = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source

# ---------- 1. 环境检查 ----------
function Require-Tool {
    param([string]$Path, [string]$Hint)
    if (-not $Path -or -not (Test-Path $Path)) {
        Write-Host "[ffmpeg] ERROR: $Hint" -ForegroundColor Red
        exit 1
    }
}

Require-Tool $msysBash "MSYS2 not found at $Msys2Root. Install from https://www.msys2.org/ (default path C:\msys64)."
Require-Tool $git "git not found on PATH."
if (-not $curl) { Write-Host "[ffmpeg] WARN: curl.exe not found, falling back to Invoke-WebRequest." }

# 定位 Visual Studio 的 vcvars64.bat
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Host "[ffmpeg] ERROR: vswhere not found. Install Visual Studio 2022 with C++ desktop workload." -ForegroundColor Red
    exit 1
}
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    Write-Host "[ffmpeg] ERROR: no Visual Studio with C++ tools found." -ForegroundColor Red
    exit 1
}
$vcvars64 = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
Require-Tool $vcvars64 "vcvars64.bat not found at $vcvars64."

# ---------- 2. 源码下载 / 检出 ----------
New-Item -ItemType Directory -Force -Path $SrcDir | Out-Null
$work = @()

function To-MsysPath {
    param([string]$WindowsPath)
    return ($WindowsPath -replace '^([A-Za-z]):', '/$1' -replace '\\', '/')
}

function Get-Source {
    param([string]$Name, [string]$Url, [string]$Dest, [string]$ExpectedSha256 = "")
    $destPath = Join-Path $SrcDir $Dest
    if (Test-Path $destPath) {
        Write-Host "[ffmpeg] source '$Name' already present at $destPath, skipping."
        return
    }
    if ($SkipDownload) { Write-Host "[ffmpeg] ERROR: '$Name' missing and -SkipDownload set." -ForegroundColor Red; exit 1 }

    Write-Host "[ffmpeg] fetching $Name <- $Url"
    $tarball = Join-Path $SrcDir "$Name.tar.gz"
    if ($curl) {
        & $curl -L -sS --ssl-no-revoke --connect-timeout 30 --retry 3 --retry-delay 5 -o $tarball $Url
        if ($LASTEXITCODE -ne 0) { Write-Host "[ffmpeg] ERROR: download failed: $Url" -ForegroundColor Red; exit 1 }
    } else {
        Invoke-WebRequest -Uri $Url -OutFile $tarball
    }
    if ($ExpectedSha256) {
        $actual = (Get-FileHash $tarball -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $ExpectedSha256.ToLowerInvariant()) {
            Write-Host "[ffmpeg] ERROR: SHA256 mismatch for $Name. expected=$ExpectedSha256 actual=$actual" -ForegroundColor Red
            exit 1
        }
    }
    tar -xf $tarball -C $SrcDir
    if ($LASTEXITCODE -ne 0) { Write-Host "[ffmpeg] ERROR: extraction failed for $tarball" -ForegroundColor Red; exit 1 }
    Remove-Item $tarball -Force
}

function Get-GitSource {
    param([string]$Name, [string]$Url, [string]$Ref, [string]$Dest, [string]$PinCommit = "")
    $destPath = Join-Path $SrcDir $Dest
    if (Test-Path $destPath) {
        Write-Host "[ffmpeg] source '$Name' already present at $destPath, skipping."
        return
    }
    if ($SkipDownload) { Write-Host "[ffmpeg] ERROR: '$Name' missing and -SkipDownload set." -ForegroundColor Red; exit 1 }
    Write-Host "[ffmpeg] cloning $Name <- $Url ($Ref)"
    if ($PinCommit) {
        & $git clone --filter=blob:none --no-checkout $Url $destPath | Out-Null
        Push-Location $destPath
        try { & $git checkout $PinCommit | Out-Null } finally { Pop-Location }
    } else {
        & $git clone --depth 1 -b $Ref $Url $destPath | Out-Null
    }
    if ($LASTEXITCODE -ne 0) { Write-Host "[ffmpeg] ERROR: git clone failed: $Url" -ForegroundColor Red; exit 1 }
}

# FFmpeg（固定 release tarball，优先 GitHub 镜像；ffmpeg.org 有时连接慢）
$ffmpegSrc = Join-Path $SrcDir "ffmpeg-$FfmpegVersion"
Get-Source -Name "ffmpeg-$FfmpegVersion" `
    -Url "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n$FfmpegVersion.tar.gz" `
    -Dest "FFmpeg-n$FfmpegVersion" -ExpectedSha256 ""
if (-not (Test-Path $ffmpegSrc) -and (Test-Path (Join-Path $SrcDir "FFmpeg-n$FfmpegVersion"))) {
    Move-Item (Join-Path $SrcDir "FFmpeg-n$FfmpegVersion") $ffmpegSrc
}

# x264（git，固定 commit 或 HEAD；code.videolan.org 有时慢，用 GitHub 镜像）
Get-GitSource -Name "x264" -Url "https://github.com/videolan/x264.git" `
    -Ref "master" -Dest "x264" -PinCommit $X264Commit
# x265（release tarball）
Get-Source -Name "x265" -Url "https://github.com/videolan/x265/archive/refs/tags/$X265Tag.tar.gz" `
    -Dest "x265-$X265Tag" -ExpectedSha256 ""
if (-not (Test-Path (Join-Path $SrcDir "x265"))) {
    Move-Item (Join-Path $SrcDir "x265-$X265Tag") (Join-Path $SrcDir "x265")
}

# libvpx / libopus / zlib（git tag）
Get-GitSource -Name "libvpx" -Url "https://github.com/webmproject/libvpx.git" `
    -Ref $LibvpxTag -Dest "libvpx"
Get-GitSource -Name "libopus" -Url "https://github.com/xiph/opus.git" `
    -Ref $LibopusTag -Dest "opus"
Get-GitSource -Name "zlib" -Url "https://github.com/madler/zlib.git" `
    -Ref $ZlibTag -Dest "zlib"

# ---------- 3. 构建（VS 环境 + MSYS2 bash） ----------
$srcMsys      = To-MsysPath $SrcDir
$prefixMsys   = To-MsysPath $Prefix
$ffmpegSrcMsys = To-MsysPath $ffmpegSrc

New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Prefix "include") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Prefix "lib") | Out-Null

$inner = "cd '$srcMsys' && bash build-deps.sh '$prefixMsys' '$Jobs' '$VsTarget' && bash build-ffmpeg.sh '$prefixMsys' '$Jobs' '$ffmpegSrcMsys'"
$cmdLine = "call `"$vcvars64`" >nul && set MSYS2_PATH_TYPE=inherit && `"$msysBash`" -lc `"$inner`""
Write-Host "[ffmpeg] running build inside MSYS2 (VS environment):"
Write-Host "  $cmdLine"

Push-Location (Join-Path $RepoRoot "scripts\build-ffmpeg")
try {
    cmd.exe /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ffmpeg] ERROR: build failed with exit code $LASTEXITCODE" -ForegroundColor Red
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

# ---------- 4. 收集产物 + manifest ----------
function Assert-Lib {
    param([string]$Name)
    $lib = Join-Path $Prefix "lib\$Name"
    if (-not (Test-Path $lib)) { Write-Host "[ffmpeg] ERROR: missing $lib" -ForegroundColor Red; exit 1 }
}

@("avcodec.lib","avformat.lib","avutil.lib","swscale.lib","swresample.lib",
  "x264.lib","x265.lib","vpx.lib","opus.lib","zlib.lib") | ForEach-Object { Assert-Lib $_ }
if (-not (Test-Path (Join-Path $Prefix "include\libavcodec\avcodec.h"))) {
    Write-Host "[ffmpeg] ERROR: headers not installed (include\libavcodec\avcodec.h missing)" -ForegroundColor Red; exit 1
}

function Get-Commit {
    param([string]$Dir, [string]$Name)
    if (-not (Test-Path (Join-Path $Dir ".git"))) { return "n/a" }
    Push-Location $Dir
    try { return (& $git rev-parse HEAD).Trim() } finally { Pop-Location }
}

$x264Commit = Get-Commit (Join-Path $SrcDir "x264") "x264"
$x265Commit = Get-Commit (Join-Path $SrcDir "x265") "x265"
$vpxCommit  = Get-Commit (Join-Path $SrcDir "libvpx") "libvpx"
$opusCommit = Get-Commit (Join-Path $SrcDir "opus") "libopus"
$zlibCommit = Get-Commit (Join-Path $SrcDir "zlib") "zlib"

$manifest = @{
    project   = "Playback"
    generated = (Get-Date -Format "yyyy-MM-ddTHH:mm:sszzz")
    license   = "GPL (static link)"
    toolchain = @{
        compiler = "MSVC"
        vcvars64 = $vcvars64
        target   = "x86_64-win64-$VsTarget"
        jobs     = $Jobs
    }
    ffmpeg    = @{
        version  = $FfmpegVersion
        url      = "https://ffmpeg.org/releases/ffmpeg-$FfmpegVersion.tar.xz"
        sha256   = (Get-FileHash (Join-Path $SrcDir "ffmpeg-$FfmpegVersion.tar.xz") -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash.ToLowerInvariant()
        configure = "./configure --prefix=$prefixMsys --target-os=win64 --arch=x86_64 --toolchain=msvc --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-network --enable-gpl --enable-libx264 --enable-libx265 --enable-libvpx --enable-libopus --enable-zlib --enable-swscale --enable-swresample"
    }
    x264      = @{ commit = $x264Commit; url = "https://code.videolan.org/videolan/x264.git"; license = "GPL-2.0-or-later" }
    x265      = @{ tag = $X265Tag; commit = $x265Commit; url = "https://github.com/videolan/x265"; license = "GPL-2.0-or-later" }
    libvpx    = @{ tag = $LibvpxTag; commit = $vpxCommit; url = "https://github.com/webmproject/libvpx"; license = "BSD-3-Clause" }
    libopus   = @{ tag = $LibopusTag; commit = $opusCommit; url = "https://github.com/xiph/opus"; license = "BSD-3-Clause" }
    zlib      = @{ tag = $ZlibTag; commit = $zlibCommit; url = "https://github.com/madler/zlib"; license = "Zlib" }
}
$manifestPath = Join-Path $Prefix "ffmpeg-build-manifest.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Path $manifestPath -Encoding UTF8
Write-Host "[ffmpeg] manifest written to $manifestPath"

# ---------- 5. 许可证随附 ----------
$licenseDir = Join-Path $RepoRoot "licenses\ffmpeg"
New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
$licenseCopies = @(
    @{ Src = (Join-Path $ffmpegSrc "COPYING");                 Dst = "FFmpeg-COPYING.txt" },
    @{ Src = (Join-Path $SrcDir "x264\COPYING");               Dst = "x264-COPYING.txt" },
    @{ Src = (Join-Path $SrcDir "x265\COPYING");               Dst = "x265-COPYING.txt" },
    @{ Src = (Join-Path $SrcDir "libvpx\LICENSE");             Dst = "libvpx-LICENSE.txt" },
    @{ Src = (Join-Path $SrcDir "opus\COPYING");               Dst = "libopus-COPYING.txt" },
    @{ Src = (Join-Path $SrcDir "zlib\LICENSE");               Dst = "zlib-LICENSE.txt" }
)
foreach ($c in $licenseCopies) {
    if (Test-Path $c.Src) {
        Copy-Item $c.Src (Join-Path $licenseDir $c.Dst) -Force
        Write-Host "[ffmpeg] license copied: $($c.Dst)"
    } else {
        Write-Host "[ffmpeg] WARN: license source not found: $($c.Src)" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "[ffmpeg] BUILD OK" -ForegroundColor Green
Write-Host "[ffmpeg] products : $Prefix"
Write-Host "[ffmpeg] licenses: $licenseDir"
Write-Host "[ffmpeg] next    : xmake f --playback_ffmpeg=y && xmake build playback"
