# smoke-libav.ps1 — 编译并运行 smoke_libav.c（最小 libav 静态链接冒烟测试）。
#
# 前置：third_party/ffmpeg 已由 build-ffmpeg.ps1 构建完成（含 include/lib 全套产物）。
# 用法：  powershell -ExecutionPolicy Bypass -File scripts/build-ffmpeg/smoke-libav.ps1
# 返回：  0 = 全部通过；非 0 = 失败。

param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
)

$ErrorActionPreference = "Stop"

$prefix = Join-Path $RepoRoot "third_party\ffmpeg"
$src    = Join-Path $PSScriptRoot "smoke_libav.c"
$out    = Join-Path $PSScriptRoot "smoke_libav.exe"
$tmp    = Join-Path $env:TEMP "pb-smoke.mp4"

if (-not (Test-Path (Join-Path $prefix "lib\avformat.lib"))) {
    Write-Host "[smoke] ERROR: third_party\ffmpeg not built. Run build-ffmpeg.ps1 first." -ForegroundColor Red
    exit 1
}

# 定位 VS 的 vcvars64.bat（与 build-ffmpeg.ps1 相同逻辑）
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Host "[smoke] ERROR: vswhere not found." -ForegroundColor Red; exit 1 }
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { Write-Host "[smoke] ERROR: no Visual Studio C++ tools found." -ForegroundColor Red; exit 1 }
$vcvars64 = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars64)) { Write-Host "[smoke] ERROR: vcvars64.bat not found." -ForegroundColor Red; exit 1 }

# 静态链接：FFmpeg 各库 + 外部依赖库 + Windows 系统库（与 build-ffmpeg.sh --extra-libs 一致）。
$libs = "avformat.lib avcodec.lib avutil.lib swscale.lib swresample.lib " +
        "x264.lib x265.lib vpx.lib opus.lib zlib.lib " +
        "ws2_32.lib bcrypt.lib secur32.lib avrt.lib user32.lib ole32.lib"

$bat = Join-Path $env:TEMP "pb-smoke-cl.bat"
@"
@echo off
call "$vcvars64" >nul 2>&1
if errorlevel 1 goto failed
cl /nologo /W3 /O2 /I "$prefix\include" "$src" /Fe:"$out" /link /LIBPATH:"$prefix\lib" $libs
exit /b %ERRORLEVEL%
:failed
echo [smoke] ERROR: vcvars64.bat failed
exit /b 1
"@ | Set-Content -Path $bat -Encoding ASCII

try {
    & $bat
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[smoke] compile/link failed with exit code $LASTEXITCODE" -ForegroundColor Red
        exit $LASTEXITCODE
    }
} finally {
    Remove-Item $bat -Force -ErrorAction SilentlyContinue
}

& $out $tmp
$code = $LASTEXITCODE
Remove-Item $out, $tmp -Force -ErrorAction SilentlyContinue
Write-Host ("[smoke] exit code: " + $code)
exit $code
