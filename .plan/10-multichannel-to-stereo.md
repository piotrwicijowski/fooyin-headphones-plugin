# Ticket 10: multichannel input to stereo headphone downmix

Status: implemented; automated and same-process software-sink validation pass.

## Goal and scope

Allow multichannel local tracks to be auditioned through ordinary stereo
headphones using a channel-layout-aware downmix. Initial acceptance targets are
conventional 3.0, quad, 5.0, 5.1, and 7.1 layouts with identified channel roles.

Preserve existing mono/stereo conversion. This ticket concerns input downmixing;
physical output channel-pair selection is tracked separately in
[11-output-channel-pair-selection.md](11-output-channel-pair-selection.md).

## Downmix policy

- Use the decoder's explicit channel layout, including side versus rear surround
  positions. Do not infer a multichannel speaker arrangement from channel count
  alone; reject missing, ambiguous, or unsupported layouts with a useful reason.
- Define and document an explicit stereo matrix: front left/right retain their
  sides, centre contributes equally to both sides, and surround channels feed
  the corresponding side. Proposed relative centre/surround coefficients are
  `1/sqrt(2)` before headroom normalisation; omit LFE in the initial policy.
- Apply one common matrix normalisation factor based on the largest row's sum
  of absolute coefficients, keeping each row sum at most unity while preserving
  relative balance. This avoids full-scale correlated-input overload from the
  matrix itself; it does not promise to prevent resampler overshoot.
- Preserve existing mono fallback for output devices that cannot accept stereo:
  derive it from the defined stereo downmix with the existing averaging rule.
- Continue rejecting malformed PCM, unsupported runtime format/layout changes,
  and formats beyond the established rate/allocation limits.

## Implementation plan

1. Split `isPreviewFormat()`'s current shared mono/stereo restriction into explicit
   accepted-input and accepted-output validation. Update worker checks and format
   negotiation so valid multichannel sources can reach conversion.
2. Map fooyin's public channel-layout representation to `AVChannelLayout` without
   losing channel ordering. Verify public float normalisation preserves the
   source channel layout; avoid an implicit count-only channel remap.
3. Extend `PreviewConverter` with the documented layout-specific matrix using
   existing libswresample support, keeping conversion on the worker.
4. Recalculate input block, staging, ring-buffer, and frame-size arithmetic for
   wider inputs with checked multiplication and existing memory bounds. Output
   buffering remains based on the negotiated output frame size.
5. Keep source-timeline seek/preroll and segment clipping before conversion;
   flush only the current converter generation at EOF.
6. Check interaction with [09-preview-replaygain.md](09-preview-replaygain.md):
   define gain/downmix order and avoid claiming source peaks bound all possible
   post-resampling peaks. Neither ticket is a prerequisite for the other.
7. Document accepted layouts, coefficients/headroom, LFE omission, and errors.

## Acceptance criteria

- [x] Per-channel impulse fixtures verify ordering, routing, and matrix gains for
  each accepted layout, including side/rear 5.1 and conventional 7.1 variants.
- [x] Correlated full-scale input remains within the documented matrix headroom;
  isolated LFE contributes no signal under the initial policy.
- [x] Unknown/ambiguous layouts are rejected without silently guessing routing.
- [x] Mono/stereo baseline samples and existing device negotiation remain correct.
- [x] Mixed-rate conversion, integer/float outputs, mono fallback, EOF tails, and
  seek/segment boundaries preserve frame counts and expected audible content.
- [x] Large channel counts, malformed frames, runtime layout changes, and size
  arithmetic cannot bypass allocation bounds or revive cancelled audio.
- [x] A real decoded multichannel fixture plays through stereo output while main
  playback and its queue/settings remain unaffected.

## Implementation notes

- Input and output validation are separate. Inputs accept conventional mono/stereo
  and explicit 3.0, rear/side quad, rear/side 5.0 and 5.1, standard 7.1, and wide
  rear/side 7.1. Outputs remain conventional mono/stereo F32, S16, or S32.
- The plugin validates the public `AudioFormat` speaker roles, including uniqueness,
  without manufacturing multichannel roles from a count. Fooyin's public API may
  itself populate defaults; it exposes no provenance flag to distinguish those
  from container metadata. Unknown layouts explicitly cleared by a decoder fail.
- An explicit `AVChannelLayout` mask represents accepted roles, with a persistent
  `swr_set_channel_mapping()` input map preserving arbitrary source interleaving.
  Float normalization copies the input layout, so the public conversion helper
  does not reorder lanes into a count-derived default layout.
- The matrix uses front L/R = 1, centre to both sides = `1/sqrt(2)`, surround and
  wide front-of-centre channels to their respective side = `1/sqrt(2)`, LFE = 0.
  Both rows share the maximum absolute row-sum normalization (at least 1).
  Divisors are `1 + 1/sqrt(2)` for 3.0/quad, `1 + 2/sqrt(2)` for 5.x, and
  `1 + 3/sqrt(2)` for 7.1. Mono fallback averages these normalized stereo rows.
- ReplayGain precedes mixing/resampling/quantization. Matrix headroom describes
  bounded source samples after gain, not all possible resampler output peaks.
- Worker read blocks remain bounded to 4096 source frames (at most 256 KiB for
  8-channel F64). Float normalization is at most 128 KiB per block. Checked byte
  arithmetic caps staging/tails/buffers at 8 MiB; the output queue remains half a
  second of negotiated mono/stereo PCM, independent of input channel count.
- Source-timeline preroll/segment clipping and generation cancellation remain
  before conversion; EOF flushes only the current session's converter.

## Validation

- `cmake --build --preset test-agent -j 4` and `ctest --preset test-agent`: all
  eight suites pass. Converter fixtures cover ten accepted layouts in native and
  permuted order, F32/S16 input, isolated-channel routing (including silent LFE),
  correlated full-scale signals, and gain before downmix. Mixed-rate fixtures
  cover mono/5.1/7.1 inputs, stereo/mono outputs, and F32/S16/S32 quantization.
- ASan/UBSan converter, engine, and output suites pass using `build-asan-agent`
  (`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`).
- `tests/run-inprocess-validation.py --multichannel --seconds 10` passes with the
  matching Qt 6.8.2 host and PulseAudio 17.0 private null sinks. FFmpeg exposes
  `FL,FR,FC,LFE,SL,SR` and `FL,FR,FC,LFE,BL,BR,SL,SR` for the real 5.1/7.1 WAVs.
  The run completed 18 stress cycles and 40 separate-routing observations,
  retaining the main track/playlist/queue/output/volume with zero main transport
  events. Logs/fixtures/report: `/tmp/opencode/headphones-downmix-validation-2`.
  This is software-sink validation; physical listening was not performed.
