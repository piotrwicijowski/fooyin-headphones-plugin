# Ticket 10: multichannel input to stereo headphone downmix

Status: planned.

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

- [ ] Per-channel impulse fixtures verify ordering, routing, and matrix gains for
  each accepted layout, including side/rear 5.1 and conventional 7.1 variants.
- [ ] Correlated full-scale input remains within the documented matrix headroom;
  isolated LFE contributes no signal under the initial policy.
- [ ] Unknown/ambiguous layouts are rejected without silently guessing routing.
- [ ] Mono/stereo baseline samples and existing device negotiation remain correct.
- [ ] Mixed-rate conversion, integer/float outputs, mono fallback, EOF tails, and
  seek/segment boundaries preserve frame counts and expected audible content.
- [ ] Large channel counts, malformed frames, runtime layout changes, and size
  arithmetic cannot bypass allocation bounds or revive cancelled audio.
- [ ] A real decoded multichannel fixture plays through stereo output while main
  playback and its queue/settings remain unaffected.
