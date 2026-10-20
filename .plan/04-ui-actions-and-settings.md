# Phase 4: actions, layout widget, and settings

Depends on: [02-session-and-decoding.md](02-session-and-decoding.md) and
[03-audio-output-and-devices.md](03-audio-output-and-devices.md).

Status: implemented and verified, including actual loaded-plugin, same-process
main-player coexistence with Vim Motions. The subsequent user-confirmed physical
Bluetooth disconnect result and WirePlumber policy are recorded in Phase 3;
wider hardware coverage remains Phase 5 work.

## Host integration

Core initialisation acquires `audioLoader` and `settingsManager`, registers
plugin settings, and creates the shared controller/service. GUI initialisation
acquires `actionManager`, `trackSelection`, and `widgetProvider` and wires UI.
Use public API entry points only; do not inspect private playlist widgets.

Use fooyin's supplied `TrackSelection` for context-menu rendering/actions. A
context-menu action should retain the intended track snapshot instead of
re-querying a now-different selection after focus moves. Keyboard actions use
the current focus-sensitive selection. Define multi-selection behaviour as
**preview the first selected track in the controller-provided order**, and make
this clear in action help text.

Selection/focus changes never implicitly replace a running preview. Clicking
preview controls must not start main playback or change the playlist selection.

## Commands

Register stable action IDs under a **Headphones Preview** category:

| ID | Label | Behaviour |
| --- | --- | --- |
| `Headphones.Tracks.PreviewSelected` | Preview in headphones | Start selected track from its beginning |
| `Headphones.Playback.PlayPause` | Play/pause headphone preview | Pause/resume or restart retained preview |
| `Headphones.Playback.Stop` | Stop headphone preview | Discard audio and reset preview position |
| `Headphones.Seek.Backward.Small` | Seek headphone preview backward (small step) | Seek back by fooyin's small seek increment when seekable |
| `Headphones.Seek.Forward.Small` | Seek headphone preview forward (small step) | Seek forward by fooyin's small seek increment when seekable |
| `Headphones.Seek.Backward.Large` | Seek headphone preview backward (large step) | Seek back by fooyin's large seek increment when seekable |
| `Headphones.Seek.Forward.Large` | Seek headphone preview forward (large step) | Seek forward by fooyin's large seek increment when seekable |

Expose these via fooyin's `ActionManager` so users can assign shortcuts, including
when using the Vim Motions plugin. Start without default key assignments to
avoid stealing existing transport/Vim keys. No direct dependency on Vim Motions
or a new application-wide event filter is required.

Add **Preview in headphones** to the public track context menu registration
points used by playlists/library views; add a queue-area registration only if
the host's public menu composition requires it. Avoid duplicate entries where
the common track menu is already included. Disabled state/help text should
explain no selection, unavailable device, or unsupported source.

## Layout widget

Register `HeadphonesPreview` through `WidgetProvider`, display name
**Headphones Preview**, submenu **Playback**, initially limited to one instance.
Use a `FyWidget` subclass following the sibling Vim plugin's indicator pattern.

Contents:

- Preview track title/artist (fallback to filename), never the main track title.
- Clearly labelled **Headphones** device name and status, including Loading,
  Buffering, Paused, Playing, and useful error text.
- Preview-selected button plus play/pause and stop controls.
- Segment-relative elapsed/duration display and seek slider (disabled if not
  seekable; unknown duration must not invent a usable slider range).
- Independent headphone-volume slider with percentage/accessibility label.
- Configuration entry point for choosing the device.

The controller outlives widget instances. Destroying/rebuilding the layout widget
does not destroy the preview session. Commands continue to work without the
widget present; device setup remains accessible through plugin configuration.
Avoid modal dialogs for ordinary playback errors; show status and log details.

Seeking: show a tentative slider position while dragging, issue one seek on
release (or a coalesced keyboard seek), and resume normal position updates only
after the current seek is applied. Preserve paused state during scrubbing.

Use normal Qt focus/accessibility conventions, translated labels, and the host's
public styling/icon facilities where available. Do not add toolbar placement or
custom layout persistence until needed for this compact widget.

## Settings integration

Implement `PluginConfigGuiPlugin::settingsProvider()` and a
`PluginSettingsProvider::createSettings(QWidget*)` provider. Fooyin manages its
single-instance dialog lifecycle; use this rather than an unrelated window.

Persist under a namespaced `Headphones/...` prefix using public SettingsManager
facilities, following the sibling plugin's typed settings patterns where useful:

| Setting | Initial value | Meaning |
| --- | --- | --- |
| `DeviceId` | empty | Encoded opaque Qt audio output ID |
| `DeviceDescription` | empty | Last-known display name, not a matching key |
| `Volume` | 25% UI value | Clamped independent headphone volume |

Do not persist active playback or auto-restore a playing session. Shortcut
persistence belongs to fooyin's action system, not a second plugin binding store.

Dialog behaviour:

- Output-device list with a **Not configured** entry, clear names, and readable
  disambiguation for duplicate descriptions.
- Retain/display a missing saved device as unavailable instead of selecting the
  first device or the system default.
- Show a short setup note: choose a device different from fooyin's main output.
- Stage changes until Apply/OK; Cancel discards edits. Hotplug refreshes available
  choices without discarding pending selection or applying a different device.
- Applying a changed device stops preview before updating the controller's active
  device; explicit play is required afterwards. Applying the same device should
  not interrupt playback unnecessarily.
- Volume can be adjusted live in the preview widget. Avoid an independent stale
  dialog volume field overwriting a newer live value; simplest v1 dialog only
  configures the device and explains volume is controlled in the widget.
- Opening the dialog never emits sound and needs no test tone in v1.

## Tests and acceptance

- [x] QAction/menu integration targets the correct track even if focus changes.
- [x] Multi-selection, empty/unsupported selections, and command enabled states
  follow the documented rules.
- [x] Widget observes the shared controller, supports pause-preserving seeking,
  and can be destroyed/recreated without resetting main or preview playback.
- [x] Settings round-trip opaque IDs, duplicate names, missing-device state,
  volume bounds, Apply/Cancel, and no autoplay after restart/reconnect.
- [x] Keyboard actions coexist with ordinary fooyin transport and a separately
  loaded Vim Motions plugin; no custom Vim integration is necessary.
- [x] No preview action emits main-player transport/track events, changes main
  volume/output settings, or mutates the queue/playlist.

## Implementation notes

- `HeadphonesActions` owns the five registered actions and their enable/help
  state. Commands have the planned stable IDs/categories and no default bindings.
  Keyboard commands use current focused selection; the panel uses the retained
  display selection. Selection updates only refresh availability, never playback.
- The public track-menu renderer creates a menu-owned action capturing the
  immutable first track of its supplied TrackSelection. Triggering rechecks source
  and device readiness without querying a new selection. QueueViewer already
  includes the common track menu, so no duplicate queue-area entry is registered.
- `HeadphonesWidget` is a FyWidget registered as `HeadphonesPreview`, display name
  Headphones Preview, category Playback, limit one. It observes the shared service
  and does not own or stop it. Metadata/device labels use plain text; controls
  have accessible names and stable object names. Volume has visible percentage
  feedback. Unknown-duration/unseekable inputs have no enabled seek control.
- Seek dragging displays a tentative value and sends one request on release.
  Pending seek feedback is retained until the current worker startup completes;
  keyboard changes can replace the pending target. Qt feedback updates cannot
  emit a second seek. Normalised slider coordinates avoid narrowing long durations
  to int milliseconds, and paused state remains owned by PreviewController.
- `HeadphonesSettingsDialog` stages its ID/description locally. Hotplug rebuilds
  the choices without dropping a missing saved/pending ID; duplicate names include
  encoded IDs. Apply/OK updates only the device, Cancel discards unapplied edits,
  and live volume has no dialog field that can overwrite newer panel changes.
- `PluginConfigGuiPlugin` is declared in both inheritance and Q_INTERFACES.
  The host provider and panel button share one central PluginSettingsProvider
  dialog through weak adapters. Opening it never starts audio. Shutdown destroys
  dialogs/panels/actions before controller/device/settings teardown and unregisters
  commands. Widget factory callbacks weakly guard service lifetime; stale factories
  cannot recreate a panel after shutdown. Public WidgetProvider has no unregister
  API, so tests release its registration callbacks before actually unloading a
  module; the inspected host has restart-based unload and Qt's default unload pin.

## Completion evidence

- Matching baseline: Debian 13 x86_64, GCC 14.2.0, CMake 3.31.6, Qt 6.8.2, fooyin
  0.13.1 `bb8d9f037`, existing `build-rebase-agent`, and the staged Qt Multimedia
  development prefix used in Phases 1–3. No private fooyin headers were introduced.
- Debug plugin/test and RelWithDebInfo builds passed. `ctest --preset test-agent
  -V` passed all six suites: lifecycle 10, engine 39, source 6, converter 14,
  output 13, GUI 8 = **90 Qt Test checks**, including suite setup/cleanup.
- New GUI tests cover first-track/menu snapshots, focus/display-selection
  separation, empty/unsupported selections, action registration/removal, staged
  settings with hotplug/missing/duplicate devices, shared provider lifecycle,
  pause-preserving single-request scrubbing, safe oversized-duration arithmetic,
  volume feedback, unknown duration,
  and panel destruction/recreation without stopping its session.
- The loaded-module test uses real public ActionManager, SettingsManager and
  WidgetProvider objects, discovers PluginConfigGuiPlugin with qobject_cast,
  creates the actual registered panel, verifies its limit/shared configuration,
  round-trips panel volume through actual plugin settings wiring, and verifies
  shutdown/factory/action cleanup before module unload.
- A fixture setup failure exposed ActionManager's requirement for a main window
  before registering actions. The test fixtures now provide it through the public
  setter. Focused GUI and loaded-module lifecycle suites passed an AddressSanitizer
  + UndefinedBehaviorSanitizer build (`build-asan-agent`, leak detection disabled
  for Qt); this checks the implemented QObject/dialog/panel teardown paths.

### Actual plugin and main player in the same process

`tests/inprocessdriver.cpp` is an included, opt-in test plugin, built under
`build-test-agent/tests/inprocess` and never installed. It only uses public host
interfaces; it does **not** instantiate a preview controller or a replacement
Headphones module. It drives the actual loaded plugin through its registered
track menu, ActionManager shortcuts, WidgetProvider panel and configuration UI.
The driver requires `HEADPHONES_INPROCESS_TEST=1`, two explicit distinct Qt output
IDs, three distinct 20-second local fixtures, an isolated configuration and the
default English UI. README contains the launch command.

The executed test loaded the actual Headphones plugin, the driver's module, and
the sibling Vim Motions module into the matching fooyin process. The main player
used its real engine/decoder and a test-only explicit-device QAudioSink backend
on software sink A; the actual Headphones plugin used its normal output path on
software sink B. The driver waited until the main engine had submitted at least
48,000 frames, then verified:

1. An actual context-menu preview action retained Preview A despite subsequent
   focus/selection changes to Preview B.
2. A configured Ctrl+Alt+Shift+F12 shortcut paused preview with Vim Motions loaded.
   Main position and submitted output frames continued increasing during pause.
3. Live volume changed to 61%, and a seek to 10 seconds remained paused.
4. Resume and panel destruction/recreation preserved the actual shared session.
5. Opening settings, same-ID Apply, changed-ID stop, restoring the device without
   autoplay, and Cancel used the real plugin UI.
6. A focused-selection Ctrl+Alt+Shift+F11 shortcut previewed Preview B; clicking
   the panel after moving focus still used its retained display selection.
7. Across the preview-only interval, main track, playlist contents and queue
   snapshots stayed unchanged; main volume/output settings did not change; and
   signal spies/subscriptions saw zero main play/pause/stop, seek, play-state,
   track/play-count, device/output or volume/output-setting events.

The final run printed `HEADPHONES SAME-PROCESS PASS` with **85,207 main output
frames** and exited status 0 with clean Headphones/Vim shutdown. This establishes
same-process host/UI/engine coexistence on the documented software backend. It is
not physical PipeWire disconnect/leak-free routing acceptance; the Phase 3
limitation remains recorded and requires separate hardware/backend work.
