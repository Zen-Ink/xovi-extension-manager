# xovi-extension-manager Broker API

External callers reach `xovi-extension-manager` through `xovi-message-broker`.
This document describes the broker command format, response objects, and
manager signals.

For package authoring rules, see
`xovi-extension-manager-package-development.md`.

## Broker Command Format

All commands below are sent through the native-extension side of
`xovi-message-broker`:

```sh
cat /run/xovi-mb-out &
reader=$!
echo '>exovi-extension-manager$list:' > /run/xovi-mb
wait "$reader"
```

All manager handlers return JSON strings allocated for the broker to free.

Please avoid `echo ""> /run/xovi-mb; cat /run/xovi-mb-out` since it may cause race condition.

## Package JSON Object

`list`, `get`, `enable`, and `disable` return package details with this shape:

```json
{
  "type": "extension",
  "source": "manifest",
  "managed": true,
  "id": "example-extension",
  "name": "An example extension",
  "version": "0.1.0",
  "author": "nobody",
  "description": "Example extension",
  "license": "MIT",
  "entry": "example-extension.so",
  "order": 50,
  "sourceEntryPath": "$XOVI_ROOT/extensions.available/example-extension/example-extension.so",
  "enabledEntryPath": "$XOVI_ROOT/extensions.d/example-extension.so",
  "sourceEntryExists": true,
  "activeEntryExists": true,
  "activeEntryIsSymlink": true,
  "activeEntryMatches": true,
  "activeEntryTarget": "$XOVI_ROOT/extensions.available/example-extension/example-extension.so",
  "activeEntryConflict": "",
  "effectiveEnabled": true,
  "enabled": true,
  "hasManifest": true,
  "valid": true,
  "directoryPath": "$XOVI_ROOT/extensions.available/example-extension",
  "packagePath": "$XOVI_ROOT/extensions.available/example-extension",
  "dataPath": "$XOVI_ROOT/exthome/example-extension",
  "manifestPath": "$XOVI_ROOT/extensions.available/example-extension/manifest.json",
  "requires": {
    "xovi": ">=0.3.0",
    "extensions": {
      "qt-resource-rebuilder": ">=0.3.0"
    },
    "qmd": {},
    "xochitl": ["3.28.0.162"],
    "architectures": ["aarch64"]
  },
  "runtime": {
    "seen": true,
    "loadState": 7,
    "loadStateName": "initialized",
    "loadError": "",
    "version": "0.1.0"
  },
  "requiresRestart": false,
  "warnings": [],
  "errors": [],
  "issues": [],
  "availableActions": ["inspect", "disable", "remove"]
}
```

For user-installed files without `manifest.json`, the manager derives `id` and
`name` from the active filename. Extension files use `<id>.so`; QMD files use
either `<id>.qmd` or `<zero-padded-order>-<id>.qmd`. These packages return
`"source":"legacy"`, `"managed":false`, `"hasManifest":false`, and normally
offer `["inspect","adopt","disableLegacy","remove"]`.

Common `issues` values include `manifest-invalid`, `entry-missing`,
`active-entry-conflict`, `effective-disabled`,
`effective-enabled-while-disabled`, `missing-manifest`, `unmanaged`,
`xovi-version-incompatible`, `missing-dependency:<id>`,
`disabled-dependency:<id>`, `dependency-version-incompatible:<id>`,
`missing-qmd-dependency:<id>`, `disabled-qmd-dependency:<id>`,
`qmd-dependency-version-incompatible:<id>`,
`qmd-dependency-order-invalid:<id>`, `qmd-dependency-cycle:<id>`,
`qmd-required-by:<id>`, `architecture-mismatch`,
`xochitl-version-incompatible`,
`missing-runtime-dependency:qt-resource-rebuilder`,
`qmd-order-conflict:<id>`, and runtime load failures such as
`runtime-dlopen-failed`.

Dependency and compatibility failures are reported through each package's
`issues` array. `enable` rejects packages with blocking issues:

```json
{"ok":false,"error":"enable-blocked","message":"package has blocking issues","issues":["missing-qmd-dependency:scroll-screen-up-or-down"]}
```

Disabling or removing a QMD with enabled reverse dependencies is blocked:

```json
{"ok":false,"error":"qmd-required-by","message":"QMD package is required by enabled consumers","issues":["qmd-required-by:navigate-using-arrow-keys"]}
```

`install` normally treats dependency issues as warnings and still installs the
package disabled. If the caller requests `enabled:true` and the package cannot be
enabled, the response includes `warnings:["enable-after-install-blocked"]` and
an `enableResult` object with the same `enable-blocked` shape. An install or
upgrade that would invalidate already enabled QMD consumers is rejected with
`error:"qmd-required-by"` and a `dependencyIssues` array.

When `xochitl` starts and XOVI initializes the manager, it performs one
inventory scan and logs enabled packages that currently have blocking dependency
or compatibility issues. This scan is diagnostic only; it does not rewrite
manifests or symlinks.

## User-Facing Caveats

- `install` is an apply-now operation. The manager intentionally has no built-in
  confirmation prompt; UI and CLI callers should confirm before invoking it.
- Dependency and compatibility issues can be warnings at install time. `enable`
  is the operation that blocks unmet dependencies.
- Installing the same package id uses one canonical target and returns
  `installMode` as `new`, `reinstall`, `upgrade`, `downgrade`, or `replace`.
  A changed package type for the same id is rejected.
- Archive installs use the managed package layout: the archive root contains
  `manifest.json` and the entry file declared by `manifest.entry`. The entry is
  a safe relative path under the archive root; it may be a flat filename or use
  subdirectories.
- Installing over an unmanaged legacy package id returns
  `installed-legacy-conflict`; callers should ask the user to adopt or disable
  the legacy file first.
- Active `.so` and `.qmd` files without manifests are reported as unmanaged
  legacy packages. Their ids are derived from filenames.
- `adopt` converts a legacy active file into a managed package. `disableLegacy`
  removes a legacy symlink or moves a regular legacy active file into
  `exthome/xovi-extension-manager/legacy-disabled/`.
- Enable and disable require a manual `xochitl` restart before native extension
  and QMD changes take effect.
- Disable is not uninstall. It only updates `manifest.enabled` and active
  symlinks; installed package files and runtime data remain on disk.
- `remove` deletes managed packages and clears unmanaged legacy packages by
  moving their files/directories into `legacy-disabled`. Managed remove does not
  delete `$XOVI_ROOT/exthome/<id>` runtime data.
- The manager package itself cannot be disabled or removed through the ordinary
  APIs. Installing `xovi-extension-manager` forces it to remain enabled and
  preserves the previous self package under
  `$XOVI_ROOT/exthome/xovi-extension-manager/state/previous/xovi-extension-manager/`.
- Active symlink conflicts are reported as `active-entry-conflict` and block
  enable.
- QMD order conflicts are reported as `qmd-order-conflict:<id>` issues so
  callers can warn the user.

## Broker API

| Signal | Purpose | Input after `:` | Expected JSON return format | Example |
| --- | --- | --- | --- | --- |
| `xovi-extension-manager$list` | Return the complete package inventory plus XOVI runtime state. | Ignored. Use an empty value. | Success: `{"ok":true,"root":"...","architecture":"...","xoviVersion":"...","xochitlVersion":"...","count":2,"packages":[<package>],"extensions":[<package>],"qmd":[<package>]}`. | Command: `echo '>exovi-extension-manager$list:' > /run/xovi-mb`<br>Sample response: `{"ok":true,"root":"$XOVI_ROOT","count":2,"packages":[...],"extensions":[...],"qmd":[...]}` |
| `xovi-extension-manager$get` | Return one package object. | Package `id` or package directory name. Whitespace is trimmed. | Success: `{"ok":true,"package":<package>,"extension":<package>}`. `extension` is a compatibility alias. Error: `{"ok":false,"error":"not-found","message":"package was not found"}`. | Command: `echo '>exovi-extension-manager$get:example-extension' > /run/xovi-mb`<br>Sample response: `{"ok":true,"package":{"id":"example-extension",...},"extension":{...}}` |
| `xovi-extension-manager$enable` | Set `manifest.enabled` to `true` and create the active symlink for an extension or QMD package. | Package `id` or package directory name. | Success: `{"ok":true,"type":"extension","id":"example-extension","enabled":true,"actions":["manifest-enabled","active-entry-symlink-created"],"requiresRestart":true,"restartTarget":"xochitl","extension":<package>}`. Blocking failures return `{"ok":false,"error":"enable-blocked","issues":[...]}`. | Command: `echo '>exovi-extension-manager$enable:example-extension' > /run/xovi-mb`<br>Sample response: `{"ok":true,"enabled":true,"actions":["manifest-enabled","active-entry-already-present"],"requiresRestart":true,"extension":{...}}` |
| `xovi-extension-manager$disable` | Set `manifest.enabled` to `false` and remove the active symlink when that path is a matching symlink. | Package `id` or package directory name. | Success: `{"ok":true,"type":"qmd","id":"hide-dev-icon","enabled":false,"actions":["manifest-disabled","active-entry-symlink-removed"],"requiresRestart":true,"restartTarget":"xochitl","extension":<package>}`. A QMD required by enabled consumers returns `{"ok":false,"error":"qmd-required-by","issues":["qmd-required-by:<consumer>"]}`. The manager itself returns `self-disable-blocked`. Other errors use the usual `ok:false` shape. | Command: `echo '>exovi-extension-manager$disable:hide-dev-icon' > /run/xovi-mb`<br>Sample response: `{"ok":true,"type":"qmd","enabled":false,"actions":["manifest-disabled","active-entry-symlink-removed"],...}` |
| `xovi-extension-manager$install` | Install a single `.so`/`.qmd` file or a `.tar.gz`/`.tgz`/`.zip` package. Archives must contain `manifest.json` and the file at `manifest.entry`; single files may use `<file>.manifest.json` or a sibling `manifest.json`, otherwise a minimal manifest is generated. | Plain path, or JSON such as `{"path":"/tmp/hide-dev-icon.zip","enabled":true}`. Missing `enabled` defaults to `false` for single files and to the manifest value for packages that provide one; self installs are forced enabled. | Success: `{"ok":true,"type":"qmd","id":"hide-dev-icon","installMode":"new","desiredEnabled":true,"actions":["archive-extracted","package-files-copied","enabled-after-install"],"warnings":[],"requiresRestart":true,"restartTarget":"xochitl","enableResult":{...},"package":<package>}`. Dependency issues are warnings at install time; if requested enable fails, `enableResult` contains `enable-blocked`. Errors include `missing-path`, `not-found`, `unsupported-package`, `missing-manifest`, `invalid-manifest`, `installed-type-conflict`, `installed-legacy-conflict`, `qmd-required-by`, `qmd-dependencies-invalid`, `copy-failed`, and `extract-failed`. | Command: `echo '>exovi-extension-manager$install:{"path":"/tmp/hide-dev-icon.zip","enabled":true}' > /run/xovi-mb`<br>Sample response: `{"ok":true,"type":"qmd","id":"hide-dev-icon","installMode":"new","package":{...}}` |
| `xovi-extension-manager$adopt` | Convert an unmanaged legacy active `.so` or `.qmd` into a managed package. The manager copies the entry into the canonical package directory, writes `manifest.json`, disables the legacy active entry, then optionally enables the managed package. | Package `id`, active path, or JSON such as `{"path":"$XOVI_ROOT/extensions.d/example-extension.so","type":"extension","id":"example-extension","enabled":true}`. Optional JSON keys: `name`, `version`, `entry`, `order`, `manifestPath`. Arbitrary inactive files should use `install`, not `adopt`. | Success: `{"ok":true,"type":"extension","id":"example-extension","actions":["legacy-entry-copied","minimal-manifest-created","legacy-active-file-moved","enabled-after-adopt"],"preservedPath":"...","desiredEnabled":true,"requiresRestart":true,"restartTarget":"xochitl","enableResult":{...},"package":<package>}`. Errors include `already-managed`, `installed-conflict`, `not-active-legacy`, `entry-conflict`, `manifest-id-mismatch`, and `adopt-enable-blocked`. | Command: `echo '>exovi-extension-manager$adopt:example-extension' > /run/xovi-mb`<br>Sample response: `{"ok":true,"id":"example-extension","actions":[...],"package":{"managed":true,...}}` |
| `xovi-extension-manager$disableLegacy` | Disable an unmanaged legacy active file without adopting it. Symlinks are removed; regular files are moved into `exthome/xovi-extension-manager/legacy-disabled/<type>/` so they are preserved. | Legacy package `id` or active path. JSON with `{"id":"example-extension","type":"extension"}` is also accepted. | Success: `{"ok":true,"type":"extension","id":"example-extension","actions":["legacy-active-file-moved"],"preservedPath":"$XOVI_ROOT/exthome/xovi-extension-manager/legacy-disabled/extensions/example-extension.so","requiresRestart":true,"restartTarget":"xochitl"}`. `requiresRestart` is `false` if the active entry was already absent. Managed packages return `not-legacy`. | Command: `echo '>exovi-extension-manager$disableLegacy:$XOVI_ROOT/extensions.d/example-extension.so' > /run/xovi-mb` |
| `xovi-extension-manager$remove` | Remove either a managed package or an unmanaged legacy package. Managed packages are deleted from the canonical layout. Legacy active files/directories are moved into `legacy-disabled` so the id can be installed again. | Package `id`, active path, or JSON such as `{"id":"hide-dev-icon","type":"qmd"}`. | Managed success: `{"ok":true,"removedKind":"managed","type":"qmd","id":"hide-dev-icon","actions":["active-entry-symlink-removed","package-directory-removed"],"requiresRestart":true,"restartTarget":"xochitl","package":<removed-package>}`. Legacy success: `{"ok":true,"removedKind":"legacy","actions":["legacy-active-symlink-removed","legacy-directory-moved"],"preservedPaths":["..."],"requiresRestart":true,...}`. Errors include `self-remove-blocked`, `unsafe-package-path`, `active-entry-conflict`, and `unsafe-active-entry`. | Command: `echo '>exovi-extension-manager$remove:hide-dev-icon' > /run/xovi-mb` |
| `xovi-extension-manager$requiresRestart` | Return whether package state in the current `xochitl` process needs a manual restart. | Ignored. Use an empty value. | Success: `{"ok":true,"requiresRestart":true,"restartTarget":"xochitl","pendingPackages":[{"type":"extension","id":"example-extension","enabled":true,"effectiveEnabled":true,"issues":[]}]}`. | Command: `echo '>exovi-extension-manager$requiresRestart:' > /run/xovi-mb`<br>Sample response: `{"ok":true,"requiresRestart":false,"restartTarget":"xochitl","pendingPackages":[]}` |
| `xovi-extension-manager$schema` | Return the current manifest schema summary exposed by the manager. | Ignored. Use an empty value. | Success: `{"ok":true,"manifestFiles":{"extension":"...","qmd":"..."},"packageFiles":{"extension":"...","qmd":"..."},"dataDirectories":{...},"installSupports":[".so",".qmd",".tar.gz",".tgz",".zip"],"required":["id"],"defaults":{...},"hardErrors":[...],"examples":{...}}`. | Command: `echo '>exovi-extension-manager$schema:' > /run/xovi-mb`<br>Sample response: `{"ok":true,"manifestFiles":{"extension":"$XOVI_ROOT/extensions.available/<id>/manifest.json",...},...}` |
| `xovi-extension-manager$health` | Return a small readiness check with root path and inventory count. | Ignored. Use an empty value. | Success: `{"ok":true,"root":"...","count":1}`. | Command: `echo '>exovi-extension-manager$health:' > /run/xovi-mb`<br>Sample response: `{"ok":true,"root":"$XOVI_ROOT","count":1}` |
