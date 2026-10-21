# Ticket 11: explicit output channel-pair selection

Status: implemented; Qt/PulseAudio software capture validated. Physical shared-interface
validation and a Qt 6.4 runtime check remain outstanding.

## Implemented experience and feasibility

- One shared destination dropdown in the output widget and Configure. Ordinary
  devices keep one entry; known-layout multichannel devices add `Automatic` and
  `Channels 1/2`, `Channels 3/4`, etc. entries. The collapsed text includes the pair.
- Widget activation commits device + pair atomically. Dialog changes stage until
  Apply/OK; Cancel discards drafts. Unchanged application does not stop preview.
- `Headphones/OutputRoute` binds the selected pair to the opaque device identity.
  Missing devices/pairs stay visibly unavailable and block playback with no fallback
  or reconnection autoplay. Existing settings migrate to Automatic.
- Qt's reported channel configuration and a supported concrete format establish
  pair choices, never maximum channel count. The initial implementation supports
  canonical known layouts up to eight channels; unknown/AUX-only maps are excluded.
- Logical stereo conversion precedes explicit zero-filled physical slot mapping.
  Worker approval, queues, partial writes, sink format, and clocks retain physical
  output frame sizes.
- `tests/run-channel-pair-validation.py` captures real Qt output on PulseAudio 17.0
  with a 48 kHz quad null sink. Qt 6.8.2 delivered exact silence in unselected lanes
  for both pairs, and distinct concurrent streams remained on separate pairs.
  Tests include 44.1-to-48 kHz conversion, preview pause/resume and sink replacement.
  See `docs/development.md` for reproduction and the remaining physical checks.

## Goal and scope

Route headphone preview to an explicitly selected pair, such as channels 3/4,
on a multichannel audio interface. This enables a setup where fooyin's main
output and preview use different pairs of the same interface when the backend
supports concurrent streams and preserves their channel mapping.

This is output routing, independent of multichannel input support. Mono/stereo
sources must work without requiring
[10-multichannel-to-stereo.md](10-multichannel-to-stereo.md); with that feature,
the stereo downmix feeds the selected pair.

## Routing contract and configuration

- Retain today's automatic mono/stereo output mode as the default, preserving
  existing installations. Add an explicit channel-pair mode with user-visible
  1-based labels (1/2, 3/4, etc.) tied to the selected output device.
- Offer only pairs backed by a verified concrete output format/channel mapping.
  A device's maximum channel count alone does not establish pair support.
- Persist the routing mode and selected pair under `Headphones/...`, bound to
  the selected device identity. A changed device requires compatible explicit
  pair validation rather than silently reusing a pair for different hardware.
- Stage dialog edits until Apply/OK. A changed routing mode/pair stops and resets
  active preview before replacement; unchanged Apply must not interrupt it.
- If a saved explicit pair is unavailable, leave it visibly unavailable and
  refuse playback. Do not fall back to another pair or automatic stereo mode.
- Emit preview left/right only on the selected channels. Fill every other
  channel of the plugin's stream with digital silence, including after seek,
  pause/resume, and conversion-tail flush.
- Continue requiring explicit playback after routing changes or reconnection.
  The user configures fooyin's main pair separately; this feature does not change
  main output profiles or start a monitoring/mixing stream.

## Implementation plan

1. Establish a backend prototype using the supported public Qt APIs. Verify
   concrete multichannel `QAudioFormat` negotiation, channel configuration/order,
   and actual delivered channel mapping, including Qt 6.4 compatibility. Check
   simultaneous main/preview streams on separate pairs of the same interface.
2. Determine whether `QAudioSink` can preserve the required mapping on the target
   backend. If it cannot, record the exact limitation and the smallest public
   output-backend/API extension needed before implementing the routing UI. Do not
   treat successful sink creation as evidence of physical pair isolation.
3. Extend the device/output capability boundary to represent supported layouts
   and pair choices. Replace the current channel-count-only Qt/fooyin format
   conversion where necessary to retain channel positions/order.
4. Separate logical preview stereo frames from physical output frames. Produce
   the stereo signal, then map it into the selected interleaved output slots
   with all other slots zero; use explicit routing rather than a generic stereo-
   to-surround upmix. Preserve correct Float/Int16/Int32 sample containers.
5. Update worker approval, PCM queues, pending/partial writes, byte capacities,
   sink format, and position accounting to use physical output frame sizes.
   Do not broaden the converter's input validation merely to permit wide output.
6. Add persistent routing settings and consistent configuration/output-widget
   presentation; revalidate after device capability changes and on every open.
7. Document a verified main-pair/preview-pair setup and its tested backend/device
   requirements, including the distinction between channel pairs and mutually
   exclusive speaker/headphone ports.

## Acceptance criteria

- [x] Automatic mode retains existing mono/stereo behaviour and settings migration
  requires no action for existing users.
- [x] Fake-device negotiation tests reject unsupported channel counts/layouts,
  missing explicit pairs, and same-name/different-ID substitutes.
- [x] PCM fixtures place distinct left/right signals only on the selected pair;
  every unselected channel is zero across rate conversion, gain processing when
  enabled, seek, EOF tail, and partial-write boundaries.
- [x] Queue/staging memory limits, frame alignment, and processed position remain
  correct with wide physical output frames.
- [x] Apply/Cancel, device changes, capability loss, restart persistence, and
  hotplug preserve explicit selection and do not autoplay or fall back.
- [ ] Software channel capture verifies the stream mapping; physical multichannel
  hardware validates the actual pair mapping and concurrent main/preview output,
  including replacement, pause, seek, stop, and disconnect behaviour.
- [x] Main playback, its routing/volume settings, active playback playlist, and
  queue remain unchanged throughout the preview workflow.
