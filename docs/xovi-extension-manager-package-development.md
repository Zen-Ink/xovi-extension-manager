# xovi-extension-manager Package Development

This document describes how extension and QMD packages should be prepared for
`xovi-extension-manager`.

`xovi-extension-manager` treats `manifest.json` as package metadata and XOVI's runtime scan API as current-process state. It does not load plugin `.so` files to discover metadata. It also scans already-active `.so` and `.qmd` files that have no manifest and reports them as unmanaged legacy packages.

The manager itself requires the modified XOVI runtime API `>=0.3.0`. Its `.xovi` file declares a runtime condition on the XOVI 0.3 scan-state API, so older XOVI builds unload the manager before initialization.

However, the modified xovi do support original xovi API 2.0 extensions. So in order to migration, simply generate a manifest.json and package is enough.

## Extension Package Development

An extension package is a XOVI-loaded `.so` plus a `manifest.json`.

Recommended package directory:

```text
example-extension/
  manifest.json
  example-extension.so
  assets/
```

Recommended install location:

```text
$XOVI_ROOT/extensions.available/<id>/
```

Enabled extension entry:

```text
$XOVI_ROOT/extensions.d/<id>.so
  -> $XOVI_ROOT/extensions.available/<id>/<entry>
```

XOVI scans active extension entries under `$XOVI_ROOT/extensions.d/` when
`xochitl` starts. The manager stores installed package contents under
`$XOVI_ROOT/extensions.available/<id>/` and creates an active symlink only when
the package is enabled. This separates installed state, active state, and
runtime data:

- `extensions.available/<id>/manifest.json` stores package metadata.
- `extensions.available/<id>/` stores read-only program files and release
  resources; `manifest.entry` points to the actual `.so` entry relative to this
  directory.
- `extensions.d/<id>.so` remains the small active entry that XOVI discovers.
- `exthome/<id>/` is reserved for plugin-owned runtime data such as config,
  cache, and user-created state.
- Disable can remove only the managed symlink while leaving package files and
  runtime data intact.
- Enable can detect active-entry conflicts instead of overwriting a manually
  installed `.so`.
- Upgrade/remove operations can reason about package ownership from the
  manifest path rather than from a bare active file.

XOVI also exposes an API for native plugins to locate their own extension data
directory:

```c
char *home = Environment->getExtensionDirectory("example-extension");
```

In the current XOVI layout this resolves to the extension home directory for the
given family, conventionally `$XOVI_ROOT/exthome/<family>`. Package authors
should pass the same stable value used as the manager package `id`, and must
store plugin-owned runtime data under that directory. The manager does not treat
`exthome/<id>/` as package payload, so reinstall, upgrade, and remove do not
delete runtime data by default.

`getExtensionDirectory()` does not read `manifest.json` and does not infer the
manager package id. If a plugin passes a different family string, XOVI will
resolve a different extension home path, so package authors should keep `.xovi`
metadata, manifest `id`, active entry name, and `getExtensionDirectory()`
argument consistent.

Development flow:

1. Define XOVI metadata in a `.xovi` file.
2. Generate XOVI glue and, when possible, the manifest from the same `.xovi`
   source:

   ```sh
   python3 "$XOVI_REPO/util/xovigen.py" \
     -o xovi.cpp \
     -H xovi.h \
     -m manifest.json \
     --manifest-entry example-extension.so \
     example-extension.xovi
   ```

3. Build the extension as a shared object with the reMarkable cross toolchain.
4. Package `manifest.json`, the `.so` entry named by `manifest.entry`, and any
   read-only release resources under the package root. Do not ship mutable
   runtime data in the package archive.

### XOVI Metadata and APIs

The `.xovi` file is the authoritative XOVI runtime contract for a native
extension. The manager consumes generated manifest metadata, while XOVI consumes
the generated glue and runtime metadata.

Common `.xovi` entries:

| Entry | XOVI role | Manager/package role |
| --- | --- | --- |
| `version major.minor.patch` | Declares the runtime plugin version reported by XOVI. | Becomes package `version` in generated manifests. |
| `depends-on module:major.minor.patch` | Hard XOVI initialization dependency. If it is not satisfied, XOVI can fail or unload the dependent plugin during startup. | Becomes `requires.extensions[module] = ">=major.minor.patch"` in generated manifests. Use only when the extension cannot safely initialize without that module. |
| `import` | Imports symbols exported by another XOVI module. | Usually paired with a real runtime dependency. |
| `export` | Exposes symbols to other XOVI modules. | Can be documented with package `provides`, but `provides` is informational. |
| `override` | Declares functions the extension overrides in xochitl or another module. | No direct manager behavior; compatibility should be captured through `requires.xochitl` and `requires.architectures`. |
| `condition` | Controls whether XOVI should load the extension in the current runtime. | The manager reports runtime load state, but does not evaluate arbitrary XOVI conditions itself. |
| `resource` | Declares generated or embedded resources for the extension. | Package authors should ship read-only resources under the package root; mutable runtime data belongs under `exthome/<id>/`. |
| `global-meta` | Carries extra metadata for tooling. | Can be used for manifest-only manager metadata such as softer dependency declarations. |

For dependencies that should be enforced by the extension manager but must not
force xochitl to exit during XOVI initialization, use manifest-only metadata:

```text
global-meta
    requiresExtensions = "xovi-message-broker:>=0.2.0"
end
```

## QMD Package Development

A QMD package is a standalone QMLDiff package. It has no XOVI `.so`; it is
applied by `qt-resource-rebuilder`.

Recommended package directory:

```text
hide-dev-icon/
  manifest.json
  HideDevIcon.qmd
```

Recommended canonical location:

```text
$XOVI_ROOT/qmd.available/<id>/
```

Recommended active QMD link:

```text
$XOVI_ROOT/exthome/qt-resource-rebuilder/<zero-padded-order>-<id>.qmd
  -> ../../qmd.available/<id>/<entry>
```

QMD packages should declare:

```json
{
  "manifestVersion": 1,
  "type": "qmd",
  "id": "hide-dev-icon",
  "name": "Hide Developer Icon",
  "version": "0.1.0",
  "entry": "HideDevIcon.qmd",
  "enabled": false,
  "order": 50,
  "requires": {
    "extensions": {
      "qt-resource-rebuilder": ">=0.3.0"
    },
    "qmd": {},
    "xochitl": ["3.28.x.x"],
    "architectures": ["aarch64"]
  }
}
```

`requires.qmd` declares hard dependencies on other managed QMD packages. Its
keys are stable package `id` values, not QMD filenames, paths, display names, or
capability names. The package that consumes another QMD declares the
dependency; the dependency package does not list its consumers.

For example, a navigation QMD that calls behavior introduced by
`scrollScreenUpOrDown.qmd` should depend on the package id
`scroll-screen-up-or-down`. The dependency package may keep the upstream
filename as its `entry`:

```json
{
  "manifestVersion": 1,
  "type": "qmd",
  "id": "scroll-screen-up-or-down",
  "name": "Scroll Screen Up Or Down",
  "version": "0.1.2",
  "entry": "scrollScreenUpOrDown.qmd",
  "enabled": false,
  "order": 50,
  "requires": {
    "extensions": {
      "qt-resource-rebuilder": ">=0.3.0"
    },
    "qmd": {},
    "xochitl": ["3.28.x.x"],
    "architectures": []
  }
}
```

The consuming package declares the relationship and uses a later order:

```json
{
  "manifestVersion": 1,
  "type": "qmd",
  "id": "navigate-using-arrow-keys",
  "name": "Navigate Using Arrow Keys",
  "version": "0.1.2",
  "entry": "navigateUsingArrowKeys.qmd",
  "enabled": false,
  "order": 60,
  "requires": {
    "extensions": {
      "qt-resource-rebuilder": ">=0.3.0"
    },
    "qmd": {
      "scroll-screen-up-or-down": ">=0.1.2"
    },
    "xochitl": ["3.28.x.x"],
    "architectures": []
  }
}
```

QMD dependency rules are strict:

- The dependency must be installed as a valid managed package with
  `type: "qmd"`.
- Its package version must satisfy the declared requirement.
- It must be enabled and have a matching active entry before the consumer can
  be enabled.
- Its `order` must be lower than the consumer's `order`; package authors must
  not rely on the alphabetical id tie-breaker.
- Direct and indirect dependency cycles are invalid.
- Enabling a consumer does not silently enable dependencies.
- Disabling or removing a QMD is blocked while enabled consumers require it.

Install may store a package whose QMD dependencies are not yet present, but it must remain disabled. Enable is the point at which missing, disabled, incompatible, cyclic, and incorrectly ordered QMD dependencies become blocking errors.

Cross-package `LOAD` paths are not a dependency mechanism. Use `LOAD` only for files shipped inside the same package. If a helper QMD is an inseparable implementation detail used by one package, ship it in that package instead of creating a separately managed dependency.

Capability metadata such as `provides` describes what a package offers; it is not a substitute for `requires.qmd`. This specification intentionally resolves QMD dependencies by exact package id so that multiple capability providers do not make dependency resolution ambiguous.

`qt-resource-rebuilder`/`qmldiff` does not read dependency metadata from QMD files. At startup it scans active `.qmd` files and sorts the filenames before loading them. The manager therefore encodes load order in the active filename:
`<zero-padded-order>-<id>.qmd`. Lower `order` values load earlier; when two packages use the same `order`, the sorted filename makes `id` the practical tie-breaker. Same-order packages are reported as `qmd-order-conflict:<id>` so a caller can warn the user.

The manager scans QMD packages from
`$XOVI_ROOT/qmd.available/*/manifest.json`.
`enable` and `disable` manage the active QMD symlink under
`$XOVI_ROOT/exthome/qt-resource-rebuilder/`.

## Manifest Metadata

| Field | Type | How `xovi-extension-manager` uses it |
| --- | --- | --- |
| `manifestVersion` | number | Defaults to `1` when missing and adds `missing-manifestVersion` to `warnings`. |
| `type` | string: `extension` or `qmd` | Used to validate `entry` suffix. Missing values add `missing-type`; the manager infers from `.so` or `.qmd` when possible. |
| `id` | string | Required package identifier. It must only contain `A-Za-z0-9._-` and cannot be `.` or `..`. `get`, `enable`, and `disable` match by `id` or directory name. |
| `name` | string | Display name. Defaults to `id` and adds `missing-name`. |
| `version` | string | Package version. Used for dependency checks by other packages. Defaults to `unknown` and adds `missing-version`. |
| `author` | string | Returned in package JSON. Empty when missing. |
| `description` | string | Returned in package JSON. Empty when missing. |
| `license` | string | Returned in package JSON. Empty when missing. |
| `entry` | string | Safe relative path to the package entry file under the package root. It may be a flat filename such as `example-extension.so` or a subdirectory path such as `lib/example-extension.so`. It must not be absolute and must not contain empty path segments, `.`, or `..`. Extension entries must end in `.so`; QMD entries must end in `.qmd`. Missing extension entries default to `<id>.so`; missing QMD entries are an error. |
| `enabled` | boolean | Desired enabled state. Defaults to `false` and adds `missing-enabled`. Current `enable` and `disable` calls rewrite this field directly in `manifest.json`. |
| `order` | number | Parsed for QMD ordering, emitted in package JSON, and defaults to `50`. Valid QMD orders are `0` through `999`. |
| `requires.xovi` | string | Compared with the compiled XOVI API version. Supported operators are `>=`, `>`, `=`, or exact semver. Missing values add `missing-requires.xovi`. |
| `requires.extensions` | object of extension id to version requirement | Hard dependencies on XOVI `.so` extension packages. QMD package ids must not be placed here. |
| `requires.qmd` | object of QMD package id to version requirement | Hard dependencies on managed QMD packages. Enable requires each dependency to be valid, version-compatible, effectively enabled, acyclic, and ordered before the consumer. |
| `requires.xochitl` | string array | If non-empty and the current xochitl version is known, at least one entry must match. Prefer lowercase `x` as a complete dot-delimited segment, such as `3.28.x.x`; the pattern and detected version must have the same number of segments. Exact versions remain supported. For compatibility with existing packages, `*` retains its simple character-level wildcard behavior and a single `*` matches any known xochitl version. |
| `requires.architectures` | string array | If non-empty and the current architecture is known, at least one entry must match. `aarch64` and `arm64` are treated as aliases. |
| Other keys | any | Ignored by the current manager unless future code adds support. `xovigen.py` may emit `homepage` and `source`, but they are not returned by the current broker responses. Legacy top-level fields such as `xoviApi`, `dependencies`, `xochitlVersions`, and top-level `architectures` are ignored; use the `requires` object instead. |

The manager does not infer or prepend any implicit payload directory. If the
entry file is stored under a subdirectory, include that subdirectory in
`manifest.entry`, for example `package/framebuffer-spy.so`. Alternatively, a
package may provide `manifest.entry` as a symlink that resolves to the real
entry file; the manager treats the active entry as matching when both paths
resolve to the same file.

Dependency and compatibility failures are reported through each package's `issues` array. `enable` rejects packages with blocking issues such as invalid manifest, missing entry, incompatible XOVI/xochitl/architecture, active symlink conflict, missing dependency, disabled dependency, or incompatible dependency
version.

The same `requires.xochitl` matching rules apply to extension and QMD packages.
For example, `["3.27.x.x", "3.28.x.x"]` accepts four-segment releases such as
`3.27.0.100` and `3.28.0.163`, but does not accept `3.28.0` because the segment
count differs. Lowercase `x` is special only when the whole segment is exactly
`x`; forms such as `3.28.0.16x` do not match as wildcards. Exact strings still
match, while `["*"]` means the package declares no xochitl version restriction
as long as the current version can be detected. Empty strings do not match.
