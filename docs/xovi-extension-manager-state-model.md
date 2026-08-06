# xovi-extension-manager State Model

This document explains the package states returned by
`xovi-extension-manager`, and how those states relate to XOVI's current runtime
state.

## 1. Two State Sources

Package state comes from two different sources:

1. Disk and manifest state: the manager scans `manifest.json`, package files,
   and active entries, which are normally symlinks.
2. XOVI runtime state: the XOVI `0.3.0` runtime API reports what the current
   `xochitl` process scanned and loaded.

These sources can temporarily disagree. For example, XOVI may have already
loaded a `.so`, and then the active symlink is changed on disk. The plugin can
keep working in the current process, while the manager reports a path
configuration issue.

## 2. XOVI Runtime Load States

`runtime.loadStateName` is mapped from the value returned by
`Environment->getExtensionLoadState()`. XOVI currently exposes these seven load
states:

| Value | `loadStateName` | Meaning |
| ---: | --- | --- |
| 1 | `discovered` | XOVI found the extension but has not finished initialization. |
| 2 | `dlopen-failed` | The dynamic library could not be opened or loaded. |
| 3 | `shouldload-failed` | The extension's `shouldLoad` check returned false. |
| 4 | `condition-failed` | A XOVI condition dependency was not satisfied. |
| 5 | `dependency-failed` | A XOVI runtime dependency failed to load. |
| 6 | `link-failed` | Import symbol resolution or linking failed. |
| 7 | `initialized` | The extension initialized successfully. |

The constants are defined in [`xovi.h`](../xovi.h), and the manager maps them
to names in `loadStateName()` in [`inventory.cpp`](../src/inventory.cpp).

If the manager does not find a XOVI runtime scan record for a package, it uses
its own sentinel state:

```json
{
  "seen": false,
  "loadState": -1,
  "loadStateName": "not-scanned"
}
```

`not-scanned` is not a XOVI load state. It means the manager did not receive a
scan result for that package from the XOVI API. The package may not be in the
active directory XOVI scans, or the runtime API may be unavailable or missing
that package.

## 3. Runtime Fields

The package `runtime` object has this shape:

```json
"runtime": {
  "seen": true,
  "loadState": 7,
  "loadStateName": "initialized",
  "loadError": "",
  "version": "0.1.0"
}
```

Fields:

- `seen`: whether the package appeared in XOVI's scanned extension list.
- `loadState`: numeric state returned by XOVI.
- `loadStateName`: manager-readable name for the numeric state.
- `loadError`: load error returned by XOVI. It is normally empty on success.
- `version`: plugin version recorded by XOVI.

The manager reads runtime state through these XOVI APIs:

- `getScannedExtensionCount()`
- `getScannedExtensionNames()`
- `getExtensionLoadState(id)`
- `getExtensionLoadError(id)`
- `getExtensionVersion(id, ...)`

The manager's `.xovi` file requires the `getScannedExtensionCount` condition,
so it requires XOVI `>=0.3.0`.

## 4. Manager Configuration and Path State

These fields describe disk configuration. They are not the same thing as
whether XOVI has already loaded a plugin in the current process:

- `enabled`: desired enabled state from `manifest.json`.
- `effectiveEnabled`: whether the manager sees an active entry that correctly
  points to the source entry.
- `activeEntryExists`: whether the active entry exists.
- `activeEntryMatches`: whether the active entry points to the package file
  declared by the manifest.
- `activeEntryConflict`: conflict reason when the active entry exists but
  points somewhere else.
- `requiresRestart`: whether the current process must restart before disk
  changes take effect.

For native `.so` extensions, the manager compares these default paths:

```text
source: /home/root/xovi/extensions.available/<id>/<entry>
active: /home/root/xovi/extensions.d/<id>.so
```

For QMD packages, the manager compares these default paths:

```text
source: /home/root/xovi/qmd.available/<id>/<entry>
active: /home/root/xovi/exthome/qt-resource-rebuilder/<order>-<id>.qmd
```

If the resolved symlink target is the same as the source path, or both paths
refer to the same file, the manager treats the active entry as matching.

## 5. Common Issues

### `effective-disabled`

Condition:

```text
manifest.enabled == true
and activeEntryMatches == false
```

This means the manifest wants the package enabled, but the manager cannot find
a valid active entry on disk.

### `active-entry-conflict`

Condition:

```text
active entry exists
but it is not a symlink to the source entry, or it points to another file
```

This means the active path is occupied by another file, or the package is using
a non-canonical layout.

### `effective-enabled-while-disabled`

The manifest says `enabled:false`, but the active entry still exists and points
to the expected source entry.

### Runtime Failures

When XOVI returns one of these failed states, the manager converts it into a
package issue:

- `dlopen-failed` -> `runtime-dlopen-failed`
- `shouldload-failed` -> `runtime-shouldload-failed`
- `condition-failed` -> `runtime-condition-failed`
- `dependency-failed` -> `runtime-dependency-failed`
- `link-failed` -> `runtime-link-failed`

## 6. Deciding Whether a Plugin Is Currently Working

Prefer checking runtime and configuration state together:

```text
runtime.seen == true
and runtime.loadStateName == "initialized"
```

This means XOVI initialized the plugin in the current process.

If `effective-disabled` or `active-entry-conflict` is also present, it means:

```text
The current xochitl process may still be using the already loaded plugin,
but the manager's disk configuration no longer matches the canonical path.
```

Do not treat this as proof that the plugin is currently unusable. A loaded `.so`
is not automatically unloaded from the current process when a symlink changes
after startup. The mismatch can still prevent the plugin from loading after the
next `xochitl` restart, so the path or manifest configuration should be fixed.

QMD packages do not use XOVI's `.so` load-state API. Do not use
`runtime.loadStateName` to decide whether a QMD has been applied. QMD state is
based on active entries, dependency checks, and restart state.

## 7. State Example

```json
{
  "enabled": true,
  "effectiveEnabled": false,
  "activeEntryExists": true,
  "activeEntryMatches": false,
  "activeEntryConflict": "active-entry-conflict",
  "runtime": {
    "seen": true,
    "loadState": 7,
    "loadStateName": "initialized",
    "loadError": ""
  },
  "issues": [
    "active-entry-conflict",
    "effective-disabled"
  ]
}
```

This means the manager considers the disk configuration invalid, but XOVI has
already loaded the plugin in the current process. Fix the active entry and
restart `xochitl` when needed; do not infer from `issues` alone that the plugin
is not running right now.

## 8. Recommended Actions

When handling states, first separate current-process runtime state from disk
configuration consistency. Do not decide that a plugin has stopped only because
`issues` is non-empty.

### `active-entry-conflict`

`active-entry-conflict` means the active entry expected by the manager already
exists, but does not point to the source entry declared in the manifest. Common
causes include:

- An external installer copied a `.so` file directly into `extensions.d/`.
- An external installer created a symlink to another directory.
- The plugin used an older directory layout or filename.
- Another plugin already owns the same active filename.
- XOVI loaded the plugin, and then the active entry was replaced or edited.

Recommended handling:

1. Check `sourceEntryPath`, `enabledEntryPath`, and `activeEntryTarget` to see
   which plugin file the active entry actually points to.
2. Check `runtime.loadStateName`. If it is `initialized`, the current process
   initialized the plugin successfully; fix the file layout and then schedule a
   `xochitl` restart.
3. If the plugin is an externally installed legacy plugin and should be managed
   by the manager, call `xovi-extension-manager$adopt`. The manager copies the
   plugin, creates a manifest, and converts it to a managed package.
4. If the plugin should not be converted to a managed package, verify that the
   active file really belongs to the target plugin, and make `entry`, `id`, and
   the path convention consistent with the actual layout.
5. If the active file belongs to another plugin, do not overwrite it directly.
   Resolve that plugin first, or use `disableLegacy` / `remove` only after
   confirming the legacy entry is no longer needed.
6. After fixing the layout, call `list` or `get` again and confirm
   `activeEntryMatches:true`. Restart `xochitl` when needed.

If the active entry is a regular file rather than a symlink, the manager does
not treat it as a canonical managed entry. Prefer adopting it, or have the
installer recreate it as a symlink to the canonical source entry.

### `effective-disabled`

This state means the manifest says `enabled:true`, but the manager cannot find
a valid active entry. Recommended handling:

- Confirm the source entry exists, and check for `entry-missing`.
- If the plugin was copied directly into `extensions.d/` by an external tool,
  use `adopt` instead of only editing the manifest.
- For managed packages, call `xovi-extension-manager$enable` so the manager
  creates the correct active entry.
- Confirm `effectiveEnabled:true` after the fix.
- If `runtime.loadStateName` is already `initialized`, do not assume the plugin
  has stopped immediately, but still fix the state before the next restart.

### `effective-enabled-while-disabled`

This state means the manifest says `enabled:false`, but the active entry still
exists and points to the expected source entry. Recommended handling:

- If the package should really be disabled, call `xovi-extension-manager$disable`.
- If the manifest was changed by mistake, restore `enabled:true` and leave the
  active entry in place.
- Because an already loaded `.so` is not automatically unloaded, disabling still
  normally requires a `xochitl` restart.

### `runtime-*-failed`

These issues come from XOVI runtime load results, not from manager path
inference. Recommended handling:

- Read `runtime.loadError` for the concrete error.
- Check the library file, architecture, XOVI version, conditions, and runtime
  dependencies based on the reported state.
- After fixing the problem, restart `xochitl`, then call `list` or `get` and
  confirm the state changed to `initialized`.
- Do not hide a XOVI load failure only by creating or editing active symlinks.

### `not-scanned`

`not-scanned` means the manager did not receive a XOVI scan record for the
package. Recommended handling:

- Confirm the plugin is in the active directory XOVI actually scans.
- Confirm the file extension, active entry name, and manifest `id` match.
- Confirm the XOVI version is at least `0.3.0`, and that the runtime API is not
  disabled.
- Restart `xochitl` and query again.
- If the package is still not scanned, check the XOVI startup log and the
  plugin's install layout.

### QMD Dependency Issues

QMD dependencies use `requires.qmd`. These issues appear in the package
`issues` returned by `list` / `get`. If a caller invokes `enable` while a
blocking issue exists, the API returns
`{"ok":false,"error":"enable-blocked","issues":[...]}`:

- `missing-qmd-dependency:<id>`: install the required QMD package first.
- `disabled-qmd-dependency:<id>`: explicitly enable the dependency first.
- `qmd-dependency-version-incompatible:<id>`: install a dependency version that
  satisfies the declared requirement.
- `qmd-dependency-order-invalid:<id>`: set the dependency to a lower `order`
  than the consumer.
- `qmd-dependency-cycle:<id>`: edit manifests to remove the direct or indirect
  cycle.
- `qmd-required-by:<id>`: disable the consumer package first, then disable or
  remove the dependency.

The manager does not silently enable dependencies, rewrite order, or cascade
disable consumer packages. Install may keep a package whose dependencies are not
yet satisfied; if install-time enable fails, the `install` response contains
`enableResult` and an `enable-after-install-blocked` warning.

### Recommended Migration Flow for External Installs

For plugins already installed into `extensions.d/` by another tool, use this
flow:

```text
1. Use list/get to inspect activeEntryTarget and runtime state.
2. Confirm the active file really belongs to the target plugin.
3. Use adopt to convert the legacy plugin into a managed package.
4. Run list/get again and confirm activeEntryMatches:true.
5. Restart xochitl according to requiresRestart.
```

If the file source is untrusted, ownership is unclear, or multiple active-path
conflicts exist, back up and inspect manually before calling `enable`. Do not
let `enable` overwrite an active entry whose ownership is unclear.
