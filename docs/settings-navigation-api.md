# Open another plugin's settings

Extension Manager UI provides an optional navigation service for native and QML
plugins. It checks the requested page against the manager's discovered providers
and schedules navigation on the GUI thread. Callers do not load the other page's
QML themselves or depend on its internal resource paths.

## Hosted settings pages

```qml
var result = settingsContext.openSettings("keyboardcjk", "main")
if (!result.accepted) {
    // Show a translated explanation in the caller's own UI.
    console.warn(result.error)
}
```

Advanced Settings uses this API for its IM page's Keyboard CJK button. Calls do
not depend on the legacy Rime library-path detector. Unsaved caller changes are
not automatically applied or discarded by the navigation service.

## Other QML pages

```qml
import org.xovi.Manager 1.0
// In a signal handler:
// var result = ManagerNavigation.requestOpenSettings("keyboardcjk", "main")
```

Both methods return the same object and default `pageId` to `main`. The original
`ManagerNavigation.openSettings` signal helper is reserved for firmware adapters;
external callers should use the validated `requestOpenSettings` method.

## Native C API

Include `sdk/xovi-navigation.h`. Discover a getter whose XOVI metadata key is
`xovi-extension-manager-ui$navigationApi` and integer value is `1`. Ensure its
extension is initialized before invoking it. Check `abiVersion == 1` and
`structSize >= sizeof(XemNavigationApiV1)` before accessing the table.

```c
const XemNavigationApiV1 *api = getter();
char *reply = api->openSettings("keyboardcjk", "main");
/* Parse UTF-8 JSON; handle accepted=false. */
api->freeString(reply);
```

`openSettings` is safe from worker threads and does not wait for GUI execution.
Strings are copied before return; the returned JSON must be freed with this
API's `freeString`. A null `pageId` selects `main`. If the metadata getter is
absent, the UI is not installed/loaded; handle that without a hard dependency.
The table remains valid for the process lifetime.

## Broker

Signal: `xovi-extension-manager-ui$openSettings`, version 1.

```json
{"ownerId":"keyboardcjk","pageId":"main"}
```

Use the broker's native broadcast API and free its response under the broker's
normal ownership rules. Provider IDs and canonical manifest package IDs are
accepted; the reply identifies the resolved provider.

## Results

```json
{"ok":true,"accepted":true,"state":"queued","ownerId":"keyboardcjk","pageId":"main"}
```

This acknowledges a queued request, not successful page creation. No blocking
GUI call is made. Actual loading/ready/failed states remain in `settingsList`'s
`uiStates["ownerId/pageId"]`. If the UI disappears after acceptance, queued work
may be cancelled. The opened page receives `launchSource == "plugin"` and closes
through the existing shortcut return behavior; this API does not promise an
in-place back stack into the caller's page.

Rejected calls return `ok:false`, `accepted:false` and an `error`:

- `invalid-id` / `invalid-request`: invalid IDs or broker JSON.
- `ui-not-ready`: no live main-view launcher is registered yet.
- `manager-unavailable`: discovery could not be queried.
- `page-not-found`: no matching provider page.
- `page-unavailable`: the matching page is disabled or unavailable.

Special manager targets are `xovi-extension-manager/inventory` and
`xovi-extension-manager/notifications`. Merely having a library on disk is not
sufficient to open a plugin page; its settings provider must be available.

## Direct pages and runtime registration (no manifest)

These GUI-thread QML APIs are independent of package installation. A manifest is
optional packaging/dependency metadata, not a prerequisite for opening a page.
Neither API requires importing shared controls or adopting a translation SDK.

For optional integration, load the helper dynamically **from a signal handler**:

```qml
var factory = Qt.createComponent("qrc:/xovi/manager/SettingsApi.qml")
if (factory.status === Component.Ready) {
    // Keep api and the registration owner alive (for example under MainView).
    api = factory.createObject(root)
}
factory.destroy()
// An unavailable helper means no manager; keep the plugin's own fallback.
```

The containing QML needs only its ordinary Qt imports. Do not add a mandatory
`import org.xovi.Manager` to a firmware page when manager support is optional.
If the manager is a required dependency, `ManagerNavigation` exposes the same
methods directly (`requestOpenSettings` corresponds to helper `openSettings`).

### 1. Open without registration

```qml
api.openPage({url: Qt.resolvedUrl("Settings.qml"), title: qsTr("Settings")})
// Or an existing `Component { id: preferences; Item { ... } }`:
api.openPage({component: preferences, title: qsTr("Settings"), chrome: "host"})
```

No ID, title declaration, manifest or registration is required. The root must be
an `Item`; an ordinary Item with no SDK properties works. Direct pages do not
enter inventory/Shortcuts, do not gain a PIN button, and do not write launcher
preferences. URLs must be absolute `file:` or `qrc:` URLs; resolve relative URLs
at the caller. Inline text is also accepted as `{kind:"inline", source:"import
QtQuick; Item {}", baseUrl:"qrc:/my-plugin/Settings.qml"}`.

`chrome:"host"` (default) provides the manager's navigation and title.
`chrome:"page"` hides them once creation succeeds, allowing the page to own its
complete layout. Error recovery remains available if the page fails to load.
To use host services, opt in with `usesSettingsContext:true` and declare
`required property var settingsContext` on the root. This supplies language,
close/navigation and other context methods; it is not required for display.
A self-contained page can call `settingsContext.close()` for its own Back button.
Settings storage needs a known provider ID; anonymous direct pages do not create
an implicit provider just to store configuration.

A borrowed Component remains owned by its caller and uses its original QML
creation context. Keep its owner alive while displayed; destroying that owner
unloads the page safely. Prefer a URL or inline source if navigation destroys the
caller. Passing an already-created Item is not supported.

The return value `{ok:true, accepted:true, state:"queued"}` acknowledges queued
navigation, not successful rendering. Loading errors appear in the host's
recovery panel; direct pages are not registered in `settingsList.uiStates`.

### 2. Register a discoverable, pinnable page at runtime

```qml
var result = api.registerPage(root, {
    id: "my-plugin", pageId: "main", title: "My plugin",
    titleTranslations: {en:"My plugin", zh_CN:"我的插件", zh_TW:"我的外掛"},
    iconSource: "qrc:/my-plugin/icon.svg",
    url: Qt.resolvedUrl("Settings.qml"),
    chrome: "host",
    launcherDefaults: {settings:true, sidebar:false, bottom:false, quick:false}
})
// The source may instead be component: preferences, or kind/source/baseUrl.
api.openSettings("my-plugin", "main")
// Optional explicit cleanup; owner destruction also unregisters:
api.unregisterPage(root, "my-plugin", "main")
```

Only a stable `id`, nonempty `title`, source and live QObject owner are required.
`pageId` defaults to `main`; all launcher locations default off. Register from a
long-lived object, not the settings page's own `onCompleted` (that page has not
yet been opened). The page appears in the manager even without a package entry.

Registrations live only for the current session. User PIN choices persist by
`id/pageId`, survive unregister/restart and override subsequent default locations.
The same owner can update a registration. Another owner using the same key gets
`page-already-registered`; it cannot unregister the first owner's page.

Use the installed QMD basename (without `.qmd` or its three-digit `NNN-` ordering prefix),
or the native plugin's runtime ID, as `id`. The manager matches that identity to
inventory and hides entries when the owning plugin is disabled. An arbitrary ID
without a matching package is a standalone runtime provider: the manager cannot
infer which unrelated QMD/native library owns it. Runtime metadata overrides a
matching manifest page; existing native provider metadata retains precedence.

Entry translations belong to the plugin, as do page translations. No controls
JSON schema is involved. Registration reports `ok`/`error`; invalid IDs, missing
titles, unsupported sources, expired Components or an unavailable backend are
reported without blocking xochitl startup. This API does not sandbox arbitrary
QML or prevent synchronous plugin code from blocking the GUI thread.

### Native window and icon handling

Opening a page while native Settings is visible uses that Settings window's host;
otherwise it uses the MainView host. Settings-sidebar PINs do not open a hidden
page behind the native Settings window. The same routing applies to direct pages.

Use the plugin's existing `qrc:` or local-file icon resources with native Ark
controls. Bluetooth retains upstream's `bluetooth.rcc` and original
`qrc:/ark/icons/bluetooth`; it does not embed icons as data URLs.
