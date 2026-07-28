## Summary

Describe the problem and the focused change that resolves it.

## Validation

- [ ] `xmake -r -y`
- [ ] `git diff --check`
- [ ] Tested in Minecraft when the change affects recording, replay, or UI behavior

Describe any additional validation and list the Minecraft, LeviLamina, and Playback versions used.

## Compatibility and Replay Impact

Describe changes to compatibility, replay files, recorded data, or runtime behavior. Write `None` when not applicable.

## Checklist

- [ ] The change is focused and contains no unrelated refactoring.
- [ ] Changed C++ files have been formatted with the repository configuration.
- [ ] Translation keys remain aligned across supported languages where applicable.
- [ ] New third-party code or assets include the required license notice.
- [ ] Logs, replays, screenshots, and test data contain no private server or player information.
