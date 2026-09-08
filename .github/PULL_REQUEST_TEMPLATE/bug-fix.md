<!--
Describe what was fixed and how it was verified.
Remove sections that do not apply.
Replace [ ] with [x] only for checks you actually completed.
Do not claim validation that was not performed.
-->

## Summary

<!-- Briefly describe the bug and the expected behavior. -->

## Related Issue

<!-- Use `Fixes #123` or `Resolves #123` when applicable. -->

## Related Discussion

<!-- Link a GitHub Discussion when the design, investigation, or approach was discussed there. -->

## Reproduction

<!--
Describe the conditions and steps needed to reproduce the problem.
If the bug is already documented in an issue, reference it instead of duplicating the full reproduction steps (e.g. `#123`).
Include relevant Minecraft, LeviLamina, and Playback versions when version-dependent.
-->

## Root Cause

<!-- Explain what caused the bug. -->

## Solution

<!-- Explain how this PR fixes the problem. -->

## Validation

- [ ] `xmake -r -y`
- [ ] `git diff --check`
- [ ] Reproduced the bug before the fix when practical
- [ ] Verified the affected behavior after the fix
- [ ] Tested in Minecraft when the change affects recording, replay, camera, export, or UI behavior

<!--
List additional validation and the Minecraft, LeviLamina, and Playback versions used.
-->

## Compatibility and Replay Impact

<!--
Describe any impact on compatibility, replay files, recorded data, or runtime behavior.
Write `None` when not applicable.
-->

## Notes

<!-- Add screenshots, logs, replay files, limitations, or other reviewer context when useful.
Do not include private server or player information. -->

## Checklist

- [ ] The change is focused and contains no unrelated refactoring.
- [ ] Changed C++ files have been formatted with the repository configuration.
- [ ] New third-party code or assets include the required license notice.
- [ ] Logs, replays, screenshots, and test data contain no private server or player information.