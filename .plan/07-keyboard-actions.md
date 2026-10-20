# Ticket 07: complete headphone keyboard actions

Status: implemented with the user-approved plugin-only select/reveal scope.
Host modifications are forbidden.

## Goal

Allow headphone volume, configuration, and navigation to the previewed track to
be controlled through fooyin's shortcut system, including without preview widgets.

## Requested actions

Register these stable IDs under the existing **Headphones Preview** category,
with translated labels/descriptions and no default bindings. Use Headphone
playback, Headphone seek, Headphone tracks, and Headphone volume subcategories to
keep related commands together without colliding with main-player categories.
Configure stays at the category root.

| Action ID | Label | Behaviour |
| --- | --- | --- |
| `Headphones.Volume.Up` | Volume up | Increase the normalised UI volume by 5 percentage points, clamped to 100% |
| `Headphones.Volume.Down` | Volume down | Decrease the normalised UI volume by 5 percentage points, clamped to 0% |
| `Headphones.Volume.Mute` | Mute | Set volume to zero; repeated calls preserve the remembered nonzero level |
| `Headphones.Volume.Unmute` | Unmute | Restore the remembered nonzero level only when currently muted |
| `Headphones.Volume.ToggleMute` | Toggle mute | Toggle between zero and the remembered nonzero level |
| `Headphones.Configure` | Configure | Open the same settings dialog used by plugin configuration and the Configure button |
| `Headphones.Tracks.FocusPreviewed` | Show previewed track | Locate the retained preview entry, select matching tracks, and reveal the entry |

The 5-percentage-point volume increment is the proposed initial fixed step.
Volume actions operate on the shared controller, including while stopped or with
no output configured. Increasing volume from zero raises it to 5%; decreasing
volume from zero leaves it at zero. Unmute follows the existing controller's
remembered-volume/default policy and does not start playback.

## Track-navigation behaviour

- Target the preview track, never the current ambient selection or main track.
- Permit navigation whenever a valid preview track is retained, including while
  paused and after Stop. Disable before any valid preview track exists.
- Retain source playlist/entry identity in the GUI integration when available;
  resolve it again on invocation so reorder/removal does not use a stale row.
- Prefer the original playlist entry. If it no longer exists, prefer a matching
  track in the current playlist, then the first matching loaded playlist in
  handler order. Match logical track/segment identity, not filename alone.
- Change the visible playlist if needed, select matching tracks, and reveal the
  target. The user accepted that matching duplicates may also be selected and
  keyboard focus is not guaranteed. Host selection equality can also include
  other segments from the same source file. Target resolution and reveal still
  use logical track/segment and entry identity. Existing filters are respected.
  If there is no matching entry, do nothing; never insert a track or create a
  playlist to make navigation possible.
- This explicit navigation action may change UI selection, but must preserve
  preview playback/position and main playback, active playback playlist, and queue.

## Implementation plan

1. Extend `HeadphonesActions` registration, lifetime cleanup, enabled states, and
   action descriptions. Add explicit controller mute/unmute operations so all
   three mute commands and the widget share one restoration policy.
2. Wire Configure through `HeadphonesSettingsProvider`; guard provider lifetime
   and reuse the host-managed dialog rather than creating a second dialog path.
3. Implement track navigation in the GUI layer using `GuiPluginContext` services:
   `playlistSelection` (`CurrentPlaylistController`), `playlistInteractor`, and
   the public playlist handler/selection metadata. Keep these dependencies out
   of the audio controller.
4. Use public `changeCurrentPlaylist()`/`selectTracks()` and
   `ensurePlaylistItemVisible()` for the accepted select/reveal behaviour. Do not
   modify the host, reach into private widgets, or invoke the main player's
   focus-playing-track command.
5. Update the shortcut/action table in the README when implemented.

## Acceptance criteria

- [x] All seven commands appear in fooyin's shortcut settings and unregister on
  plugin shutdown; existing action IDs and bindings continue to work.
- [x] Volume bounds, repeated mute/unmute, toggle restoration, and multiple
  widgets agree on one shared volume; no command starts audio.
- [x] Configure works without widgets and reuses the existing dialog.
- [x] Navigation works after selection changes and playlist reordering, resolves
  the original entry/segment for reveal, and handles removed tracks/playlists and
  absent GUI services without insertion, playback mutation or crashes. Selection
  uses the host's matching rules rather than an exact-one-entry guarantee.
- [x] GUI/lifecycle tests cover command invocation and provider lifetime; a
  same-process check confirms selection/reveal and unchanged audio/queue state.

## Implementation and validation

- `HeadphonesActions` registers all seven IDs without defaults, uses the shared
  controller for volume/mute, and holds the settings provider weakly. The provider
  reuses the same dialog as plugin configuration and player-control buttons.
- Shortcut names stay concise and related commands use nested, plugin-specific
  category labels. This avoids the host's label-only category collisions without
  any host changes.
- Focus-sensitive, display, and immutable context-menu selections retain the first
  track's playlist/entry identity. Navigation resolves the original entry again, then
  uses `Track::sameIdentityAs()` for current/loaded-playlist fallbacks. Controller
  audio code remains independent of playlist and player services.
- All assistant-introduced changes to `../fooyin` have been removed, including
  the proposed navigation API, shortcut-model change, and host regression test.
- Navigation uses existing public selection/reveal APIs with bounded retries
  while the host loads its playlist view. Each try re-resolves the entry ID;
  further user input, target removal, playlist changes, or preview replacement
  cancels a pending request. No private widget or main-player focus command is used.
- Built `/workspace/fooyin/build-rebase-agent` and `build-test-agent`; all seven
  CTest suites passed. GUI/lifecycle coverage includes action registration/removal,
  bounds and repeated mute operations without output, shared widgets, Configure
  without widgets, provider expiry, and missing playlist GUI services.
- The same-process software-sink workflow on unmodified fooyin verifies matching
  selection and the revealed entry's visible rectangle: original duplicate after
  reorder, paused position retention, navigation after Stop, segment-aware reveal
  targets, loaded-playlist fallback after removal, and missing-target no-op.
  The fixture explicitly checks the host's broader duplicate/segment selection.
  Main transport events, active playback playlist, output, volume and queue remain
  unchanged. The final plugin-only run also verified user-input cancellation and
  removal of a pending entry before the first selection attempt: 22 separate-routing
  observations in 8.77 seconds. Artifacts:
  `/tmp/opencode/headphones-public-api-final-validation`.
