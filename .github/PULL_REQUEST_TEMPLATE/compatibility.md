<!--
Describe the compatibility-related change.
Remove sections that do not apply.
Replace [ ] with [x] only for checks you actually completed.
Do not claim compatibility or validation that was not actually verified.
-->

## Summary

<!-- Briefly describe what compatibility problem this PR addresses. -->

## Related Issue

<!-- Use `Fixes #123`, `Resolves #123`, or `Related to #123` when applicable. -->

## Related Discussion

<!-- Link a GitHub Discussion when the compatibility issue, investigation, or design was discussed there. -->

## Affected Versions

<!--
List the relevant Minecraft, LeviLamina, and Playback versions.
Mention whether the change affects a specific release line or multiple versions.
-->

## Compatibility Changes

<!--
Describe what compatibility changes and why.
Mention changed APIs, dependencies, runtime assumptions, or other compatibility requirements where relevant.
-->

## Replay Impact

<!--
Describe whether this affects:
- existing replay archives;
- newly recorded replays;
- replay readability;
- recorded data;
- replay format or configuration.

Clearly state whether existing replays remain usable.
Write `None` when not applicable.
-->

## Validation

- [ ] `xmake -r -y`
- [ ] `git diff --check`
- [ ] Tested the affected compatibility behavior in Minecraft when applicable

<!--
Describe the versions and environments actually tested.
If multiple versions were tested, list them.
If a version could not be tested, state that explicitly.
-->

## Notes

<!--
Add migration requirements, known limitations, compatibility trade-offs,
screenshots, logs, or other reviewer context when useful.
-->

## Checklist

- [ ] The change is focused and contains no unrelated refactoring.
- [ ] Changed C++ files have been formatted with the repository configuration.
- [ ] Compatibility claims are supported by actual validation or clearly identified as untested.
- [ ] New third-party code or assets include the required license notice.
- [ ] Logs, replays, screenshots, and test data contain no private server or player information.