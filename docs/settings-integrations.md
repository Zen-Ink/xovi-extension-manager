# Settings integrations

Open **Settings → Extensions**. The manager lists installed and runtime-discovered
plugins, their desired enablement, native load state, errors and restart needs.
Each registered provider page appears as a button on its plugin card. Native
provider pages are discovered only after the plugin has initialized. Disabled or
failed native plugins therefore keep their inventory entry but have no callable
settings provider. Existing standalone plugin entry points remain available.

| Plugin | Manager page | Configuration / backend |
| --- | --- | --- |
| Advanced Settings | Existing advanced settings interface | Existing AdvancedSettingsManager and configuration files |
| Keyboard CJK | Input schema, direct input, number row, physical keyboard behavior, candidate layout/count/position, shortcuts | Existing SettingsManager / RimeEngine, `settings.ini`; changes apply live |
| rm-librarian | Rescan library, progress and result/error | Existing `rescanLibrary`; shared asynchronous controller survives closing the page |
| epub-preloader | Queue, idle scheduling, status and diagnostics | Native EPUB worker; session-only queue, optional completion/error notifications; aarch64 3.28.0.172 prototype |
| rmfakecloud-control | Configuration, proxy service and connection status, endpoint, refresh | Existing RmFakeCloudBridge/helper protocol; status queries only |
| xovi-extension-manager | Plugin inventory, enable/disable, runtime and injection diagnostics | Built directly into Manager.qml |

These native integrations export the C settings provider descriptor. They do not
need a manifest `settings` entry or a runtime dependency on the manager. Pages
reuse the original backend rather than creating a second copy of plugin settings
in the manager's generic JSON store. See [the provider API](settings-provider-api.md)
for native and standalone QML provider details.

The provider getter only returns a descriptor; opening a page creates its QML.
Resource rebuild success, native initialization and successful settings-page
creation are separate states. A `ready` settings page is not proof that all later
QML bindings, network requests or user actions will succeed. Advanced Settings
also uses xochitl-private QML modules and must be exercised on the device.
Its Wi-Fi and language links use the manager settingsContext navigation API.
The manager entry forwards the system navigator on supported 3.27/3.28 firmware;
links are disabled if the host has no navigation capability. Standalone entry
points retain their original navigator.

## Build and package

The local `advanced_settings` and `keyboardcjk` paths may be symlinks to adjacent
projects. Keep those projects alongside this workspace; their changes belong to
their own repositories. Each consumer pins `https://github.com/Zen-Ink/xovi-extension-manager-sdk` as a `sdk` submodule, supplying `sdk/xovi-settings.h`.

Build all plugins and packages through the existing top-level entry:

```sh
make all
# Or one architecture:
./build.sh aarch64
```

Do not build both architectures concurrently in projects that generate shared
source files. Toolchains default to the root Makefile's `TOOLCHAIN_ROOT`.
Each plugin owns its build logic; the top-level Makefile only invokes it and
packages the result. Archives are written to `build/packages/<arch>/` and
contain a manifest and the matching native library;
install and enable them with the manager. Existing plugin data stays in place.
Dependencies such as xovi-message-broker, Keyboard CJK's RIME data and the
rmfakecloud helper/backend still need their normal installation. The archives
are plugin updates, not a replacement for those runtime dependencies.

EPUB Preloader is included only in the aarch64 integration build/package. Its
native settings provider is also discovered as a pinnable shortcut; sidebar and
bottom-bar pins stay user-controlled. Queue state belongs to the plugin and
survives closing its page, but resets on xochitl restart. Completion/error
notifications use the optional manager API, so the plugin has no mandatory
manager dependency. See [its usage and validation notes](../../epub-preloader/README.md).

## Verification

- `xovi-extension-manager/tests/run-settings-tests.sh`: discovery, settings store,
  UI reports, injection policy and QML-only package lifecycle.
- `xovi-extension-manager-ui/tests/run-host-tests.sh`: actual Qt page creation,
  invalid pages, stale completion/close protection, populated inventory,
  multiple provider buttons and load-error diagnostics.
- `keyboardcjk/tests/run_manager_settings_offscreen.sh`: actual settings QML with
  mocked plugin singletons; exercises the controls and backend calls.
- `rm-librarian/tests/settings-offscreen.pro`: actual Librarian and rmfakecloud
  settings pages with mocked modules; checks creation errors and QML warnings.
  From the workspace root:

```sh
test_build=$(mktemp -d)
qmake6 -o "$test_build/Makefile" rm-librarian/tests/settings-offscreen.pro
make -C "$test_build"
QT_QPA_PLATFORM=offscreen "$test_build/settings-offscreen" \
  "$PWD/rm-librarian/Settings.qml" \
  "$PWD/xovi-rmfakecloud-plugin/plugin/Settings.qml" \
  "$PWD/xovi-extension-manager-ui/tests/mocks"
```

Cross-compilation and host Qt tests do not validate xochitl's private modules or
real device operations. On-device checks should cover opening each page, changing
and reopening a Keyboard CJK setting, leaving/reopening Librarian during a scan,
and rmfakecloud status when its helper is present, missing or returns an error.

The interface now uses 36px black/white controls, SVG icons, native/QMD/QML tabs
and explicit pagination. Diagnostics are opened on demand. See
[launchers and notifications](launchers-and-notifications.md) for user-controlled
sidebar and bottom-bar pins and the current-session notification service.
