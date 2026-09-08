<!--
Describe the performance-related change.
Remove sections that do not apply.
Replace [ ] with [x] only for checks you actually completed.
Do not claim validation or measurements that were not performed.
-->

## Summary

<!-- Briefly describe what was optimized and where it affects Playback. -->

## Related Issue

<!-- Use `Fixes #123`, `Resolves #123`, or `Related to #123` when applicable. -->

## Related Discussion

<!-- Link a GitHub Discussion when the design, investigation, or approach was discussed there. -->

## What Changed

<!-- List the main implementation changes. -->

-

## Why It Improves Performance

<!--
Explain what unnecessary work, overhead, contention, allocation, I/O,
memory usage, or other bottleneck is reduced or avoided.
-->

## Benchmark

<!--
Optional.

If benchmark or profiling data is available, describe:
- what was measured;
- the workload or scenario;
- relevant environment and versions;
- before/after results;
- how the measurement was obtained.

Do not include benchmark data that was not actually measured.
-->

- [ ] Benchmark or profiling data provided

## Validation

- [ ] `xmake -r -y`
- [ ] `git diff --check`
- [ ] Tested the affected behavior in Minecraft when applicable

<!--
Describe additional validation and include relevant Minecraft,
LeviLamina, and Playback versions when runtime behavior is affected.
-->

## Compatibility and Replay Impact

<!--
Describe any impact on compatibility, replay files, recorded data,
or runtime behavior. Write `None` when not applicable.
-->

## Notes

<!-- Add limitations, trade-offs, profiling details, or follow-up work when useful. -->

## Checklist

- [ ] The change is focused and contains no unrelated refactoring.
- [ ] Changed C++ files have been formatted with the repository configuration.
- [ ] New third-party code or assets include the required license notice.
- [ ] Logs, replays, screenshots, and test data contain no private server or player information.