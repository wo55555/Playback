<!--
Keep this PR focused and concise.
Remove sections that do not apply.
Replace [ ] with [x] only for checks you actually completed.
Do not claim validation that was not performed.

For a bug fix, feature, documentation, or translation PR, you may use the corresponding specialized template in `.github/PULL_REQUEST_TEMPLATE/`.
-->

## Summary

<!-- Describe the problem and the focused change that resolves it.
Link the relevant issue when applicable, for example: `Closes #123`. -->

## Validation

<!-- Include additional validation performed and, when relevant, the Minecraft, LeviLamina, and Playback versions used. -->

- [ ] `xmake -r -y`
- [ ] `git diff --check`
- [ ] Tested in Minecraft when the change affects recording, replay, camera, export, or UI behavior

## Compatibility and Replay Impact

<!--
Describe changes to compatibility, replay files, recorded data, or runtime behavior.
Write `None` when not applicable.
-->

## Checklist

- [ ] The change is focused and contains no unrelated refactoring.
- [ ] Changed C++ files have been formatted with the repository configuration.
- [ ] Translation keys remain aligned across supported languages where applicable.
- [ ] New third-party code or assets include the required license notice.
- [ ] Logs, replays, screenshots, and test data contain no private server or player information.