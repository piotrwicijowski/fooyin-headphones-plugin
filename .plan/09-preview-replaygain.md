# Ticket 09: independent preview ReplayGain

Status: planned.

## Goal

Reduce loudness differences when auditioning tracks by applying ReplayGain in
the preview pipeline, independently of the main player's processing and volume.

## Scope and proposed settings

Expose these preview-specific settings in the existing configuration dialog,
with Apply/OK/Cancel semantics:

| Setting | Proposed default | Meaning |
| --- | --- | --- |
| `Headphones/ReplayGainMode` | `Off` | Off, Track, or Album |
| `Headphones/ReplayGainPreampDb` | `0.0` | Additional gain when ReplayGain metadata is available |
| `Headphones/ReplayGainWithoutInfoPreampDb` | `0.0` | Gain when neither requested nor fallback gain metadata exists |
| `Headphones/ReplayGainPreventClipping` | `true` | Limit applied gain using a valid peak value when available |

Off preserves the current decoded signal, including decoder-defined source gain.
Track mode prefers track gain and falls back to album gain; Album mode prefers
album gain and falls back to track gain. Use the corresponding valid peak where
available and document peak fallback consistently with fooyin's behaviour.
Reject non-finite metadata/settings and nonpositive peak values; validate bounded
preamp ranges. Missing peaks must not be treated as proof that gain cannot clip.

Applying these settings affects the next fresh/restarted preview. An active
session, including pause/resume and manual seeks, retains its captured ReplayGain
policy; applying settings alone does not restart or change its loudness.

This ticket covers metadata-based gain and peak-based clipping prevention, not
ReplayGain scanning, dynamic loudness analysis, a limiter, or full main-DSP parity.

## Implementation plan

1. Add a small immutable ReplayGain policy captured for a fresh preview session
   and retained across that session's seek generations.
2. Calculate gain from the retained track's public metadata and apply it on the
   decoder worker in floating-point PCM before final integer quantisation.
   Keep `QAudioSink` volume as the independent user-volume stage: it cannot
   represent arbitrary amplification above unity on its own.
3. Inspect fooyin's ReplayGain implementation as a behavioural reference, using
   only installed public headers in the plugin. Check decoder/Opus header-gain
   treatment explicitly so source gain is neither omitted nor applied twice;
   do not assume effective metadata getters can simply be multiplied onto
   already adjusted PCM.
4. Define processing order with downmixing/resampling. Peak metadata describes
   source audio; it cannot by itself guarantee no downmix or resampling overshoot.
   Ensure the clipping-prevention description matches the implemented guarantee.
5. Retain the native-format passthrough path when no sample processing is needed;
   disable passthrough when a non-unity gain requires actual sample changes.
6. Wire settings/dialog persistence and document preview-specific operation and
   next-session application. Do not change fooyin's main ReplayGain settings.

## Acceptance criteria

- [ ] Off/default leaves baseline output samples unchanged.
- [ ] PCM tests verify known positive/negative dB gains, both mode preferences,
  metadata fallback, tagged/untagged preamps, and valid-peak gain limiting.
- [ ] Missing/malformed gain and peak values produce finite, defined output.
- [ ] Opus/source-gain fixtures establish that decoder gain is applied exactly
  once for Off, Track, and Album modes.
- [ ] Integer and float output, resampling tails, seek/replacement cancellation,
  and muted preview behave correctly with gain processing enabled.
- [ ] Applying settings does not alter a running/paused session; fresh preview
  adopts them and manual seeking retains the active session's policy.
- [ ] Main audio samples/settings, main volume, and queue remain unchanged.
