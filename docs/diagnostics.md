# Diagnostics and restart semantics

A failed plugin is not a pending restart. `wrong ELF class` means the plugin or a
loaded dependency has the wrong ELF bitness for the host process. Install a
matching build first; restarting the unchanged files cannot fix it.

## Backward-compatible result fields

Existing `issues`, `warnings`, `error`, `message` and `requiresRestart` remain.
Inventory records additionally contain `diagnostics`, `pendingChange` and
`restartToApply`. Failed broker operations return a `diagnostic` object; settings
and notification APIs also include it. `injectionsGet.results[]` gains the same
object while preserving the rebuilder's original `state`, `code` and `message`.
UI-local navigation/registration rejections use the same object shape.

```json
{
  "code": "runtime-dlopen-failed",
  "causeCode": "elf-class-mismatch",
  "severity": "error",
  "category": "compatibility",
  "action": "replace-build",
  "summary": "Plugin or dependency ELF bitness does not match this process.",
  "recovery": "Install a build matching the host process and its dependencies.",
  "detail": "/lib/example.so: wrong ELF class: ELFCLASS32",
  "retryable": false
}
```

`summary` and `recovery` are English translation keys in the UI's `Diagnostics`
context. The UI owns English, Simplified Chinese and Traditional Chinese catalogs.
`detail` preserves the loader/system error; it must not be replaced by generic
advice. `causeCode` is empty when the underlying cause cannot be identified.
Dependency load errors retain the dependency ID in `code` and its loader detail.

## Levels and recovery

| Level/category | Meaning | Recovery |
|---|---|---|
| info / injection | Registered, processed, skipped or target not yet seen | No automatic repair; not proof of page rendering |
| warning / metadata | Incomplete/unverified metadata | Inspect if needed; not a broken-plugin claim |
| warning / runtime | Plugin refused loading or condition did not match | Check device/process/conditions |
| warning / concurrency | Busy, stale data, action pending | Refresh/retry after completion, or wait |
| warning / protection | Last entry/self-disable/self-remove rejected | Keep a working manager entry |
| error / compatibility | ELF bitness, ABI or environment mismatch | Install compatible builds |
| error / dependency | Missing/failed/incompatible dependencies, invalid QMD dependency graph | Restore dependencies and correct order |
| error / activation | Entry mismatch or conflicting path | Repair supported symlink state; preserve foreign files |
| error / configuration | Invalid persisted config | Back up and repair; never silently discard |
| error / page/request | Invalid page ownership/context or request | Correct the plugin integration/request |
| error / injection/resource | Patch or resource processing failed | Check firmware, patch, resources and original cause |
| error / filesystem | File operation failed | Check permissions, space, integrity and original error |
| error / unknown | Unrecognized error | Inspect; never assume restarting fixes it |

The same code can have different severity by context: `missing-manifest` in the
inventory is a warning, but a manifest-required operation fails. A failed
operation retains `ok:false` even for a warning such as stale content. No code
alone asserts a fatal host failure; that requires independent crash/startup
impact evidence. `critical` is reserved for such evidence, not guessed here.

`action` is advice, not a callable command or permission to mutate files. Only
`availableActions` advertises implemented package commands. Retryable means retry
after the stated prerequisite, not spin, sleep indefinitely or restart blindly.

## Restart is separate

- An active native plugin with a recorded load failure does not imply restart.
- A QMD activation mismatch needs entry repair, not a restart-only prompt.
- Explicit saved installation/activation changes still set pending restart state.
- A loaded native plugin removed from active entries still requires restart to
  unload. A newly activated, not-yet-loaded plugin can require restart to load.
- `pendingChange` records changes tracked in this session. External entry changes
  may also require restart, so it is not identical to `restartToApply`.
- If a previous failure and a pending change coexist, UI shows the failure first
  and describes the pending change separately in diagnostics. It does not claim
  the new files have already been validated.

Startup logs now report classified errors and warnings separately from the number
of packages blocked by enable preflight; a non-blocking load failure is not hidden
from the diagnostic count.

Enable preflight still validates on-disk metadata/dependencies separately from
runtime failure history. A historical load error must not prevent installing or
activating a corrected build. Restart/load success must be checked next session.

## Page errors and limits of feedback

Missing QML resources, incompatible properties and expired contexts have separate
recovery hints. Expired context -> reopen/check lifetime; missing component ->
restore component; incompatible page -> compatible plugin. There is no blanket
"update then restart" hint. Raw Qt details remain accessible.

Manager classification does not rewrite the rebuilder's raw state. In particular,
`shadowed-by-qrr` and `replacement-conflict` are warnings to inspect, even if the
raw diagnostic says `failed`. A prepared `no-matching-block` can still be replaced
by `resource-registered` by the current rebuilder; the manager cannot recover
lost history. Resource registration and QML object creation do not prove that
later bindings, timers or plugin code are correct. Arbitrary in-process code can
still block/crash the host.

See [the baseline audit](error-classification-audit.zh-CN.md) for the enumerated
codes and original UI/API reports. The implementation is in `src/diagnostics.h`;
recovery advice is centralized in manager, with UI-local validation handled in UI
so it works even when manager is unavailable.
