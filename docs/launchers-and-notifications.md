# Pinned entries and notifications

The manager discovers native and QML settings pages automatically. Plugins may
optionally declare initial pin locations; they never need to inject their own entry. Open a plugin's settings page and
choose its labelled **Pin** action, then choose **Sidebar**, **Bottom bar**, **Settings sidebar**, or **Quick settings**,
then toggle that entry’s visibility.
Each control combines a location icon and text; black fill means pinned. Each page in the plugin detail view also has its own Pin action. Returning
from pin options keeps the existing settings page alive. **Manage shortcuts**
in the left navigation manages all pages, including the manager
and notification center. Undeclared plugin pins default off; the Extensions entry
in the native Settings sidebar defaults on. Native General/Wi-Fi/Language categories
inside Settings are not managed or modified.
Choices live in `exthome/xovi-extension-manager/launchers.json`; they survive a
restart. Missing, failed, or manifest-disabled providers are hidden without discarding
their choices, even if their native library is still initialized in this process.
Disabling or removing a legacy native provider also suppresses its entries for
the rest of the session; it does not need a manifest to be hidden.
Successful UI disable operations refresh pins immediately; visible launcher
widgets also refresh periodically to pick up external changes.
Pinned entries use native `ArkControls.SidebarItem` and homescreen `Action`
components, with the original title and qrc icon supplied by the plugin's optional
presentation getter (or a generic icon for older providers). The next-page action occupies a
normal slot in the same bar; it is not an overlay. Extra shortcuts use explicit
next-page actions rather than scrolling.

The backend broker commands are:

- `xovi-extension-manager$launcherList`: returns `entries` with `id`, `pageId`,
  `title`, `available`, `sidebar`, `bottom`, `settings` and `quick`.
- `xovi-extension-manager$launcherSet`: `{id, pageId, location: "sidebar" |
  "bottom" | "settings" | "quick", enabled: boolean}`. Saves immediately, no restart required.

Firmware adapters are maintained separately in
`xovi-extension-manager-ui/qmd/manager_3_27_x.qmd` and
`manager_3_28_0_162.qmd`. They place the manager entry, shortcut widgets and the
shared main-window host. Advanced Settings no longer adds its own sidebar entry.
These adapter changes require one xochitl restart after updating the libraries.

## Plugin defaults and user overrides

For a native provider, export an optional getter with metadata:

```text
export my_launcher_defaults
with
    xovi-extension-manager$launcherDefaults = 1
end
```

```cpp
#include "xovi-settings.h"
extern "C" const XemLauncherDefaultsV1 *my_launcher_defaults() {
    static const XemLauncherDefaultsV1 defaults{
        1, sizeof(XemLauncherDefaultsV1), "main", XEM_LAUNCHER_SETTINGS
    };
    return &defaults;
}
```

`locations` is a bitmask of `XEM_LAUNCHER_SIDEBAR`, `XEM_LAUNCHER_BOTTOM`, `XEM_LAUNCHER_SETTINGS`, and `XEM_LAUNCHER_QUICK`. Zero explicitly defaults all locations off. The pageId
must match an exported settings provider; declarations do not create pages.
The getter must be thread-safe, side-effect free and return process-lifetime data.
It is optional and does not create a dependency on the manager.

A QML/QMD package (or a native package without that getter) declares defaults in
its manifest's existing settings descriptor:

```json
"settings": {
  "apiVersion": 1,
  "pageId": "main",
  "page": "Settings.qml",
  "iconSource": "qrc:/my-plugin/icons/settings.svg",
  "launchers": {"sidebar": false, "bottom": false, "settings": true, "quick": false}
}
```

For QMD packages, `page` names a QML file shipped with the package, not the QMD
patch. Entry rendering/opening is provided by the manager firmware adapter; no
extra QMD insertion is needed. A native getter takes precedence over manifest
defaults for the same runtime page. Unspecified locations default off.

The first discovery of an enabled, available page snapshots all four locations
into `launchers.json` using a file lock and atomic write. After that the user's
saved values win, including explicit `false`; upgrades and disable/re-enable do
not reapply defaults. Existing saved choices are retained. A disabled native
package cannot publish its runtime defaults until it is loaded and initialized.

## Notifications

Notifications are scoped to the current xochitl process: restart clears history.
The thread-safe in-memory store keeps at most 100 entries, evicting the oldest.
An ordinary repeated owner/key post updates the entry, increases its count and
makes it unread again. A successful post reports `queued`, not proof of display.

QML settings pages use their injected context:

```qml
settingsContext.notify({
    notificationId: "rescan-complete",
    title: qsTr("Library updated"),
    message: qsTr("The rescan has finished."),
    level: "info",
    pageId: "main"
})
settingsContext.dismissNotification("rescan-complete")
```

The context binds `ownerId` to the current plugin; a page cannot override it via
this convenience API. Native code can optionally discover the getter tagged
`xovi-extension-manager$notificationsApi = 1` using the normal XOVI metadata
iterator. Check that the manager is initialized, the ABI is 1 and `structSize`
is at least `sizeof(XemNotificationsApiV1)`. The table is declared in
`sdk/xovi-notifications.h`; use its `freeString` for returned JSON. Do not add a
mandatory manager import just to publish optional notifications.

The native table's `post` takes a JSON object with `ownerId`, `notificationId`,
`title`, `message`, optional `level` (`info`, `warning`, `error`) and optional
`pageId`. A V2 table is separately discoverable through
`xovi-extension-manager$notificationsApiV2 = 2`; its V1-compatible prefix adds
`pollActions(ownerId)`. Check ABI 2 and `sizeof(XemNotificationsApiV2)` before
using that field. The broker exposes the same data via `notificationsPost`,
`notificationsList`, `notificationsRead`, `notificationsDismiss` and
`notificationsClear`. Read/dismiss require the owner and key; clear requires an
owner and removes that owner's notifications. List may filter by owner.

Posts may include `state` (`running`, `completed`, `failed`, `cancelled`),
`progress` and up to two actions. `progress` is either `null` to clear it, or
an object with `value` in 0..1; `indeterminate: true` may omit `value`. Actions
are `{id, label}` objects. A same-key running progress/state update is a patch:
it keeps the entry position, count, read state and `toastRevision`. A transition
from `running` to a terminal state makes the entry unread and assigns one new
per-entry `toastRevision`; repeating that terminal update does not toast again.
State/progress patches may omit title, message, page, level and actions, which
then retain their prior values, except that a transition out of `running` clears
old actions unless new actions are explicitly supplied. Notifications without
progress return `progress: null`. Evicting an entry also clears its queued actions.

The broker's `notificationsActionInvoke` maps to `actionInvoke` with owner,
notification key, action key and optional entry `sequence`; a stale sequence or
unavailable action is rejected. Valid invocation only queues the request and
returns `queued`; it never runs plugin code. `notificationsPollActions` maps to
`pollActions(owner)` and atomically takes that owner's pending actions. Entries
include `pendingActionIds`, which grows on queueing and is cleared when actions
are delivered. Repeated clicks for the same notification/action are rejected
while pending; the action queue is bounded to 100 and reports full rather than
discarding requests. Dismiss and clear remove relevant pending actions.
Owner/key/page IDs use the usual manager ID rules; titles are at most 256 UTF-8
bytes, messages 4096 bytes and requests 16 KiB. Invalid requests return an error.
All native extensions share the process; this is not a security boundary.

The UI polls only this in-memory store every two seconds.

The firmware adapter adds a native bell switch to the vertical toggle column in
Quick Settings (3.27 and 3.28). Its full-height drawer occupies the space entirely
to the left of the native Quick Settings panel, including on narrow screens. The list uses paginated cards rather
than scrolling; selecting a card exposes details, progress and both declared
actions. The manager's notification page uses the same list component.

All short notifications use a compact white/black prompt at the screen's upper
right, with a title and at most two message lines. Tapping it opens the drawer.
They do not also enqueue a native notification. Prompts last five seconds and
are rate-limited to one every six seconds; suppressed prompts remain in history.
Progress updates do not restart the timeout or replay dismissed prompts. Progress
and task controls stay in the notification center; unknown totals use a static
pattern rather than continuous e-paper animation.

Closing a banner is not cancellation: only an explicit action asks the plugin
to cancel; its subsequent state update confirms the result.
No caller-provided QML, shell command or JavaScript action is executed.
Opening a settings page that fails also records a notification.

## Task notification example (QML)

```qml
function startImport() {
    settingsContext.notify({
        notificationId: "import", title: qsTr("Importing"),
        message: qsTr("Preparing documents"), state: "running",
        progress: {value: 0}, actions: [{id: "cancel", label: qsTr("Cancel")}]
    })
}
function updateImport(fraction) {
    settingsContext.notify({notificationId: "import", progress: {value: fraction}})
}
function finishImport() {
    settingsContext.notify({notificationId: "import", state: "completed",
                            progress: {value: 1}, message: qsTr("Done")})
}
Connections {
    target: settingsContext
    function onNotificationAction(action) {
        if (action.notificationId === "import" && action.actionId === "cancel")
            importController.requestCancel() // asynchronous plugin-owned operation
    }
}
```

Posting actions through a SettingsContext enables its one-second action polling.
The signal runs on the UI thread: hand work to an asynchronous controller.
The context only lives while its page is loaded. Reopening a page that expects
pending actions can set `settingsContext.notificationActionsEnabled = true`.
Alternatively `settingsContext.takeNotificationActions()` takes the pending
batch manually. Use one consumer per owner; polling takes events once, without
acknowledgment/redelivery. A native task that outlives its page should use the
V2 API from its own controller; the manager never calls native task code from
a button handler. An unavailable consumer leaves the action pending, not done.

## Build and check

Use `make all` (or the equivalent `./build.sh all`) for both architectures.
The top-level Makefile invokes each plugin's own Makefile/build.sh, then packages
its output in `build/packages/<arch>/` and refreshes `qmd-repositories.json`.
`make build ARCH=aarch64` builds/packages one architecture; `make package
ARCH=aarch64` repackages existing binaries. There is no separate manager output
tree. The old manager-only command names are compatibility aliases to this flow.

Host tests cover the memory store, ownership, duplicate/cap behavior, actual
manager QML, navigation, upper-right notification prompt and shortcut discovery.
Firmware patches are applied to both extracted RCC trees and parsed. Mock native
modules on a host do not replace device testing of gestures, keyboard focus and
xochitl's notification queue display.

## Device compatibility regression checks

`bash xovi-extension-manager-ui/tests/check-firmware-qmd.sh` applies both firmware
adapters, parses the generated QML and checks the injected settings entry.
The entry must use xochitl's `showLabels` condition, retain SidebarItem's own
content/background, and avoid forcing a minimum text width on compact screens.
Provider pages and shared controls must not use `Accessible.*`: both supplied device SDKs have
`QT_FEATURE_accessibility = -1`, even when host Qt supports the attached object.
Host rendering alone cannot detect this build-feature mismatch. The scan covers
manager, shared SDK controls and all bundled provider setting resources, including
EPUB Preloader. Settings load failures show a short explanation and recovery hint;
raw QML errors remain under Details.

The reported FormatFont invalid-index error is addressed in Advanced Settings'
3.28 font-model adapter; run `sh advanced_settings/tests/test-format-font-qmd.sh`
for the applied-resource regression check. Experimental.qml's missing Values
import and DropdownButton.qml's unguarded parent.width also exist in the supplied
original firmware resources; their exact on-device trigger is not established
by these host checks. No blanket override of these shared system components is
included in the manager adapter.

### Native navigation entries

`launcherList` additionally returns `nativeEntries`. These have `id: "xochitl"`,
`native: true`, and a fixed `location` (`sidebar`, `bottom`, or `quick`). They default to
visible. `launcherSet` saves their visibility in the same preferences file;
requests to move native actions into another container are rejected. The firmware
QMD adapters gate the existing components, preserving native actions, icons and
supported-view checks. Unknown actions and unavailable manager replies remain
visible. This supports visibility changes; it does not rename or reorder native
items. Plugin pins remain in `entries` and are rendered separately.

Shortcut management has only location categories: sidebar, bottom bar, settings
sidebar, and quick settings. Each category lists all supported entries, including
hidden ones, with one row containing the original icon, translated title and a
single visibility switch. Native entries appear only under their supported
location. Plugin pages can be selected at each location. There are no separate
Available/Built-in categories. Multiple rows fit each page; pagination stays in
the footer without scrolling. Filters reset the page index.

Quick settings preserves the original StateSwitch controls and actions for airplane
mode, Screen Share and rotation lock. Their WidgetLoader visibility follows saved
preferences, so hidden controls can be restored without recreating the underlying
feature. The notification shortcut still opens the left-hand notification drawer;
plugin shortcuts open the corresponding settings page. A direct Extensions quick
shortcut counts as a protected manager route; the notification drawer alone does not.

Enable/disable results are posted to the notification store, including successful
changes that require a restart and failures. Only a notification-post failure
falls back to an inline message.

Manager buttons are embedded under the manager's private QML module using the
same SDK source files, avoiding stale shared-control resources from other plugins.
Disabled text, icons and borders are gray; no disabled underline is drawn.

Native-tinted SVGs contain only the symbol, without an opaque white canvas: the
button supplies its background. Otherwise Ark's recoloring can turn that canvas
into a solid square, particularly for selected buttons.

The navigation rail shows labels on wide screens and icons on narrow screens.
Shortcut rows show the visibility state and location icon. The list remains paginated without scrolling.

`launcherSet` rejects `last-manager-entry` if the proposed configuration removes
all routes to Extensions: the native Settings entry together with an enabled
Extensions/Notifications entry inside it, or a direct inventory/notification-center
pin on the home sidebar or bottom bar, or a direct Extensions quick-settings pin.
The check runs under the configuration lock, so native/broker callers cannot
bypass it. Older configs with all routes hidden effectively restore the native
Settings entry and its Extensions child, and atomically persist that recovery.

## Native presentation and resource ownership

Manager and Advanced Settings sidebars use the native Ark SidebarItem content,
padding, sizing, selection colors and dividers. Back is the first sidebar item.
Only label typography is supplied by the shared Typography source (36px).
Labels follow the native wide-screen threshold; narrower screens show icons.
Shortcut names resolve the provider’s own translation catalog before falling back
to the manager/native catalogs, independently of whether its page has been opened.

`org.xovi.Controls` is a compile-time QML resource bundle. Providers such as
Advanced Settings and Keyboard CJK include `sdk/xovi_controls.qrc` in their own
libraries. It does not require extension-manager-ui to be enabled. A plugin with its own
standalone launcher can use these controls without the manager; a page hosted through
the manager uses its supplied settingsContext, and manager-owned launchers naturally
require the manager. Keyboard CJK’s manager wrapper reuses the existing keyboard
popup without adding a second Settings/Input-scheme tab bar.
