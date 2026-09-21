# Bluetooth Settings: minimal QMD integration

The checkout at `../../rm-qmdiffs/xovi-bluetoothsettings` retains upstream's
README, UI, qsTr calls, Bluetooth logic and `bluetooth.rcc` icon resource.
The checkout adds `bluetoothSettings.qmd` and the unchanged `bluetooth.rcc`.
The QMD is adapted directly from the
upstream v0.2.1 release. Upstream Git tracks documentation but not the released
QMD, so review integration against that release file rather than treating every
line of the newly tracked file as new implementation.

The existing page becomes a reusable QMD SLOT, inserted in native Settings and
MainView. MainView registers its Component through optional SettingsApi lookup.
With manager UI present, the original sidebar entry is hidden and the registered
entry follows the user's PIN choices. Without it, the original native entry
remains. The page has one optional settingsContext property and suppresses only
its duplicate title/top spacer when hosted. The original connection monitor,
commands, icon resource and translations are otherwise retained.

No manifest, generator, separate QML page, translation table, build script or
new icon is required. Install the QMD beside upstream's unchanged bluetooth.rcc,
as upstream already documents. Firmware application/syntax checks cover 3.28;
Bluetooth pairing still needs device testing.
