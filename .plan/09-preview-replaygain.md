# Ticket 09: preview ReplayGain using fooyin's configuration

Status: implemented. Shared-configuration PCM, session-lifecycle, and real native
Opus validation cover the acceptance criteria below.

## Goal

Reduce loudness differences when auditioning tracks by applying ReplayGain in
the preview pipeline using fooyin's main ReplayGain configuration as the sole
source of configuration values. Preview processing and user volume remain
independent of the main player's audio pipeline.

## Scope and shared configuration

Read the existing settings through fooyin's shared `SettingsManager` and public
`Settings::Core` keys. Do not add headphones-only ReplayGain settings, persisted
copies, defaults, overrides, or controls in the plugin configuration dialog.
Users configure ReplayGain through fooyin's existing ReplayGain settings page.

| Existing fooyin setting | Preview behaviour |
| --- | --- |
| `Settings::Core::RGMode` (`Engine/ReplayGainMode`) | Reuse Disabled, Apply gain, Apply gain and prevent clipping, or Only prevent clipping |
| `Settings::Core::RGType` (`Engine/ReplayGainType`) | Reuse Track, Album, or gain based on playback order |
| `Settings::Core::RGPreAmp` (`Engine/ReplayGainPreAmp`) | Reuse the preamp for tracks with ReplayGain metadata |
| `Settings::Core::NonRGPreAmp` (`Engine/NonReplayGainPreAmp`) | Reuse the preamp for tracks without ReplayGain metadata |

Reuse fooyin's registered defaults and processing semantics, including the
clipping-only mode rather than reducing processing to an on/off clipping flag.
Disabled preserves the current decoded signal, including decoder-defined source
gain. Track mode prefers track gain and falls back to album gain; Album mode
prefers album gain and falls back to track gain. Resolve playback-order selection
using the shared `Settings::Core::PlayMode`, exactly as fooyin does: Track when
shuffling tracks, otherwise Album. Use fooyin-compatible peak preference and
fallback for the selected type. Apply the tagged/untagged preamps only when gain
application is enabled; clipping-only mode uses peak-based attenuation without
ReplayGain or preamp amplification.

Reject non-finite metadata/settings and nonpositive peak values; validate preamps
against fooyin's supported range without introducing separate plugin values.
Missing peaks must not be treated as proof that gain cannot clip.

Capture the current applied main configuration, including resolved playback-order
selection, for each fresh/restarted preview. An active session, including
pause/resume and manual seeks, retains its captured ReplayGain policy. Changes to
the shared settings or playback order affect the next fresh/restarted preview;
they do not restart or change the loudness of an existing session. This snapshot
is transient session state, not a second persisted configuration.

This ticket covers metadata-based gain and peak-based clipping prevention, not
ReplayGain scanning, dynamic loudness analysis, a limiter, or full main-DSP parity.

## Implementation plan

1. Read all ReplayGain values from fooyin's shared settings and capture a small
   immutable policy for a fresh preview session, retained across that session's
   seek generations. Include processing flags, resolved gain type, and both
   preamps; do not register or persist plugin-specific ReplayGain settings.
2. Calculate gain from the retained track's public metadata and apply it on the
   decoder worker in floating-point PCM before final integer quantisation.
   Keep `QAudioSink` volume as the independent user-volume stage: it cannot
   represent arbitrary amplification above unity on its own.
3. Match fooyin's ReplayGain implementation for processing modes, playback-order
   selection, gain/peak fallback, and preamps, using only installed public headers
   in the plugin. Check decoder/Opus header-gain treatment explicitly so source
   gain is neither omitted nor applied twice; do not assume effective metadata
   getters can simply be multiplied onto already adjusted PCM.
4. Define processing order with downmixing/resampling. Peak metadata describes
   source audio; it cannot by itself guarantee no downmix or resampling overshoot.
   Ensure the clipping-prevention description matches the implemented guarantee.
5. Retain the native-format passthrough path when no sample processing is needed;
   disable passthrough when a non-unity gain requires actual sample changes.
6. Document that previews reuse fooyin's main ReplayGain configuration and adopt
   changes on the next fresh/restarted session. Do not add ReplayGain controls to
   `HeadphonesSettingsDialog` or ReplayGain persistence to `HeadphonesSettings`.
   Reading/capturing the shared configuration must not write or reset it.

## Acceptance criteria

- [x] Disabled leaves baseline output samples unchanged; initial behaviour uses
  fooyin's registered defaults/current configuration without plugin overrides.
- [x] No headphones-only ReplayGain keys, persisted copies, or dialog controls
  are introduced; all configuration values come from the shared fooyin settings.
- [x] PCM tests verify all four processing modes, known positive/negative dB
  gains, Track/Album preferences, playback-order selection for shuffled and
  non-shuffled tracks, metadata fallback, shared tagged/untagged preamps, and
  valid-peak gain limiting, including clipping-only behaviour.
- [x] Missing/malformed gain and peak values produce finite, defined output.
- [x] Opus/source-gain fixtures establish that the selected decoder's source-gain
  policy is preserved without adding header gain again for Disabled and each
  enabled processing/gain-selection mode.
- [x] Integer and float output, resampling tails, seek/replacement cancellation,
  and muted preview behave correctly with gain processing enabled.
- [x] Applying main ReplayGain settings or changing playback order does not alter
  a running/paused session; fresh/restarted preview adopts the latest shared
  configuration and manual seeking retains the active session's policy.
- [x] Main audio samples/settings, main volume, and queue remain unchanged.

## Implementation notes

- `PreviewReplayGain::capture()` reads the host-owned `SettingsManager` through
  public keys; the plugin never registers, writes, or resets ReplayGain settings.
  `HeadphonesPlugin` supplies the manager to `PreviewController` once. A guarded
  pointer handles manager destruction without retaining host services.
- Fresh/restarted previews capture the policy before decoder open; manual seeks
  copy the active session's policy before invalidating the old generation. All
  gain calculation and sample processing run on the decoder worker.
- Fooyin's `ReplayGainType` enum is private. The snapshot adapter documents its
  persisted values (Track 0, Album 1, PlaybackOrder 2) and uses the public
  `Playlist::ShuffleTracks` flag to resolve playback-order selection.
- Gain and peak preference/fallback are independent, matching the main processor.
  Invalid/nonpositive peaks are ignored and permit valid peak fallback. Preamps
  are bounded to the main settings page's -20 to +20 dB range; non-finite preamps
  become 0 dB in the snapshot without writing back. Invalid processing modes are
  disabled. Unrepresentable gain calculations fall back to unity.
- `PreviewConverter` applies non-unity gain after float normalisation and before
  channel mixing/resampling and integer quantisation; unity keeps native-format
  passthrough. Clipping prevention constrains gain against source peak metadata,
  not later resampling overshoot. Headphone sink volume remains independent.
- Raw `Track::rg*` getters match fooyin's main ReplayGain processor. Effective
  getters include Opus header gain and must not be applied to decoder output.
  The native Opus decoder uses `OP_ABSOLUTE_GAIN` to suppress header gain even
  when ReplayGain is disabled; preview preserves that existing decoder policy.
- `previewreplaygaintest` covers shared settings and PCM, bit-exact disabled
  passthrough, session snapshots, pause/seek/restart/EOF/mute, cancellation,
  replacement, and resampler tails. Its optional native-Opus check loads a
  matching public input plugin and verifies zero/+6 dB header fixtures across all
  processing/selection modes; see `docs/development.md` for invocation.

Validation: `cmake --build --preset test-agent -j 4` and
`ctest --preset test-agent` pass (all eight suites). The native-Opus check also
passes with fooyin's current Opus sources built against the matching Qt 6.8.2
host libraries and `tests/data/audio/audiotest.opus` from the fooyin tree.
