# One-off migration script (Slice 3): screen/ split + command/ -> runtime/command/ + Config -> configuration/.
$ErrorActionPreference = 'Stop'
$root = "D:\Projects\Source\Repos\Playback\src\playback"

$files = Get-ChildItem -Path $root -Recurse -File | Where-Object { $_.Extension -in '.h','.cpp','.hpp' }
$changed = 0
foreach ($f in $files) {
    $content = Get-Content -LiteralPath $f.FullName -Raw
    $orig = $content

    # config namespace
    $content = $content.Replace('namespace playback::config', 'namespace playback::configuration')
    $content = $content.Replace('playback::config::', 'playback::configuration::')
    $content = $content.Replace('config::', 'configuration::')

    # ReplayBrowser -> ReplayLibrary (precise, avoid ReplayBrowserState / lang keys)
    $content = $content.Replace('class ReplayBrowser', 'class ReplayLibrary')
    $content = $content.Replace('ReplayBrowser::', 'ReplayLibrary::')
    $content = $content.Replace('screen::ReplayBrowser', 'io::ReplayLibrary')
    $content = $content.Replace('screen::ReplaySummary', 'io::ReplaySummary')
    $content = $content.Replace('screen::ReplaySort', 'io::ReplaySort')

    # IdleDetectionHooks -> exporting
    $content = $content.Replace('screen::hookIdleDetection', 'exporting::hookIdleDetection')

    # command calls in Playback.cpp (avoid ll::command::)
    $content = $content.Replace('command::registerPlaybackCommand', 'runtime::command::registerPlaybackCommand')
    $content = $content.Replace('command::registerRecordCommand', 'runtime::command::registerRecordCommand')

    # include paths
    $content = $content.Replace('playback/screen/ReplayBrowser.h', 'playback/io/ReplayLibrary.h')
    $content = $content.Replace('playback/screen/IdleDetectionHooks.h', 'playback/exporting/IdleDetectionHooks.h')
    $content = $content.Replace('playback/Config.h', 'playback/configuration/Config.h')
    $content = $content.Replace('playback/command/', 'playback/runtime/command/')
    $content = $content.Replace('playback/state/editing/EventBus.h', 'playback/utils/event/EventBus.h')

    # per-file namespace declarations
    $rel = $f.FullName.Substring($root.Length).Replace('\','/')
    if ($rel -match '/io/')          { $content = $content.Replace('namespace playback::screen', 'namespace playback::io') }
    elseif ($rel -match '/exporting/'){ $content = $content.Replace('namespace playback::screen', 'namespace playback::exporting') }
    if ($rel -match '/runtime/command/') { $content = $content.Replace('namespace playback::command', 'namespace playback::runtime::command') }
    if ($rel -match '/configuration/') { $content = $content.Replace('namespace playback::config', 'namespace playback::configuration') }
    if ($rel -match '/utils/event/') { $content = $content.Replace('namespace playback::state::editing', 'namespace playback::utils::event') }

    if ($content -ne $orig) {
        Set-Content -LiteralPath $f.FullName -Value $content -Encoding UTF8 -NoNewline
        $changed++
    }
}
Write-Output "Files changed: $changed"
