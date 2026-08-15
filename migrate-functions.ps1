# One-off migration script (Slice 1): flatten src/playback/functions/* into top-level domain dirs.
# Maps include paths and namespaces deterministically. Safe because git tree is clean (revertible).
$ErrorActionPreference = 'Stop'
$root = "D:\Projects\Source\Repos\Playback\src\playback"

$symbolMap = [ordered]@{
    'functions::ReplaySession'                  = 'replay::ReplaySession'
    'functions::Recorder'                       = 'record::Recorder'
    'functions::ReplayExporter'                 = 'record::ReplayExporter'
    'functions::PlaybackMeta'                   = 'record::PlaybackMeta'
    'functions::PlaybackView'                   = 'record::PlaybackView'
    'functions::PlaybackChunkMeta'              = 'record::PlaybackChunkMeta'
    'functions::ChunkMutationBarrier'           = 'record::ChunkMutationBarrier'
    'functions::hookNetwork'                    = 'record::hookNetwork'
    'functions::hookClientTick'                 = 'runtime::hookClientTick'
    'functions::beginOfflineReplayTickGate'     = 'runtime::beginOfflineReplayTickGate'
    'functions::endOfflineReplayTickGate'       = 'runtime::endOfflineReplayTickGate'
    'functions::wasOfflineReplayTickCompleted'  = 'runtime::wasOfflineReplayTickCompleted'
    'functions::getOfflineReplayTickCompletion' = 'runtime::getOfflineReplayTickCompletion'
    'functions::requestOfflineReplayTick'       = 'runtime::requestOfflineReplayTick'
    'functions::OfflineReplayTickToken'         = 'runtime::OfflineReplayTickToken'
    'functions::OfflineReplayTickCompletion'    = 'runtime::OfflineReplayTickCompletion'
    'functions::OfflineReplayTickRequestResult' = 'runtime::OfflineReplayTickRequestResult'
    'functions::ReplayExportTickState'          = 'replay::ReplayExportTickState'
    'functions::ReplayExportTimelinePhase'      = 'replay::ReplayExportTimelinePhase'
    'functions::ReplayCameraViewpoint'          = 'replay::ReplayCameraViewpoint'
    'functions::ReplaySceneReadiness'           = 'replay::ReplaySceneReadiness'
    'functions::ActionRegistry'                 = 'action::ActionRegistry'
    'functions::ActionNextTick'                 = 'action::ActionNextTick'
    'functions::ActionSnapshotContext'          = 'action::ActionSnapshotContext'
    'functions::ActionCreateLocalPlayer'        = 'action::ActionCreateLocalPlayer'
    'functions::ActionLevelChunkCached'         = 'action::ActionLevelChunkCached'
    'functions::ActionSubChunkCached'           = 'action::ActionSubChunkCached'
    'functions::ActionConfigurationPacket'      = 'action::ActionConfigurationPacket'
    'functions::ActionGamePacket'               = 'action::ActionGamePacket'
    'functions::ActionMoveEntities'             = 'action::ActionMoveEntities'
    'functions::PacketLifecycle'                = 'packet::PacketLifecycle'
    'functions::describePacketLifecycle'        = 'packet::describePacketLifecycle'
    'functions::PlaybackBuffer'                 = 'io::PlaybackBuffer'
    'functions::PlaybackSerializedGamePacket'   = 'io::PlaybackSerializedGamePacket'
    'functions::PlaybackSnapshotContext'        = 'io::PlaybackSnapshotContext'
    'functions::CachedChunkPacket'              = 'io::CachedChunkPacket'
    'functions::ReplayReader'                   = 'io::ReplayReader'
    'functions::ReplayWriter'                   = 'io::ReplayWriter'
    'functions::AsyncReplaySaver'               = 'io::AsyncReplaySaver'
    'functions::writeSnapshotContext'           = 'io::writeSnapshotContext'
    'functions::readSnapshotContext'            = 'io::readSnapshotContext'
    'functions::Action'                         = 'action::Action'
}

$files = Get-ChildItem -Path $root -Recurse -File | Where-Object { $_.Extension -in '.h','.cpp','.hpp' }
$changed = 0
foreach ($f in $files) {
    $content = Get-Content -LiteralPath $f.FullName -Raw
    $orig = $content

    # 1. namespace declaration for render -> visuals
    $content = $content.Replace('namespace playback::functions::render', 'namespace playback::visuals')
    # 2. fully-qualified render references
    $content = $content.Replace('playback::functions::render::', 'playback::visuals::')
    # 3. strip playback:: prefix on fully-qualified functions:: refs (symbol map handles the rest)
    $content = $content.Replace('playback::functions::', 'functions::')
    # 4. functions::render:: -> visuals::
    $content = $content.Replace('functions::render::', 'visuals::')
    # 5. bare render:: -> visuals:: (verified: only moved replay/record files)
    $content = $content.Replace('render::', 'visuals::')
    # 6. symbol map functions::X -> module::X
    foreach ($k in $symbolMap.Keys) {
        $content = $content.Replace($k, $symbolMap[$k])
    }
    # 7. include path mapping
    $content = $content.Replace('playback/functions/action/', 'playback/action/')
    $content = $content.Replace('playback/functions/io/', 'playback/io/')
    $content = $content.Replace('playback/functions/packet/', 'playback/packet/')
    $content = $content.Replace('playback/functions/record/', 'playback/record/')
    $content = $content.Replace('playback/functions/render/', 'playback/visuals/')
    $content = $content.Replace('playback/functions/replay/', 'playback/replay/')
    $content = $content.Replace('playback/functions/tick/', 'playback/runtime/')
    # 8. namespace declaration per module directory
    $rel = $f.FullName.Substring($root.Length).Replace('\','/')
    if ($rel -match '/action/')   { $content = $content.Replace('namespace playback::functions', 'namespace playback::action') }
    elseif ($rel -match '/io/')   { $content = $content.Replace('namespace playback::functions', 'namespace playback::io') }
    elseif ($rel -match '/packet/'){ $content = $content.Replace('namespace playback::functions', 'namespace playback::packet') }
    elseif ($rel -match '/record/'){ $content = $content.Replace('namespace playback::functions', 'namespace playback::record') }
    elseif ($rel -match '/replay/'){ $content = $content.Replace('namespace playback::functions', 'namespace playback::replay') }
    elseif ($rel -match '/runtime/'){ $content = $content.Replace('namespace playback::functions', 'namespace playback::runtime') }
    elseif ($rel -match '/visuals/'){ $content = $content.Replace('namespace playback::functions', 'namespace playback::visuals') }
    # 9. editor/exporting forward-declaration blocks (ReplaySession)
    elseif ($rel -match '/editor/exporting/') { $content = $content.Replace('namespace playback::functions', 'namespace playback::replay') }

    if ($content -ne $orig) {
        Set-Content -LiteralPath $f.FullName -Value $content -Encoding UTF8 -NoNewline
        $changed++
    }
}
Write-Output "Files changed: $changed"
