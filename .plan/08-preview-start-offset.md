# Ticket 08: fixed configurable preview start offset

Status: implemented. Automated settings, GUI/action, and engine sample-level
regressions cover the acceptance criteria below.

## Goal and scope

Start explicit headphone previews at a fixed user-configured offset instead of
always starting at the beginning. The default is **0 seconds**.

This ticket is limited to a fixed time offset. Percentage-based starts, cue
points, remembered per-track positions, and looping are outside its scope.

## Behaviour and settings

- Add `Headphones/PreviewStartOffsetSeconds`, a nonnegative integer with default
  `0`, and a labelled seconds spin box in the plugin settings dialog.
- Stage edits until Apply/OK; Cancel discards unapplied changes. Applying an
  offset does not stop, seek, or restart an existing preview.
- Use the latest applied offset for Preview selected/context-menu/shortcut
  requests and explicit restarts, including Play on an already playing track
  and Play/PlayPause after Stop or EOF.
- Resuming a paused preview retains its position. Manual seeking uses the
  requested target directly and never adds the configured offset.
- Interpret the offset relative to the logical track start. For CUE/chapter
  segments, add the source segment offset exactly once in the worker.
- For a known duration, an offset at or beyond the duration falls back to the
  track beginning, allowing short tracks to remain auditionable. For unknown
  duration, attempt the requested offset; EOF before audible audio stops normally.
- For a source that cannot seek, fall back to the logical beginning when that
  beginning is playable. Preserve the existing error for unseekable segments
  whose source offset already requires seeking. Do not perform long decode-and-
  discard solely to implement a configured offset on an unseekable source.
- Invalid/negative/overflowing persisted values resolve to the default; define
  a representable UI maximum and check seconds-to-milliseconds/source-offset
  arithmetic before use.

## Implementation plan

1. Add the setting, validated getter/setter, and staged dialog control to
   `HeadphonesSettings` and `HeadphonesSettingsDialog`.
2. Pass the applied policy to `PreviewController`. Separate a fresh/restarted
   preview's initial target from resume and explicit seek paths; `begin()` is
   currently shared by these operations.
3. Resolve seekability fallback after source open on the worker. Ensure the
   effective initial position reported to the controller matches emitted PCM,
   rather than retaining an offset that was not actually applied.
4. Preserve generation cancellation, pause state, segment-boundary trimming,
   and exact seek/preroll behaviour. Stop/EOF still reset the displayed position
   to zero and retain the track without autoplay.
5. Document the setting, restart semantics, and short/unseekable-track fallback.

## Acceptance criteria

- [x] Default/missing setting preserves existing start-at-zero behaviour.
- [x] Settings round-trip, Apply/Cancel, and invalid/oversized stored values are
  covered; changing the offset leaves ongoing playback untouched.
- [x] Fresh preview and explicit restart begin at the offset; pause/resume and
  manual seeking do not reapply it.
- [x] Worker/engine tests inspect first emitted samples for whole files and
  bounded segments, including exact source offset application.
- [x] Short tracks, unknown duration/EOF, unseekable sources, and source timeline
  overflow follow the documented rules without stale-session audio.
- [x] Main playback, queue, volume, and output remain independent.

## Implementation notes

- The spin box and persisted setting accept 0–2,147,483,647 whole seconds;
  invalid, negative, fractional, or oversized saved values read as 0.
- `PreviewController` snapshots the applied policy into each fresh generation.
  Resume and manual seek retain their separate semantics.
- The worker resolves duration/seekability fallback before source-timeline
  arithmetic and publishes the effective logical position with its open result.
  Source offsets and bounded end times are range-checked before seeking.
- Regression coverage lives in `previewenginetest`, `previewoutputtest`, and
  `headphonesguitest`; existing default-offset tests exercise unchanged playback.

Validation: `cmake --build --preset test-agent -j 4` and
`ctest --preset test-agent` pass (all seven test suites).
