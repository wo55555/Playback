# Contributing to Playback

Thank you for helping improve Playback. Bug fixes, compatibility updates, documentation, and focused replay improvements are welcome.

## Before You Start

- Search existing issues before opening a new one.
- Keep changes focused and avoid unrelated refactors.
- For replay behavior changes, describe the Minecraft and LeviLamina versions used for validation.
- Do not include worlds, replay files, logs, or screenshots that contain private server addresses or player information without permission.

## Build

Playback currently supports the Windows x64 LeviLamina client target.

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

## Format and Check

Format changed C++ files with the repository configuration:

```powershell
clang-format -i <changed-cpp-or-header-files>
```

Before opening a pull request, run a clean Release build and check for whitespace errors:

```powershell
xmake -r -y
git diff --check
```

Runtime-sensitive replay changes should also be tested in Minecraft. A successful build alone does not establish that a replay can be opened or played correctly.

## Translations

Playback has two localization layers:

- `src/lang/*.json` contains command and replay-editor translations.
- `resources/texts/*.lang` contains resource-pack UI translations.

When adding or changing translated text, keep the keys aligned across all supported languages in the relevant layer. Add new locale codes to `resources/texts/languages.json` when introducing a resource-pack language, and verify that the packaged files are present under `bin/playback/lang/` and inside `bin/playback/playback-ui.mcpack`.

## Pull Requests

Include:

- A concise description of the problem and solution.
- The validation performed, including runtime checks where applicable.
- Compatibility or replay-format impact.
- License notices for any newly introduced third-party code or assets.
