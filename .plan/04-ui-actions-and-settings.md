# Phase 4: actions, layout widget, and settings

Depends on: [02-session-and-decoding.md](02-session-and-decoding.md) and
[03-audio-output-and-devices.md](03-audio-output-and-devices.md).

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
| `Headphones.PreviewSelected` | Preview in headphones | Start selected track from its beginning |
| `Headphones.PlayPause` | Play/pause headphone preview | Pause/resume or restart retained preview |
| `Headphones.Stop` | Stop headphone preview | Discard audio and reset preview position |
| `Headphones.SeekBackward` | Seek headphone preview backward | Seek back 5 seconds when seekable |
| `Headphones.SeekForward` | Seek headphone preview forward | Seek forward 5 seconds when seekable |

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

- [ ] QAction/menu integration targets the correct track even if focus changes.
- [ ] Multi-selection, empty/unsupported selections, and command enabled states
  follow the documented rules.
- [ ] Widget observes the shared controller, supports pause-preserving seeking,
  and can be destroyed/recreated without resetting main or preview playback.
- [ ] Settings round-trip opaque IDs, duplicate names, missing-device state,
  volume bounds, Apply/Cancel, and no autoplay after restart/reconnect.
- [ ] Keyboard actions coexist with ordinary fooyin transport and a separately
  loaded Vim Motions plugin; no custom Vim integration is necessary.
- [ ] No preview action emits main-player transport/track events, changes main
  volume/output settings, or mutates the queue/playlist.
