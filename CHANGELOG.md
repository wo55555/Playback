# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Expanded the replay editor's undo and redo history to 256 steps, with the Edit menu displaying the next action name and a History submenu to jump across up to 20 recent undo or redo steps at once.
- Added undo and redo buttons with action tooltips to the timeline toolbar, a bottom viewport toast showing each undo or redo step, and key-repeat support when holding the undo or redo shortcuts.
- Saving an editor project now keeps a `.pbproj.old` backup of the previous file and automatically recovers from it if the primary project file fails to load.
- Added an on-demand Speed track from the timeline `+` menu for continuous speed ramps (`0.01x` to `32x`) with a logarithmic curve and shaded acceleration/deceleration regions, supporting eight interpolation modes with draggable cubic Bezier handles, real-time speed-ramped preview, integrated export frame timing, and full undo/redo and project persistence.
- Added an on-demand Freeze track that can be added and removed from the timeline `+` menu to freeze and unfreeze world time with a configurable 0–10 tick slowdown easing curve, keeping camera motion active while pausing world and entity animations in both live preview and offline export, automatically muting audio while frozen and seamlessly resuming after unfreezing, with full undo/redo and project persistence.
- Added an on-demand Timelapse track that can be added and removed from the timeline `+` menu to map replay intervals to target output timestamps via keyframes, automatically computing segment output duration and effective speed while highlighting single-keyframe or non-increasing invalid segments, with multi-format duration input and quick presets, real-time high-speed preview with audio muted during active segments, explicit priority overriding the Speed track and fixed-interval export timelapse, and full undo/redo and project persistence.
- Added on-demand multi-track Audio tracks from the timeline `+` menu to import WAV and FFmpeg-decoded external audio files via a native file picker, rendering mirrored peak waveforms mapped to output duration across speed and timelapse edits, supporting trim start offset, duration trimming, volume, mute, drag editing, real-time synchronized playback, and mixing with game audio or exporting independently into video files and sidecar WAVs (remaining available even during fixed-interval timelapse exports), with full undo/redo and project persistence.
- Added on-demand Time of Day and Weather tracks from the timeline `+` menu alongside per-scene World Environment overrides (fixed daytime override and five-mode weather override: Clear, Overcast, Rain, Snow, and Thunder) in the Preview page. The Time of Day track animates sky time across multiple day-night cycles with interpolation curves and cubic Bezier handles, while the Weather track switches discrete color-coded weather segments along the timeline; both take priority over per-scene overrides and apply consistently across biomes in paused preview, live playback, and offline export, with full undo/redo and project persistence.
- Improved timeline start/end frame navigation with two-stage `Home` / `End` jumping (stopping at export In/Out points before jumping to the start or end), `Shift+I` / `Shift+O` shortcuts, `Up` / `Down` adjacent edit-point stepping, an interactive Export In/Out capsule control in the transport bar, clickable ruler brackets with context menus, Export page and dialog quick-jump buttons, and a track-side navigation help icon.
- Added a live-target searchable subject picker directly in the camera keyframe inspector for first-person, third-person, and Look at entity (`Aim`) keyframes.
- Added an option to disable server resource packs when opening replays (available in both the Replay Browser settings menu and the editor Settings page, persisted across sessions) together with a Reload at Current Tick action to reopen the active replay in place.
- Added a Box Select tool (`B`, also triggered via `Alt+drag` or dragging across full or fixed clips in Select mode) to marquee-select keyframes and clip cuts across tracks, with `Delete` removing selected keyframes, healing split seams, and restoring cut gaps in a single undoable step.

### Changed

- Unified playback speed control into the on-demand Speed track (replacing per-clip speed controls on camera clips and updating the camera keyframe option to Ignore Speed Track), and updated the export inspector to show the actual output duration and frame count after applying the speed curve.
- Consolidated audio controls into the audio layer by removing the redundant mute toggle from camera clips, and separated game audio capture and external audio track mixing into independent export audio settings.
- Changed the `K` keyframe shortcut to insert Free Camera keyframes by default, with a Shortcut Keyframe Lens option on the Settings page to match the active preview perspective (`1P`, `3P`, or Free) instead.
- Adding a keyframe now automatically selects it and switches the right inspector to the Selection page.
- Simplified the transport bar's Export In and Out buttons to clean `[` and `]` bracket icons with balanced padding.

### Fixed

- The replay browser, editor overlay, export progress screen, and frame capture now remain visible without hanging when third-party overlays or capture tools (such as RTSS) hook, patch, or bypass DXGI swap-chain presentation and resizing.
- Held weapons and attachables (such as bows, crossbows, and tridents) now stay attached to the player's hand across skin loads and mid-replay skin swaps, remain visible alongside the player's arm in first-person follow for persona skins, and continue updating on the recorded player while following someone else.
- Player skins (including custom skins and other players' skins captured in snapshots) now stay intact during offline replay even when "Only Allow Trusted Skins" is enabled.
- Players gliding with an elytra now tilt horizontally along their flight path with spread wings during replay and timeline seeks instead of remaining upright.
- Undoing back to the saved state now clears the unsaved indicator in the status bar instead of leaving the project marked as dirty.
- Undoing or redoing edits made in another scene now switches back to that scene and clears stale timeline selections, and dragging a clip's speed slider now records a single history entry on release instead of pushing one per frame.
- Dropped item entities no longer disappear during high-speed timelapse playback or seeking.
- Replays recorded in worlds with the daylight cycle disabled now keep their recorded sun and moon time stationary during preview and export instead of advancing automatically.
- Dropped items and non-player entities captured in snapshots or spawned mid-recording now appear at their current positions and orientations instead of resetting to where they originally spawned.
- Players and entities riding boats, minecarts, horses, and other vehicles now stay synchronized with their vehicle across mid-recording snapshots, playback, and first-person or third-person follow cameras without jittering or drifting.

## [0.4.1-mc26.60] - 2026-11-22

### Added

- Added a horizontal scene bar above the timeline for adding, duplicating, renaming, reordering, switching and deleting independent scenes, each with its own camera tracks, markers and export range. Clips and keyframes are edited directly on each camera track, where higher tracks cover lower tracks across active clips and uncover them across gaps; a left tool strip provides Select (`V`), Razor (`C`) and Pen (`P`) tools, and clips default to the full replay and can be split (`Ctrl+B`, `Ctrl+K` or Razor), trimmed (`Alt+[`, `Alt+]` or edge drag), moved within free gaps, renamed, muted, locked, toggled for export directly on the timeline (`Shift+E`), or deleted without shifting later clips.
- Clips support playback speeds from `0.05x` to `32x` in both preview and export, with muted clips silencing preview and exported audio. Interpolated camera keyframes can enable Ignore clip speed to keep 1x camera motion across sped-up or slowed-down clips, marked with a `1x` badge on the timeline.
- Video export can render the active scene (enabled clips, in/out range, or a single clip) or batch-export multiple selected scenes sequentially, with an option to merge multiple enabled clips into one video or export them as separate videos, cutting cleanly across gaps and disabled clips.
- Video export includes a Timelapse mode that samples the replay at fixed real-time intervals (50 ms to 3,600,000 ms, with presets and custom input) while disabling audio export.
- Camera tracks can be reordered to change their priority. Drag a track in the timeline's track list, or use Move to top, Raise priority, Lower priority and Move to bottom in its right-click menu. Where tracks overlap, the track higher in the list plays, in preview and in export. Reordering can be undone and is saved with the project.
- Third-person keyframes can now enable Custom orbit / interpolate (`3P+`) to smoothly interpolate distance, anchor offset, yaw, pitch, roll and FOV around a subject using any interpolation curve (and Ignore clip speed), with options to follow the subject's rotation, clip against blocks, and capture or jump to the current free-camera view; existing fixed third-person keyframes stay unchanged.
- Interpolated free-camera keyframes can enable Look at entity (`Aim`) to keep yaw and pitch aimed at a chosen player, mob or non-mob entity (with vertical offset, smooth blending across target or free transitions, and viewport right-click assignment), and the entity picker now also lists non-mob entities such as boats, minecarts and projectiles in a collapsible section.

### Changed

- Refined the replay editor's right inspector and status bar with consistent inner padding, bordered inputs and checkboxes, styled primary/secondary/destructive action buttons with icons, responsive paired actions and two-column toggle grids, proportional clip speed chips, and aligned multi-axis keyframe inputs.
- Replaced the first-person and third-person camera track types with a lens mode on each keyframe: interpolated, first person or third person. A keyframe applies until the next keyframe on its track, and a person keyframe keeps its subject across dimension changes until the replay ends. The timeline marks each mode with its own keyframe shape and a `1P` or `3P` bar, the inspector and the keyframe menu change the mode of one or several keyframes, and the "+" button adds a camera track directly. When person keyframes are selected, the inspector can change the subject for all of them at once, and players or nearby mobs can be chosen from the searchable subject picker in the viewport and inspector as well as by right-clicking them in the viewport.

### Fixed

- The free camera no longer freezes in a replay while the recording held the player's input locked, such as with `/inputpermission` or a cutscene add-on, and no longer snaps back to positions the server corrected the recording player to. The recording player's input-permission and movement-correction packets are no longer recorded or applied to the replay viewer.
- Recorded player positions and replay player lists no longer drop players during respawn or when another entity reuses their runtime ID.
- Piston animations now stay aligned when camera tracks or cuts move the view across long distances during preview and export.
- First-person camera views no longer jitter or clip into nearby blocks from integrated-server movement corrections.
- First-person follow for other players now lowers the camera while crawling or gliding, and hides entities riding the followed player.
- Burning entities in replays show their fire on their bodies instead of floating above or beside them. Replay entities' bounding boxes now follow their current and interpolated poses, so fire overlays, culling, and viewport entity picking stay aligned with where entities are drawn.
- Opening a replay from a world with custom blocks that use custom components no longer crashes.
- A keyframe's roll now shows in the editor preview, both while paused and while playing. Before, only the export applied it.
- Moving the free camera in the replay editor no longer slows down after adding keyframes, pausing a camera preview, or jumping to a keyframe or player.
- Fixed missing chunks and stalled terrain loading when recording or scrubbing across repeated dimension transitions.
- Fixed free-camera lockups and backward-seek crashes when scrubbing across villager trade, mount equipment, or other previewed screens.

## [0.4.0-mc26.60] - 2026-11-15

### Added

- Added first-person follow for the recorded player, with teleport and follow actions in the camera menu. The view uses the recorded camera offset and rendered FOV (including the spyglass), the recorded HUD (inventory, armor, offhand, selected slot, health, hunger, experience and game mode), held maps, view bobbing, hand sway, the nether portal warp and the bed view with its fade to black while sleeping. Recorded container screens are previewed while following.
- Other recorded players and any entity can be followed in first person, from the subject picker or the viewport's right-click and nearby entities menus. When the subject leaves the server, or an entity stays gone, the view returns to the recorder.
- Video export can render first-person video from the chosen subject, with the subject's arm and held item and without its model.
- Video export now captures optional mono or stereo game audio: AAC, Opus or FLAC in MP4, or an uncompressed sidecar `.audio.wav` for PNG sequences. AAC and Opus offer 128, 192, 256 or 320 kbps; AAC is the default. The game's sound output is restored when the export ends.
- Recordings now embed the server's downloaded resource packs and mount them on playback, so server textures and models show anywhere. Extracted packs are cached for the three most recent replays. `record.embedServerPacks` in the config turns this off.
- The local player's view can be recorded between ticks with `/record rate` (20 to 120 samples per second; 20 records once per tick, the default). Replay uses those samples instead of a straight line between ticks. The starting rate comes from `record.localPlayerUpdatesPerSecond` in the config; `/record rate` changes it until the game restarts.
- Camera keyframes are now applied to the preview while the replay is paused.
- Added first-person and third-person camera tracks next to keyframe tracks. A person track views a replay player from its first key to its last, and each key picks the player from the inspector's searchable list. Where tracks overlap, the highest track in the list plays, in preview and in export. The viewport camera chip names the track that is playing.
- Added a searchable player list for choosing which player to follow in the viewport.
- Added right-click menus for keyframes, empty lanes, track headers and the ruler on the timeline.
- Tapping Shift on its own while following a player now stops following.
- Added a third-person follow view, toggled with F5 while following a player. A sleeping subject still shows its first-person bed view.
- Camera tracks can be renamed, locked and cleared from their right-click menu, and a keyframe's menu can move the free camera to its pose.
- Added multi-select for keyframes: Ctrl+click, box select on empty canvas and Ctrl+A. Selected keyframes move, delete, copy, paste and change interpolation together.
- Holding Shift while dragging snaps to keyframes, markers, segment edges, the playhead and the export range.
- Markers can be added with M or from the ruler menu, dragged, renamed and deleted. `[` and `]` jump between them.
- Added recording controls to the small icon row of the pause menu: start or resume, pause, finish and cancel.
- Finishing a recording shows a save dialog with a name input that defaults to the timestamp, plus save and delete actions. Deleting and cancelling ask for confirmation first. Escape cannot dismiss the save dialog, so a finished recording cannot be dropped by accident. Starting a new recording while one is still waiting for a name saves it under its timestamp name first.
- Added a top-right recording indicator that shows the recording state and elapsed time. `/record indicator [on|off]` shows or hides it. It never appears in replays, the editor, exported videos or recording thumbnails.
- The inspector has a Preview page for choosing which HUD elements a replay shows: chat, boss bar, title text, scoreboard, action bar and hotbar. Only the hotbar is on by default; the choices are saved and also apply to exported video. The export dialog's Hide HUD option leaves the whole HUD out of an export.
- The inspector's Preview page has audio controls for the editor preview: mute, volume, and Silent while paused, which keeps sounds triggered while the replay is paused or seeking from playing. They are saved and do not affect exported audio.

### Changed

- The camera path overlay and the paused camera glyph only show for keyframe tracks.
- The camera switch track was removed, and its data in earlier editor projects is dropped.
- Replays recorded before `v0.4.0` are no longer supported and must be recorded again. Older archives may not be detected as incompatible; back up recordings and projects before upgrading. No migration is provided.
- Dropdowns, popups and context menus in the editor now open under the control that opened them, with menu padding.
- Recording snapshots reuse unchanged chunk columns and serialize changed columns on background threads to reduce game stalls.
- Seeking in a replay now catches up faster by loading the world in merged chunk batches.
- Requires exactly Minecraft `1.26.60.5`; Playback refuses to load on other game builds and reports the required version.
- Exports now always run a client tick as soon as they ask for one instead of waiting for the game's 20 Hz timer, which the experimental Accelerate client option used to turn on. The option is removed.
- Exports on DirectX 12 now draw a frame once instead of two or more times, which shortens them; a frame is only drawn again while the replay is still preparing the next one. Ray-traced lighting therefore adapts about once per exported frame, so how quickly it follows a change such as the time of day no longer depends on how fast the export happens to run.
- The export setting Convergence frames is now called Extra renders, which is what it does: each frame is rendered that many more times before it is saved. What it does and its default of 0 are unchanged.
- The export dialog now has General, Video, Audio and Advanced pages. Export buttons stay visible on every page; the summary includes encoder settings and hides first on short windows before the settings page scrolls.

### Fixed

- Recordings remain playable when the server switches dimensions again before the new world finishes loading. Stopping during that transition keeps the portion already recorded.
- Terrain stays visible in continuous previews and video exports after rapid dimension changes.
- Replays from servers with custom items now record the server's item registry and apply it on playback, so recorded item ids resolve to the server's items.
- Fixed a freeze when chunk draining re-entered the level join while recording.
- Particles are rebuilt after a seek.
- Recorded container screens stay open across seeks and exports, and the inventory screen is no longer previewed.
- Fixed a dimension loading screen that could stay up forever during export.
- First-person follow now shows the followed player's arm swing and item-use animation, such as eating.
- Recordings now keep the recording player's own predicted sounds, such as hitting blocks, landing and strong attacks, so replays and exported audio no longer miss them.
- Recordings now keep the sounds the client plays for every entity itself, such as footsteps, jumping, mob calls and wing flaps, and replays mute the copies the game regenerates, so replays and exported audio match what was heard and nothing plays twice.
- Fixed item pickups, mob hurt and death sounds, and other entity and block events playing twice in replays. Each packet the client handles is now recorded once, as it was received.
- Fixed a crash when quitting the game after the replay editor had been opened or while a video export was running.
- Fixed the free camera's FOV widening while a followed first-person player sprinted. Following a player now only copies the attributes the HUD shows.
- Recorded server forms no longer trap the free camera. Containers and forms are previewed only while following the recorder in first person; seeks restore their recorded state and switching away closes the preview. Other server response screens are not opened.
- Ray tracing and Vibrant Visuals exports now time auto exposure and other time-driven effects on the replay instead of on how long each frame took to render, so exported brightness adapts at the same pace as in the replay.
- Exports now repeat the renderer warm-up after the replay changes dimension, so the first frames in the new dimension are no longer captured while auto exposure is still adapting.
- Ray tracing exports of the same replay no longer differ in brightness for a few seconds after the lighting changes, such as after a time change. The game now draws on every frame it sends to the renderer during an export.
- Exports now time interface animations, such as a chat message leaving the HUD, on the replay instead of on how long the export took, so a message stays on screen for the same length of video in every export.
- Exports now run particles, the held item's equip animation, the sky, cloud and rain transitions and HUD timers such as the item name on the replay instead of on how long the export took, so they move at the same speed in the video as in the replay.
- Recordings now take the snapshot of a new dimension once the player has finished loading it, including the short gap before the loading screen appears, so replays and exports show the terrain the player first saw right after a dimension change instead of an empty world that fills in over the next second or two. Replays recorded earlier are unchanged.
- Exports without a camera track keep their fixed 70 degree field of view after a dimension change instead of switching to the player's field of view setting for the rest of the video.
- Replays and exports of the End no longer show a second ender dragon and boss bar that the replay world spawned on its own, and no longer drop a recorded entity that shared its id.
- Replay worlds now use the recorded world's seed instead of choosing a new one.
- Replays and exports now preserve the recorded biomes when returning to a dimension, keeping the sky and water colors consistent with the recording.
- Closing the game from a replay now waits for its temporary world to close and be removed, including when an export is in progress.
- The replay list's selection summary separates the file count and the total size with a middle dot instead of a garbled character.
- Editor dialogs and menus stay inside the game window at any size. The keyboard shortcut sheet and message dialogs stay centred when the window is resized, the shortcut sheet switches to two columns or scrolls on a short window, menus too long for the window scroll, and long tooltips wrap. On a narrow window the replay list toolbar shrinks the search box first, then drops the import and sort labels and finally the title.
- Numbers typed into the export dialog take effect at once, without pressing Enter, so starting the export straight after typing uses them. Out-of-range values are clamped as they are typed.

## [0.3.3-mc26.60] - 2026-11-03

### Changed

- Adapted to LeviLamina 26.60.*

## [0.3.3-mc26.50] - 2026-10-11

### Added

- Added encode speed (Quality, Fast, Fastest) and CRF settings for MP4 export under Advanced. They map to the libx264 `medium`, `veryfast` and `ultrafast` presets and `-crf`; the defaults match the previous fixed `medium` / CRF 18.
- Added an experimental Accelerate client option under Advanced that hands out client ticks as soon as an export asks for one instead of waiting for the game's 20 Hz timer. It is off by default.

### Changed

- Regrouped the export dialog into output, timeline and capture sections plus a collapsible Advanced section that keeps the dialog height and scrolls instead of growing.
- Export frames whose scene misses the capture are now re-rendered as soon as the miss is detected instead of after a timeout, making exports several times faster.
- Splitters and the timeline zoom bar now show an open or closed hand cursor, and splitters highlight while hovered or dragged.
- Restyled the timeline zoom bar with ring handles and a thin grey bar.
- Delete, rename and error prompts in the replay list and editor now share one dialog style.
- The selected replay in the replay list is tinted with the accent colour and outlined, so it is easy to tell from hover.
- Raised the export convergence frame limit from 240 to 1000.
- Moved the export convergence frame count into the Advanced section and made it default to 0 on every renderer, including ray tracing.
- Opening the Advanced section of the export dialog now scrolls it into view.

### Fixed

- Fixed long exports slowing down and GPU usage dropping after about 15 minutes without input. The game's Eco Mode is now switched off while an export runs and restored afterwards, including when the export fails with an error or the game is killed mid-export (restored on the next launch).
- Pinned the replay world time before every render pass of an export frame, so the level clock cannot drift between passes.
- Fixed the export progress UI blurring when the export resolution is below the window size; it now draws on its own layer at the window resolution.
- Fixed the replay list flashing and reloading every thumbnail when a replay is deleted.
- Fixed the selected replay's highlight flickering in the replay list.

## [0.3.2-mc26.50] - 2026-10-01

### Added

- Added a 4x supersampling tier to video export. Tiers whose supersampled render size does not fit are disabled for the chosen resolution.
- Added a convergence frame count to the export settings. Each frame is rendered without a capture until the ray-traced denoiser settles, removing green and yellow streaks from exported skies; it defaults to 8 with ray tracing active and 0 otherwise.
- Added support for the separate OptiPiston mod: when it is installed, Playback drives its piston animation from the replay clock, with its settings in the inspector.

### Changed

- Exports now use the chosen name verbatim as the output file name instead of rewriting it into a folder with a parameter suffix. Existing files get a numbered suffix, and the default name is a timestamp.
- Refined the export settings and progress dialogs: a fixed height that fits 1080p without a scrollbar, clearer hints and timecodes, and a translucent progress overlay with a pill-shaped cancel button.
- Supersampled captures are now downsampled on the GPU, so readback stays at the output size; frames are normalized and encoded in separate stages, and captured pixel buffers are reused.
- The replay browser now loads thumbnails on demand with bounded caches.
- Kept the configuration version, recording-file snapshot context version, and project format version at `1`; existing `v0.3.1` archives and projects remain compatible.

### Fixed

- Fixed player skins not loading in replays on MC 26.51. Skin-change packets are now recorded and remapped, snapshots fall back to the player's own skin, and player-list entries without a skin get the default skin.
- Fixed custom block geometry not being initialized in replays on MC 26.51.
- Kept the offline export clock in exact milliseconds on MC 26.51 so exported frame timing does not drift.
- Fixed the export cancel button not responding to the mouse while an export runs.
- Kept the renderer drawing while an export waits, so the render clock keeps advancing instead of stalling the export.
- Preserved completed offline frame samples so they are not discarded before capture.
- Hid camera paths and markers while the game HUD is covered by loading, progress, or menu screens, and restored them when the HUD returns.

## [0.3.1-mc26.50] - 2026-09-23

> **First release on the MC 26.51 line, branched from `v0.3.1-mc26.40`.**

### Added

- Added `Ctrl+Shift+N` to create a camera track and `1`, `2`, `3` to switch to the first three cameras, which also drives the viewport preview.
- Added `Ctrl+Shift+-`, `Ctrl+Shift+=`, and `Ctrl+Shift+0` to step and reset the interface scale.
- Added `Alt+Wheel` to zoom the timeline around the cursor.
- Added a keyboard shortcut reference to the getting-started guide.

### Changed

- Moved timeline zoom to `-` and `=`, zoom-to-fit to `\`, and playback speed to `J` and `L`, matching common video editors. `Shift+Wheel` no longer zooms.
- Made `K` add a keyframe to the first usable camera track when nothing is selected instead of doing nothing.

### Fixed

- Fixed recorded players standing still on MC 26.40 by restoring their entity-id registration and remapping the local-player snapshot's abilities and entity links.
- Fixed replay disconnections caused by teleport movement packets missing their required teleport data.
- Fixed backward seeks stalling and closing the replay when previously loaded request-mode chunk columns were not replaced with the recorded data.
- Preserved recorded player names instead of forcing synthetic names, and restored names from the recorded player list for affected existing recordings.
- Fixed entities jittering during preview playback. Recorded poses are now interpolated on preview frames as well as during export, and are sampled one tick behind the camera so the pose history is actually interpolated instead of snapping to its end.
- Fixed one missing or incomplete entity cancelling pose interpolation for the whole frame, and left passenger positions to their vehicle so riders no longer fight the mount.
- Fixed camera-path overlays being sampled across dimension boundaries, which spent work on points that were discarded at draw time.
- Fixed a plain click on a timeline keyframe nudging it; any horizontal movement is now required before a drag begins, so a click only seeks.
- Fixed players not loading in the replay when a server withheld skin data for any one of them. A single entry without a skin discarded the whole player list; that entry is now skipped and the rest of the list still spawns.
- Fixed the real local player's identity leaking into recorded `AddPlayer` packets through the abilities payload's target-player field, which is where MC 26.40 moved that field.
- Fixed the imgui SRV descriptor heap running out once the font atlas and per-replay browser thumbnails were added; the heap is now larger and logs instead of handing imgui a null descriptor when it is still exhausted.
- Fixed the editor UI waiting on full chunk streaming before it appeared; it now shows as soon as the loading and progress screens clear.
- Fixed stale replay-player references after a failed refresh, remapped `AddPlayer` abilities and link references from the real local player to the recorded sentinel id, and stopped raising the global runtime-id allocator to the synthetic recorded-player id.
- Fixed passenger detection after the `isRiding` overload it relied on was removed upstream.

## [0.3.1-mc26.20] - 2026-09-14

### Added

- Added a camera-path overlay in the viewport. Camera tracks are drawn as splines with keyframe markers, clipped against the near plane so the path stays anchored to the rendered view.
- Added selectable UI scale tiers for the editor, applied without re-rasterising glyphs.
- Added direct playback-speed selection in the transport, snapped to the same speed ladder the keyboard shortcuts step through.
- Added an export-range marker on the timeline, with `I` and `O` to set its bounds and `Space` to toggle play/pause.

### Changed

- Rebuilt the editor layout around a centred transport, with reworked timeline rows, details panel, status bar, and menu bar.
- Aligned timeline and toolbar icons optically rather than on their glyph boxes.
- Kept the configuration version, recording-file snapshot context version, and project format version at `1`; `v0.3.0-mc26.20` archives and projects remain compatible.

### Removed

- Removed the scene inspector page and the property search box; neither affected the replay or the export.
- Removed the world-actor segment speed slider, command, and action. Segment speed had no export path, so it never changed playback; the serialised field stays because splitting a segment derives its source tick from it.

### Fixed

- Fixed replays losing a dimension boundary when the recorder rotated the chunk instead of leaving a packet on the timeline; boundaries are now derived from the snapshot contexts.
- Fixed columns without complete subchunk coverage being injected, which could leave partially built terrain.
- Fixed clicks on viewport overlays reaching the game camera.

## [0.3.0-mc26.20] - 2026-09-08

### Added

- Added persistent editor projects. Editing state is saved to `<dataDir>/projects/<replay>.pbproj` on Ctrl+S, from the menu, every 30 seconds, and when the editor closes; the replay file remains authoritative for the timeline duration.
- Added replay of server custom blocks. Block types recorded in the StartGame packet are registered when the replay world loads and rendered with their recorded geometry and materials.
- Added resolution of custom dimensions by their recorded name, so replays can reconstruct dimensions whose ids are not vanilla.
- Added the recording game version to replay metadata. The replay browser refuses to open files recorded on a different Minecraft version and shows both versions.

### Changed

- Made keyboard input UI-exclusive while the editor is open unless the left mouse button is held over the game viewport. Escape no longer reaches the game; holding it for 0.8 s exits the editor and saves the project.
- Kept the editor hidden until the replay world is joined and ready, so loading, resource-pack download, and sign-in screens are no longer covered.
- Cut seek time roughly in half by draining chunk injection inline during catch-up and reusing already-applied snapshot columns.
- Downgraded project save/load, keyframe capture, and custom block registration logs to debug.
- Kept the configuration version, recording-file snapshot context version, and new project format version at `1`; complete `v0.2.1-mc26.20` archives remain compatible and require no conversion.
- Documented that Vibrant Visuals is not supported yet; recording, replay, and export are only validated with the standard renderer.
- Documented that `0.2.0` was skipped on the MC 26.20 line because video export stalled on that runtime, and that `0.2.1` is published for both runtimes.

### Fixed

- Fixed replays recorded by a newer build failing outright when a configuration packet had an unknown lifecycle; such packets are now skipped with a warning.
- Fixed Ctrl shortcuts such as undo/redo never triggering because ImGui modifier state was not submitted.
- Fixed the camera preview keeping a stale cached pose after a camera track was toggled.
- Fixed timeline wheel-zoom anchoring to the wrong tick when the cursor was outside the canvas.
- Fixed the property search box discarding typed text every frame.
- Fixed a rewind keeping stale player-list skins instead of re-registering them.

## [0.2.1-mc26.20] - 2026-08-27

### Changed

- Reworked export capture around the RenderDragon scene target and semantic world-submission detection instead of the swap-chain UI target and a draw-count threshold.
- Polished editor and replay-browser scrolling, timeline routing, selection feedback, icon alignment, compact controls, animations, and bilingual labels.
- Preserved the release tag during CI builds so packaged manifests carry the published version.
- Kept the configuration version and recording-file snapshot context version at `1`; complete `v0.2.0-mc26.20` archives remain compatible and require no conversion.

### Removed

- Removed unreachable legacy clip, track, transition, clipboard, menu, hint-bar, curve-editor, and entity-render-hook code.
- Removed periodic camera, entity-pose, and graphics-capture diagnostics from normal rendering and export paths.

### Fixed

- Fixed D3D12 exports capturing the UI composition target, which could produce uniform sky-color frames or include the export overlay instead of the world scene.
- Fixed sparse scenes stalling export when world submissions fell below an arbitrary draw-count threshold.
- Fixed the HBUI idle screen appearing in captures while Playback owned input.
- Fixed camera preview stepping backward when native render alpha and replay ticks advanced on different clocks, especially at reduced playback speeds.
- Fixed ordinary observer server synchronization abandoning a parked preview camera after pausing.
- Fixed replay-browser wheel scrolling and crowded timeline labels or click routing obscuring the intended target.

## [0.2.0-mc26.20] - 2026-08-20

### Added

- Added an in-game cinematic camera workflow with camera creation, capture-at-playhead, keyframe editing, sequence binding, live preview, property inspection, and undo/redo.
- Added Smooth, Linear, Ease In, Ease Out, Ease In/Out, Hold, Hermite, and Cubic Bezier camera interpolation.
- Added experimental frame-by-frame H.264 MP4 and PNG-sequence export with configurable tick range, frame rate, resolution, SSAA, warm-up, progress, cancellation, finalization, output validation, and clear-frame retry handling.
- Added D3D11 and D3D12 framebuffer capture paths. D3D12 supports SSAA 1x/2x; D3D11 uses the stable 1x path.
- Added camera-driven observer/chunk tracking and stable-frame warm-up after fast movement or dimension changes.

### Changed

- Flattened the source tree into recording, replay, editor, state, keyframe, exporting, visuals, runtime, packet, and I/O domains.
- Made every recorded dimension change an interpolation boundary. Camera curves never connect across a dimension transition, including when an intermediate dimension has no keyframes.
- Made paused replay use the freely movable observer camera, while active keyframe preview/export uses the sampled timeline camera.
- Reworked replay rendering so preview and export share the camera timeline, entity-pose sampling, observer synchronization, and cross-dimension state machine.
- Unified entity rendering around one sampled pose per frame and temporarily suppressed native movement interpolation during export rendering.
- Reworked chunk recording to preserve native request-mode packets for local worlds while converting inline or cache-enabled server chunks into portable replay data after client decoding.
- Changed export preparation to rely on replay tick, dimension transition, UI stability, and warm-up state instead of requiring every chunk in a fixed camera neighborhood.
- Derived the packaged mod version from the nearest Git release tag so hotfix builds retain the `0.2.0-mc26.20` identifier.
- Set the product version to `0.2.0` and the MC 26.20 release identifier to `v0.2.0-mc26.20`.
- Kept the configuration version in `Config.h` and the recording-file snapshot context version at `1`; no configuration or replay migration layer is provided.
- Replays recorded before this hotfix on affected servers may already be missing portable chunk or custom-entity registry data. Those archives cannot be repaired and must be recorded again; complete `v0.2.0-mc26.20` archives do not require conversion.

### Removed

- Removed compatibility branches for older action payload layouts. Replay archives created by previous Playback releases are incompatible and must be recorded again.

### Fixed

- Fixed preview and export stalls when entering, revisiting, or leaving dimensions.
- Fixed forced-snapshot reader boundaries being considered export-ready before the next replay chunk was applied.
- Fixed camera paths loading or capturing the wrong chunk region during fast movement and cross-dimension export.
- Fixed high-speed player ghosting caused by native and replay render paths observing different interpolated positions.
- Fixed free-camera translation stutter, pause rebound, third-person offsets, mouse grab/release loops, and unnecessary pause-time server teleports.
- Fixed raw camera FOV units, D3D11 supersampling fallback, D3D12 clear-frame recovery, and SSAA values above the stable 2x limit.
- Fixed replay terrain appearing as air on servers that send inline or cache-enabled chunk data.
- Fixed local-world seek failures caused by duplicate chunk columns or missing subchunk sections after server compatibility conversion.
- Fixed resource-pack handshakes discarding actor-registry configuration required to reconstruct custom server entities in newly recorded replays.
- Fixed snapshot application choosing an older LevelChunk record when multiple versions of the same chunk column were present.
- Fixed offline video export stopping when Minecraft lost focus or attempted to open pause or suspend UI.
- Fixed valid exports remaining at frame 0 because a fixed 5x5 camera-neighborhood readiness check could never be satisfied.

## [0.1.2-mc26.20] - 2026-08-03

### Added

- Added replay-browser search, playable/unavailable filters, five sorting modes, grid/details views, import, rename, multi-selection deletion, path copying, and File Explorer integration.
- Added replay thumbnail capture into `icon.png` and thumbnail display in the browser on both D3D11 and D3D12.
- Added in-memory editor groundwork for sequence and world-actor segment operations, camera-track data and keyframes, and undo/redo. Project persistence and video export remain unavailable.
- Added D3D11 rendering support for the native replay UI alongside the existing D3D12 path.

### Changed

- Changed the replay metadata and snapshot stream to require per-chunk dimension/view context, explicit local-player creation, and forced snapshot boundaries. Replays created by `v0.1.1` or earlier are not compatible and must be recorded again after upgrading.
- Kept the internal `Config` version field at its initial value (`1`) and left third-party dependency version declarations unchanged. No configuration or replay migration path is provided.
- Replaced the resource-pack-based replay browser with a native ImGui browser while retaining a lightweight resource-pack button on the main menu.
- Rebuilt the existing replay editor around resizable viewport, details, and timeline panels, zoomable tracks, viewport maximize/restore, and revised transport controls.
- Refactored replay snapshots to carry dimension context and local-player state explicitly, and to force snapshot playback at dimension boundaries.
- Unified command, replay-browser, and editor messages through the English and Simplified Chinese catalogs using LeviLamina's native translation path.
- Bundled the Lucide icon font directly with the mod and reduced the UI resource pack to its main-menu button, bilingual label, and required metadata.
- Consolidated the feature overview in the root README files and replaced the legacy documentation tree with focused installation and source-build guides.

### Fixed

- Restored installed and standalone `playback-ui.mcpack` packaging so the main-menu button is available in complete mod installations and as a separate Release asset.
- Reworked cross-dimension recording and replay handling to close the source chunk at the tick boundary, wait for the target dimension, and apply a dimension-scoped forced snapshot.
- Prevented ordinary forward seeks from reloading snapshots and moving the viewer; snapshot reloads remain for backward seeks, dimension changes, and forced boundaries.
- Improved replay snapshot cleanup and chunk application by refreshing the replay player, isolating chunks by dimension, filtering unsuccessful subchunk responses, and clearing recorded objectives between snapshots.
- Expanded D3D12 command-queue and swap-chain capture to cover additional renderer creation paths.

## [0.1.1-mc26.20] - 2026-07-29

### Added

- Added PDB symbols to release assets to help diagnose crashes.
- Adapted to Minecraft 26.20.4

## [0.1.0-alpha.2] - 2026-07-29

### Changed

- Bundled the Playback UI resource pack with the mod so LeviLauncher and Lip installations load it automatically through LeviLamina.
- Kept `playback-ui.mcpack` available as a standalone release asset for manual import.

  > **This release changes installation packaging only. The replay format and replay runtime behavior are unchanged from `0.1.0-alpha.1`.**

## [0.1.0-alpha.1] - 2026-07-27

### Added

- Client-side recording with asynchronous replay storage and export.
- Main-menu replay browser and isolated replay-world loading.
- Replay timeline controls for pause, seek, speed, and exit.
- Chunk snapshots, cached chunk replay, entity movement, and selected game-packet replay.
- English and Simplified Chinese localization for commands, the replay editor, and the resource-pack UI.

  > **This is the first public test release. Replay files and behavior may change before `1.0.0`.**
  > **Playback currently targets Windows x64 and the LeviLamina `26.10.*` client runtime.**

[Unreleased]: https://github.com/wo55555/Playback/compare/v0.4.1-mc26.60...HEAD
[0.4.1-mc26.60]: https://github.com/wo55555/Playback/compare/v0.4.0-mc26.60...v0.4.1-mc26.60
[0.4.0-mc26.60]: https://github.com/wo55555/Playback/compare/v0.3.3-mc26.60...v0.4.0-mc26.60
[0.3.3-mc26.60]: https://github.com/wo55555/Playback/compare/v0.3.3-mc26.50...v0.3.3-mc26.60
[0.3.3-mc26.50]: https://github.com/wo55555/Playback/compare/v0.3.2-mc26.50...v0.3.3-mc26.50
[0.3.2-mc26.50]: https://github.com/wo55555/Playback/compare/v0.3.1-mc26.50...v0.3.2-mc26.50
[0.3.1-mc26.50]: https://github.com/wo55555/Playback/compare/v0.3.1-mc26.20...v0.3.1-mc26.50
[0.3.1-mc26.20]: https://github.com/wo55555/Playback/compare/v0.3.0-mc26.20...v0.3.1-mc26.20
[0.3.0-mc26.20]: https://github.com/wo55555/Playback/compare/v0.2.1-mc26.20...v0.3.0-mc26.20
[0.2.1-mc26.20]: https://github.com/wo55555/Playback/compare/v0.2.0-mc26.20...v0.2.1-mc26.20
[0.2.0-mc26.20]: https://github.com/wo55555/Playback/compare/v0.1.2-mc26.20...v0.2.0-mc26.20
[0.1.2-mc26.20]: https://github.com/wo55555/Playback/compare/v0.1.1-mc26.20...v0.1.2-mc26.20
[0.1.1-mc26.20]: https://github.com/wo55555/Playback/compare/v0.1.0-alpha.2...v0.1.1-mc26.20
[0.1.0-alpha.2]: https://github.com/wo55555/Playback/compare/v0.1.0-alpha.1...v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/wo55555/Playback/releases/tag/v0.1.0-alpha.1
