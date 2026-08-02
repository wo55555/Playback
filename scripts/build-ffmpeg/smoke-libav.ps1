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
$vswhereCandidates = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe",
    (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\Installer\vswhere.exe")
)
$vswhere = $vswhereCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vswhere) { Write-Host "[smoke] ERROR: vswhere not found." -ForegroundColor Red; exit 1 }
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { Write-Host "[smoke] ERROR: no Visual Studio C++ tools found." -ForegroundColor Red; exit 1 }
$vcvars64 = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars64)) { Write-Host "[smoke] ERROR: vcvars64.bat not found." -ForegroundColor Red; exit 1 }

# 静态链接：FFmpeg 各库 + 外部依赖库 + Windows 系统库。
# 注：必须 /MD（全部静态库均以 /MD 编译，引用动态 CRT 的 __imp_* 导入符号）；
#     mfuuid/strmiids 来自 FFmpeg 检测 mediafoundation 时记录的 EXTRALIBS-avcodec。
$libs = "avformat.lib avcodec.lib avutil.lib swscale.lib swresample.lib " +
        "x264.lib x265.lib vpx.lib opus.lib zlib.lib " +
        "ws2_32.lib bcrypt.lib secur32.lib avrt.lib user32.lib ole32.lib " +
        "mfuuid.lib strmiids.lib"

$bat = Join-Path $env:TEMP "pb-smoke-cl.bat"
@"
@echo off
call "$vcvars64" >nul 2>&1
if errorlevel 1 goto failed
cl /nologo /W3 /O2 /MD /I "$prefix\include" "$src" /Fe:"$out" /link /LIBPATH:"$prefix\lib" $libs
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

# exe 以 /MD 链接，运行依赖 VC++ 运行时 DLL（vcruntime140/msvcp140/ucrtbase）。
# 将 VS Redist 的 CRT 目录加入 PATH，保证在无 Redist 的系统上也能运行。
$redistRoot = Join-Path $vsInstall "VC\Redist\MSVC"
if (Test-Path $redistRoot) {
    $crtDirs = Get-ChildItem $redistRoot -Directory | Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
        Where-Object { Test-Path $_ }
    if ($crtDirs) { $env:PATH = ($crtDirs -join ";") + ";" + $env:PATH }
}

& $out $tmp
$code = $LASTEXITCODE
Remove-Item $tmp -Force -ErrorAction SilentlyContinue
Write-Host ("[smoke] exit code: " + $code)
exit $code
