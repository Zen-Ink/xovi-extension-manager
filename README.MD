# xovi-extension-manager

`xovi-extension-manager` provides manifest-based inventory, install, and enable/disable control for XOVI native extensions and standalone QMD packages used by `qt-resource-rebuilder`.

It treats `manifest.json` as static package metadata and XOVI runtime APIs as
current-process load state. It does not load plugin `.so` files to inspect metadata.

Please note that the manager requires the modified XOVI runtime API `>=0.3.0`,
and asivery's `xovi-message-broker` to support API calling.

The required XOVI source is tracked as a git submodule under `xovi/`.
Initialize it after cloning:

```sh
git submodule update --init --recursive
```

## Managed Layout

Native extension packages are stored under `extensions.available/<id>/` and
enabled through a symlink in `extensions.d/`:

```text
/home/root/xovi/extensions.available/<id>/
  manifest.json
  <entry>.so

/home/root/xovi/extensions.d/<id>.so
  -> ../extensions.available/<id>/<entry>.so
```

Standalone QMD packages are stored under `qmd.available/<id>/` and enabled
through `qt-resource-rebuilder`'s active directory:

```text
/home/root/xovi/qmd.available/<id>/
  manifest.json
  <entry>.qmd

/home/root/xovi/exthome/qt-resource-rebuilder/<zero-padded-order>-<id>.qmd
  -> ../../qmd.available/<id>/<entry>.qmd
```

`manifest.entry` is the entry file path relative to the package directory. It
may be a flat filename or a safe relative path with subdirectories; absolute
paths, empty path segments, `.`, and `..` are invalid.

`exthome/<id>/` is reserved for plugin runtime data such as config, cache, and
user-created state. Reinstall, upgrade, and remove operate on `*.available`
package directories and do not delete `exthome/<id>` unless a future explicit
purge operation is added.

## Broker API

The manager exports these signals through `xovi-message-broker`:

| Signal | Purpose |
| --- | --- |
| `xovi-extension-manager$list` | Return inventory plus XOVI runtime state. |
| `xovi-extension-manager$get` | Return one package by id or directory name. |
| `xovi-extension-manager$enable` | Set `manifest.enabled = true` and create the active symlink. |
| `xovi-extension-manager$disable` | Set `manifest.enabled = false` and remove the managed active symlink. |
| `xovi-extension-manager$install` | Install a `.so`, `.qmd`, `.tar.gz`, `.tgz`, or `.zip` package. |
| `xovi-extension-manager$adopt` | Convert an unmanaged active `.so` or `.qmd` into a managed package. |
| `xovi-extension-manager$disableLegacy` | Disable an unmanaged active file without adopting it. |
| `xovi-extension-manager$remove` | Remove a managed package or clear an unmanaged legacy package. |
| `xovi-extension-manager$requiresRestart` | Report whether a manual `xochitl` restart is needed. |
| `xovi-extension-manager$schema` | Return the manager's manifest schema summary. |
| `xovi-extension-manager$health` | Return a small readiness check. |

Response-bearing commands should open `/run/xovi-mb-out` for reading before or immediately after writing to `/run/xovi-mb`:

```sh
cat /run/xovi-mb-out &
reader=$!
echo '>exovi-extension-manager$list:' > /run/xovi-mb
wait "$reader"
```

For full request/response formats and error codes, see
[`docs/xovi-extension-manager-broker-api.md`](docs/xovi-extension-manager-broker-api.md).

## Documentation

- [`docs/xovi-extension-manager-broker-api.md`](docs/xovi-extension-manager-broker-api.md):
  command syntax, response objects, user-facing caveats, and Broker API details.
- [`docs/xovi-extension-manager-package-development.md`](docs/xovi-extension-manager-package-development.md):
  native extension packages, QMD packages, manifest fields, and dependency
  rules.
- [`docs/xovi-extension-manager-state-model.md`](docs/xovi-extension-manager-state-model.md):
  runtime load states, disk state, issues, and recommended operations.
