# Settings provider and injection status API (v1)

The manager owns package inventory, a shared configuration store, injection
policy, and aggregated status. `xovi-extension-manager-ui` owns Qt page creation.
`qt-resource-rebuilder` owns resource-processing results. None of these states
means that arbitrary plugin business logic has been proven correct.

## No-manifest QML integration

For direct opening or runtime registration, use the
[QML navigation API](settings-navigation-api.md#direct-pages-and-runtime-registration-no-manifest).
`openPage` accepts an ordinary Item page without any provider declaration.
`registerPage` adds discovery and PIN using a live owner and stable ID. The
manifest and native provider contracts below remain optional alternatives.

## Native settings pages

Include [`sdk/xovi-settings.h`](../sdk/xovi-settings.h). Export a function
returning a process-lifetime `const XemSettingsProviderV1 *` and tag its `.xovi`
export with `xovi-extension-manager$settingsProvider = 1`. There is no import of
manager or manager-ui, so a plugin can continue working without either installed.
The manager queries only initialized providers. Getters must be side-effect free,
thread safe, and must not create Qt objects, perform I/O, or call back into manager.
A provider is trusted in-process code, not a sandbox.

See the complete [`settings-native`](../../examples/settings-native) example.
The ABI carries its version and structure size; do not change v1 field layout.
All source pointers remain valid until process exit; manager copies their data.
`sourceKind=0` supplies a URL; `sourceKind=1` supplies UTF-8 QML and its byte count.
For inline QML use a unique `baseUrl`, e.g.
`qrc:/xovi/<plugin-id>/Settings.qml`. A base URL does not register sibling files.
Package multi-file pages in a Qt RCC or provide a file URL with real dependencies.
A `.xovi resource name:file` only embeds bytes as `r$name`; it does not register
Qt resources. The inline example deliberately demonstrates XOVI resource usage.

Provider ownership is derived from XOVI's extension name, not a claimed owner in
the descriptor. `pageId` is unique within that owner. Native page records also
include `packageId`, resolving a legacy library basename to its manifest ID for
the inventory UI. Keep using `id` as the settings and shortcut owner so stored
preferences survive this association. Static duplicate entries
with the same owner/page ID are superseded by a live native provider. A disabled
or failed native plugin has no callable provider; manager still displays its
package and load error. An optional manifest page can provide offline settings.

## Pure QML packages and static pages

A `type: "qml"` package needs no native library and no QMD:

```json
{
  "manifestVersion": 1,
  "type": "qml",
  "id": "my-settings",
  "name": "My settings",
  "version": "1.0.0",
  "entry": "ui/Settings.qml",
  "enabled": false,
  "requires": { "xovi": ">=0.3.0" }
}
```

Install a tar.gz/zip with this manifest at its root using the existing `install`
command. Files live in `qml.available/<id>/`, and enabling creates
`qml.d/<id>.qml` pointing to the declared entry. Entry paths must be safe relative
paths. `settingsList` resolves page files canonically and rejects paths escaping
the package directory, including symlinks. Config lives in `exthome/<id>/` and
survives uninstall. New package enable/disable affects future page openings and
does not restart xochitl. Replacing an installed QML package requires a restart
because xochitl may have cached its old component; manager does not clear the
application's shared QML cache. Close already open pages before disabling/removing.

Native and QMD manifests may optionally add:

```json
"settings": { "page": "ui/Settings.qml", "pageId": "main" }
```

This is a page entry, not a form schema. Static pages can be opened with the
native backend disabled, so those pages must explicitly handle missing backend
capabilities. For QML packages `entry` supplies the default page.

## Page contract

The page root must be a `QQuickItem` (usually `Item`) with:

```qml
required property var settingsContext
```

A page shared with an existing standalone entry may instead declare optional
`property var settingsContext` and guard its use when opened outside the manager.
The host injects this property before creation and manages sizing and lifetime.
Do not access internal xochitl IDs or assume a particular parent hierarchy.
Use `settingsContext.close()` to request navigation back. Pages load only when
opened. Root component loading/creation errors leave the manager UI intact, with
an error and Back button. A page using nested Loaders must display their errors
itself; successful creation of the outer page does not prove its children loaded. Arbitrary JS loops or blocking native calls are still in process
and cannot be isolated by asynchronous QML loading.

`settingsContext` exposes:

| Member | Meaning |
| --- | --- |
| `pluginId` | Provider/package owner. |
| `values`, `revision`, `error` | Current shared config snapshot and last error. |
| `save(object)` | Merge top-level values with optimistic revision checking. |
| `reload()` | Reload config after a revision conflict or external change. |
| `injectionStates()` | Get rebuilder results and desired policy. |
| `setInjectionEnabled(id, bool)` | Save next-start injection policy. |
| `sendPluginSignal(signal, text)` | Call `<pluginId>$<signal>` through native broker. |
| `close()` | Return to the package list. |
| `notify(object)` | Queue a session notification owned by this plugin. |
| `notificationActionsEnabled` | Subscribe to owner-bound action/state events while the context is alive. Posting actions enables it. |
| `notificationAction(action)` | Signal carrying one queued action; start work asynchronously. |
| `notificationState()` | Query entries and queued/delivered/completed/failed actions without consuming them. |
| `notificationStateChanged()` | Signal: query a new snapshot after a store change. |
| `completeNotificationAction(actionSequence, success, result)` | Explicitly acknowledge completion or failure; delivery alone is not success. |
| `dismissNotification(key)` | Dismiss this plugin’s notification by key. |
| `systemNavigationAvailable` | Whether the host currently supplies system navigation; updates live. |
| `openSystemSettings(target)` | Open `wifi` or `language` using the host adapter. Returns `{ok:true,state:"dispatched"}` or `{ok:false,error:...}`. |

`save()` means **stored**, not applied. A plugin may keep its existing backend
and persistence instead of using this store. Backend operations must validate
inputs themselves and declare their own apply/restart behavior. The shared file
is `exthome/<id>/manager-settings.json`, containing `revision` and `values`.
Do not edit it concurrently outside this API.

## Manager broker commands

All requests below are JSON objects; all return JSON. These also work via the
existing native in-process broker. The shell FIFO retains its existing request
size limit; do not send whole QML pages or large config objects through it.

| Signal suffix (`xovi-extension-manager$...`) | Request | Result |
| --- | --- | --- |
| `settingsList` | `{}` | `pages`, manager `sessionId`, `uiStates`. |
| `settingsRegister` / `settingsUnregister` | UI-managed page descriptor and registration token. | Session-only registration/removal. Use the QML API to manage QObject/Component lifetimes; Component tokens are process-local. |
| `settingsGet` | `{"id":"example"}` | `values`, `revision`, `applyState:"stored"`. |
| `settingsUpdate` | `{"id":"example","expectedRevision":0,"values":{"foo":true}}` | Updated snapshot or `revision-conflict`. |
| `injectionsGet` | `{}` | Rebuilder snapshot plus persisted `policy`. |
| `injectionsSet` | `{"id":"example","injectionId":"button","enabled":false}` | `requiresRestart:true`, `pending-restart`. |
| `uiReport` | `{"id":"example","pageId":"main","state":"ready"}` | Host-reported UI status. Optional `message`. |

Writes use a lock file and atomic replacement; corrupt configuration is rejected
rather than silently replaced. UI status is process-local and independent of the
rebuilder session. Warning messages are separate from component creation state.
The UI currently opens the first available page per plugin; v1 descriptors allow
multiple page IDs for future navigation and direct consumers.

## Rebuilder API and policy

Include [`sdk/qrr-api.h`](../sdk/qrr-api.h). `qrr_get_api_v1` is exported and
tagged `qt-resource-rebuilder$api = 1`. Consumers can discover it through XOVI
metadata without a mandatory dependency. A plugin which requires QRR can instead
use `depends-on qt-resource-rebuilder:0.4.0` and
`import? qt-resource-rebuilder$qrr_get_api_v1`. In XOVI `import?` is a load
condition as well as an import; it is **not** an optional dependency.

The API table supplies:

- `registerPatch(ownerId, injectionId, bytes, size)`: register before resource
  processing. Returns acceptance, never a promise of runtime success.
- `snapshot()`: immutable JSON snapshot; does not invoke any plugin or Qt callback.
- `freeString(response)`: release **every** returned string with the same API.

IDs use 1–128 ASCII letters/digits/`_`/`.`/`-`, excluding `.` and `..`.
Duplicate registrations and late registrations are rejected. The legacy
`qmldiff_add_external_diff` remains available with legacy ownership and boolean
acceptance. There is no retroactive removal of a patch from registered resources.

The manager writes `exthome/qt-resource-rebuilder/injection-policy.json`:

```json
{"version":1,"entries":{"example/button":false}}
```

Missing entries default to enabled. An unreadable/malformed policy fails closed
for applicable QMD registrations and is reported. QRR reads this independently
of manager/UI initialization. Managed standalone QMDs use their manifest ID and
`injectionId:"main"`; LOAD children retain their source names while inheriting
ownership. Legacy QRR/RCC diagnostics are reported, but these formats do not yet
participate in per-plugin injection policy; use QMD/new API for managed switches.

Snapshot fields: `apiVersion`, `sessionId`, `revision`, `results`. Each result has
`ownerId`, `injectionId`, `source`, `resource`, `state`, `code`, `message`, and
`revision`. No subscription is required, so early errors remain queryable.

| State | Meaning |
| --- | --- |
| `accepted` | Registration succeeded; no target has necessarily been seen. |
| `pending` | Target resource not encountered yet; not proof of failure. |
| `skipped` | Disabled or no version-applicable changes. |
| `prepared` | Processing produced a candidate resource. |
| `applied` | Qt accepted the rebuilt resource archive. Not QML/runtime health. |
| `failed` | Registration, decoding, or resource registration failure. |
| `rolled-back` | A contributor failed; this resource's whole patch set was discarded. |

`processed` counts matched AFFECT blocks, not semantic property mutations. A
conditional branch can do nothing; `applied` only promises accepted output.
Conflicts that produce valid but undesired QML cannot be detected generically.
Errors on shared resources identify affected contributors; they do not falsely
assign sole blame without evidence.

QMD processing uses cloned state and catches recoverable Rust panics before the
C ABI boundary. Explicit `ERROR` becomes a processing error. Slots and changes
are restored if processing/Qt archive registration fails. QRR keeps original
resources on normal processing failures; if registering a rebuilt archive fails,
it retries the original archive. Incorrect disk-cache setup disables QRR instead
of aborting xochitl. This is not protection against arbitrary memory corruption,
stack overflow, OOM termination, or a plugin's blocking native code.

QML compile/create errors and exact-page URL warnings come from manager-ui, not
QRR. Errors in xochitl's own root QML or shared dependencies cannot always be
attributed to one plugin. Crash-loop recovery needs an external supervisor and
is outside this in-process API.

System navigation targets are stable names, not xochitl resource paths. The
manager UI maps them to the supported firmware routes. `dispatched` means the
navigator accepted the call without throwing or explicitly returning false;
it does not prove the target window finished loading. Errors are
`unsupported-target`, `navigation-unavailable`, or `navigation-failed`. Plugins
should disable navigation controls when `systemNavigationAvailable` is false.
Shared standalone pages may continue using their original navigator outside the
manager. This capability is available to native and QML-only provider pages.

Pinned sidebar/bottom-bar entries need no plugin declaration. See
[launchers and notifications](launchers-and-notifications.md) for APIs and lifetime.


## Shared presentation controls

The SDK owns `org.xovi.Controls 1.0`. `Typography.qml` is the sole source for
extension UI font family and size (36px). Use `ELabel`/`EButton` for plain controls
and `SettingsBody`, `SettingsLabel`, `SettingsTitle`, `SettingsPanel`, or
`SettingsSidebarItem` for native Ark presentation. These adapt local typography
without modifying xochitl's global tokens; do not set numeric font sizes in pages.
User-selected fonts in reading-content previews are separate from UI typography.

`Pager` is an opaque, full-width footer. Reserve its actual `implicitHeight` in
the content layout, and calculate page capacity from the remaining space. Do not
anchor a pager over a list or subtract a guessed fixed height. Use the shared
Pager for previous/next navigation: 36px text, black/white SVG buttons, page
counter, identical disabled state. A provider handles page selection; the SDK
handles the presentation. Manager-ui keeps its old Controls aliases for compatibility.

Native providers add `sdk/xovi_controls.qrc` to their Qt RESOURCES. It embeds the
same source module at `/qt/qml/org/xovi/Controls` and icons at `/xovi/controls/icons`.
This module has no manager service dependency, so original standalone pages can
use it even when manager-ui is not installed. Avoid copying another private
implementation into each settings page.

```qml
import org.xovi.Controls 1.0
Pager {
    pageIndex: root.page
    pageCount: 3
    onPageRequested: function(index) { root.page = index }
}
```

Both supplied device SDKs disable Qt Accessibility. An `Accessible.name` or
`Accessible.role` attached property therefore fails when the device creates the
page, even if it works on host Qt. Use EButton's `description` for descriptive
metadata; do not add unsupported attached properties. Run the firmware/resource
compatibility scan as well as a host rendering test. The manager contains page
creation failures, shows a short compatible-device/update hint and offers Back,
Retry and Details; it cannot make an unsupported Qt type available to the plugin.

`xovi-extension-manager-ui/tests/run-typography-tests.sh` exercises the real Ark
text, panel and sidebar QML extracted from both supported firmware versions.
Only device C++ settings, text sanitation and icon rendering are substituted on
the host. This catches nested labels that ignore an outer control's font.



## Page identity and localization

An optional C getter tagged `xovi-extension-manager$settingsPresentation = 1`
returns `XemSettingsPresentationV1` from `sdk/xovi-settings.h`. It supplies the
existing `pageId`, original `title` translation key, `translationContext`, and
`iconSource` (a qrc URL). The getter follows the same static-storage/thread rules
as the page getter. It does not declare pin locations: those remain user choices.
The manager carries these fields through `settingsList` and `launcherList`, and
uses `qsTranslate(context, title)` at display time. It never guesses a plugin's
icon from its library name. V1 page providers still work with a generic icon.
Static manifest pages can supply `settings.iconSource` and `settings.titleContext`.
Advanced Settings supplies its original Control Center title and bundled icon.

Each plugin owns its English, Simplified Chinese and Traditional Chinese TS files
under its own `translations/` directory. Its own build compiles them to a unique
`/xovi/i18n/<plugin-id>/` resource prefix. Manager-ui neither supplies nor replaces
another plugin's language pack.

`sdk/i18n.pri` and `xovi-i18n.h` are optional build/loading helpers, not a required
settings-provider ABI. Set `XOVI_TRANSLATION_ID` and `XOVI_TRANSLATION_DIR` before
including the pri. In `_xovi_construct`, register
`qAddPreRoutine(+[] { XoviI18n::prepareCatalog("plugin-id"); });` so titles are
translated before first display; call `XoviI18n::attach(engine, "plugin-id")` in the
provider to refresh live QML when the language changes.
An external plugin may instead use standard Qt `QTranslator` with its own loader.
The helper uses one application-owned language service, shared by all plugins and
QML engines. Like rm-appload, it manually reads `[General] Language` from
`/data/xochitl.conf` (QSettings is not used). The legacy home config is considered
only when the canonical file is absent. File changes and atomic replacements are
watched; temporary absence preserves the current language. A newly opened engine
cannot reset the session to English. Catalogs remain plugin-owned and live for the
application session; real language changes trigger one queued refresh per engine.
The pri binds generated RCC functions locally with `-Bsymbolic-functions`, avoiding
resource initializer collisions between globally loaded plugin libraries.
External pages can read `settingsContext.uiLanguage` and observe `languageChanged`
without reading any configuration file themselves.

### Page chrome ownership

Existing presentation V1 providers default to `host`: manager supplies the header;
the plugin supplies content and its own content pagination, if any. To supply the
entire page, export `XemSettingsPresentationV2` through metadata
`xovi-extension-manager$settingsPresentation=2`, set its base ABI to 2 and size to
`sizeof(XemSettingsPresentationV2)`, and set `chromeMode = XEM_CHROME_PAGE`.
`XEM_CHROME_HOST` retains the default. Provider V1 itself is unchanged.
Manifest QML pages can instead declare `settings.chrome: "page"` or `"host"`.

In page mode the ready page occupies the whole viewport with no manager header,
footer or margins. Loading/error recovery still belongs to the host. Advanced
Settings and Keyboard CJK use page mode. `settingsContext.hostHeaderVisible` and
`hostFooterVisible` describe ownership; `launchSource` identifies the entry point
(`manager`, `sidebar`, `bottom`, `shortcut`, or `notification`). Call
`settingsContext.close()` from the page's own Back/Close control: a shortcut closes
to its caller, while a manager-opened page returns to the inventory. Optional
`settingsContext.requestPin()` opens the manager's pin controls. Pages must provide
a visible close action in page mode. Pinning is also available from inventory.

Maintain source text with `qsTr` or a plugin-specific `qsTranslate` context.
Keep protocol IDs, configuration keys and diagnostic codes untranslated. External
plugins can install their own Qt catalogs and provide that same context in page
presentation metadata; do not ask manager-ui to reproduce their page content.
Run `scripts/update_ui_translations.sh` explicitly when text changes, then finish
all three catalogs. Normal builds only compile translations; they do not rewrite
TS sources. `sdk/tests/check-catalogs.py` checks completeness and placeholders;
`sdk/tests/run-i18n-tests.sh` checks live switching, script/region matching and
English fallback.

Manager-owned labels resolve through its own catalog (`ManagerBridge.translate`),
so a subsequently installed global translator cannot shadow the manager's text.
Source strings remain extractable with `QT_TR_NOOP`. Each hosted provider also
receives a separate child of the stable manager QQmlContext; destroying it leaves
the manager context intact and preserves inherited native services. Cancelled navigation with an invalid host context is
reported as unloaded, not as a plugin failure. Shortcut navigation is queued, and
the firmware uses a declarative Loader source to avoid capturing a closing
notification callback context. This does not transfer ownership of plugin translations to the manager.

Cross-plugin navigation is documented in [settings-navigation-api.md](settings-navigation-api.md).

### Pure-QML package presentation

A manifest settings descriptor may provide `title`, `titleTranslations` keyed by
`en`/`zh_CN`/`zh_TW`, and `iconSource` as a qrc URL or package-relative asset path.
Relative paths are canonicalized and must remain inside the package. Native
presentation getters continue to own their original title/catalog/icon metadata.
See [the Bluetooth integration case study](external-plugin-bluetooth-case-study.md)
for a QMD package that needs no native shim or shared-controls import.
