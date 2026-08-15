# One-off migration script (Slice 2): split editor/ into state/, exporting/, keyframe/, editor/host/.
$ErrorActionPreference = 'Stop'
$root = "D:\Projects\Source\Repos\Playback\src\playback"

$files = Get-ChildItem -Path $root -Recurse -File | Where-Object { $_.Extension -in '.h','.cpp','.hpp' }
$changed = 0
foreach ($f in $files) {
    $content = Get-Content -LiteralPath $f.FullName -Raw
    $orig = $content

    # 1. namespace declarations (global, longest-first)
    $content = $content.Replace('namespace playback::editor::editing::model', 'namespace playback::state::editing::model')
    $content = $content.Replace('namespace playback::editor::editing::command', 'namespace playback::state::editing::command')
    $content = $content.Replace('namespace playback::editor::editing', 'namespace playback::state::editing')
    $content = $content.Replace('namespace playback::editor::exporting', 'namespace playback::exporting')
    $content = $content.Replace('namespace playback::editor::keyframe', 'namespace playback::keyframe')
    $content = $content.Replace('namespace playback::editor::renderer', 'namespace playback::editor::host')

    # 2. qualified reference rules
    $content = $content.Replace('playback::editor::editing::model::', 'playback::state::editing::model::')
    $content = $content.Replace('playback::editor::editing::command::', 'playback::state::editing::command::')
    $content = $content.Replace('playback::editor::editing::', 'playback::state::editing::')
    $content = $content.Replace('playback::editor::exporting::', 'playback::exporting::')
    $content = $content.Replace('playback::editor::keyframe::', 'playback::keyframe::')
    $content = $content.Replace('playback::editor::renderer::', 'playback::editor::host::')
    $content = $content.Replace('editor::editing::model::', 'state::editing::model::')
    $content = $content.Replace('editor::editing::command::', 'state::editing::command::')
    $content = $content.Replace('editor::editing::', 'state::editing::')
    $content = $content.Replace('editor::exporting::', 'exporting::')
    $content = $content.Replace('editor::keyframe::', 'keyframe::')
    $content = $content.Replace('editor::renderer::', 'editor::host::')

    # 3. state types (fully-qualified)
    $content = $content.Replace('playback::editor::EditorActionType', 'playback::state::EditorActionType')
    $content = $content.Replace('playback::editor::EditorAction', 'playback::state::EditorAction')
    $content = $content.Replace('playback::editor::EditorState', 'playback::state::EditorState')
    $content = $content.Replace('playback::editor::EditorContext', 'playback::state::EditorContext')
    $content = $content.Replace('playback::editor::EditorController', 'playback::state::EditorController')
    $content = $content.Replace('playback::editor::ReplayBrowserState', 'playback::state::ReplayBrowserState')
    $content = $content.Replace('editor::EditorActionType', 'state::EditorActionType')
    $content = $content.Replace('editor::EditorAction', 'state::EditorAction')
    $content = $content.Replace('editor::EditorState', 'state::EditorState')
    $content = $content.Replace('editor::EditorContext', 'state::EditorContext')
    $content = $content.Replace('editor::EditorController', 'state::EditorController')
    $content = $content.Replace('editor::ReplayBrowserState', 'state::ReplayBrowserState')

    # 4. ImGuiRenderer.h fwd-decl block for EditorContext
    $content = $content.Replace("namespace playback::editor {`r`nclass EditorContext;", "namespace playback::state {`r`nclass EditorContext;")
    $content = $content.Replace("namespace playback::editor {`nclass EditorContext;", "namespace playback::state {`nclass EditorContext;")

    # 5. bare editing:: -> state::editing:: (protected)
    $content = $content.Replace('editing::model::', 'state::editing::model::')
    $content = $content.Replace('editing::command::', 'state::editing::command::')
    $content = $content.Replace('state::editing::', '__PB_STATE_EDITING__')
    $content = $content.Replace('editing::', 'state::editing::')
    $content = $content.Replace('__PB_STATE_EDITING__', 'state::editing::')

    # 6. bare renderer:: -> editor::host::
    $content = $content.Replace('renderer::', 'editor::host::')

    # 7. include path mapping
    $content = $content.Replace('playback/editor/context/', 'playback/state/')
    $content = $content.Replace('playback/editor/controller/', 'playback/state/')
    $content = $content.Replace('playback/editor/editing/', 'playback/state/editing/')
    $content = $content.Replace('playback/editor/exporting/', 'playback/exporting/')
    $content = $content.Replace('playback/editor/keyframe/', 'playback/keyframe/')
    $content = $content.Replace('playback/editor/renderer/', 'playback/editor/host/')

    # 8. per-file namespace declaration for context/controller files now under state/
    $rel = $f.FullName.Substring($root.Length).Replace('\','/')
    if ($rel -match '/state/' -and $rel -notmatch '/state/editing/') {
        $content = $content.Replace('namespace playback::editor', 'namespace playback::state')
    }

    if ($content -ne $orig) {
        Set-Content -LiteralPath $f.FullName -Value $content -Encoding UTF8 -NoNewline
        $changed++
    }
}
Write-Output "Files changed: $changed"
