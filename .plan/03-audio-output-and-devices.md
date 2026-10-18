# Phase 3: QAudioSink, format conversion, and device policy

Depends on: [02-session-and-decoding.md](02-session-and-decoding.md).

## Explicit device selection

`HeadphoneDevices` owns a GUI-thread `QMediaDevices` instance and publishes
snapshots from `QMediaDevices::audioOutputs()`. Observe `audioOutputsChanged`.

- Store the exact opaque `QAudioDevice::id()` as a byte-preserving encoded value
  (for example Base64), plus description for display only.
- Match the ID on each open; descriptions are not unique identifiers.
- Resolve a non-null device before constructing
  `QAudioSink(selectedDevice, selectedFormat, parent)`.
- Do not use the default-device constructor, a null-device substitute, or a
  preferred-device fallback when a stored ID is missing.
- A stored ID is best-effort identity across restarts, not universally stable
  across driver/server/device changes. An unmatched ID remains unavailable until
  the user selects a device again.
- Stop/reset output if the active device disappears or an output error occurs.
  Invalidate pending startup and show the reason. Reconnection updates the UI
  but never resumes playback automatically.
- Other device-list changes should not stop a valid active session unnecessarily.
- Changing the system default alone should not intentionally change preview's
  selected device.

Keep the Qt enumeration behind a small injectable device-catalogue boundary so
device-loss/ID-resolution behaviour can be tested without physical hardware.

### Routing limits and Linux validation

Separate devices are a setup requirement, not something a generic Qt device ID
can prove. Do not compare Qt IDs with fooyin's ALSA/PipeWire/backend IDs or alter
fooyin's main-output settings. Show the chosen headphone device clearly.

The plugin must never deliberately fall back to default. Nevertheless,
PipeWire/PulseAudio/other OS policy may move a stream before the plugin observes
device loss. Validate disconnect/default-change behaviour with a monitored main
output. If the selected Qt backend reroutes undetectably, document the observed
limitation and investigate backend/server policy before claiming leak-free
hotplug behaviour. QAudioSink alone cannot guarantee physical isolation.

## Output format negotiation

For v1, accept mono/stereo input; output stereo when supported, otherwise use a
supported mono format with explicit remixing. Reject devices/layouts outside
the initial mono/stereo contract rather than guessing channel-pair assignments.

1. Validate decoded sample type, rate, channels, frame alignment, and layout.
2. On the GUI thread, test concrete candidate `QAudioFormat`s using
   `QAudioDevice::isFormatSupported()`.
3. Prefer Float PCM at the source rate and desired channels. If unsupported,
   try a supported device-preferred rate/sample type and sensible common
   44.1/48 kHz stereo/mono candidates. Int16/Int32 are valid conversion targets.
4. Select only a tested supported format; minimum/maximum capability ranges
   alone do not establish that every combination is supported.
5. If none of the supported v1 combinations work, report an output-format error.

Do not simply relabel PCM with a different sample rate. Even if an OS backend
performs some conversion, the plugin must feed bytes matching its declared Qt
format and maintain its own correct frame/position accounting.

## Conversion/resampling

Use a plugin-owned streaming converter on the decoder worker:

- Normalise fooyin sample formats to F32 at the native rate/channel layout using
  its public conversion helper where appropriate. Verify S24In32 amplitude and
  container alignment with fixtures; do not reinterpret it as arbitrary S32.
- Use `libswresample`/`libavutil` explicitly for sample-rate conversion and
  mono/stereo remix/output-format conversion.
- Handle mono duplication and stereo-to-mono with a defined gain matrix; avoid
  clipping introduced by summing channels. Reject unsupported layouts clearly.
- Maintain resampler state across decode blocks, allocate bounded staging
  buffers, and account for conversion delay and changed frame counts.
- Flush the converter at normal EOF and drain its tail exactly once.
- Discard/reset conversion delay on seek, stop, replacement, or error.
- Apply bounded-segment clipping in the source timeline before conversion so
  frames from the following track are not fed into the resampler.
- Keep main fooyin DSP, ReplayGain settings, and master volume out of this path.
  First-release preview is decoded audio plus format conversion and its own
  stream volume. Decoder-defined source gain remains part of decoding.

The helper `Fooyin::Audio::convert()` is not a sample-rate converter; avoid
mistaking its channel mapping for a complete resampling/downmixing pipeline.

## QAudioSink adapter and pump

- Construct, control, query, and destroy the sink on its owning GUI thread.
- Use `start()` returning the sink's writable `QIODevice` and a short GUI timer
  (initially around 10 ms) to feed ready PCM. No decoding occurs in that timer.
- Set a modest requested sink buffer before start (initial target 100 ms), then
  inspect the actual buffer size; Qt/backend may adjust it.
- Bound plugin-side PCM to an initial target around 500 ms and prebuffer around
  40 ms, clamped to valid frame sizes/capacities. Keep these implementation
  constants until hardware measurements justify exposing tuning settings.
- Use `bytesFree()` and bounded chunks. Handle zero/partial writes and preserve
  unwritten bytes; consume queue data only when safely copied/accepted. Keep a
  pending-write offset so short writes cannot duplicate or skip samples.
- Handle write failures as preview errors. No wait-for-bytes-written or drain
  loop is allowed on the GUI thread.
- Pause uses `suspend()` and resume uses `resume()`. Preserve buffered frames and
  state; do not destroy the session merely to pause it.
- Stop/replacement/seek/error use `reset()` to discard queued audio immediately.
  `stop()` may synchronously drain on some platforms, so it is not the initial
  operation for controls that promise immediate discard.
- The QIODevice pointer becomes invalid after reset/stop/restart; reacquire it
  and clear the old pending-write buffer before feeding a new epoch.
- `IdleState` alone is not EOF: it may mean an underrun. Complete only after
  worker EOF, converter tail, queue, pending write, and sink drain are accounted
  for. New data should recover a temporary underrun without restarting a track.

Map Qt state/error details into plugin-owned types at the adapter boundary.
Avoid making the whole controller depend on a particular Qt-version namespace.

## Position and volume

- Track processed audio from the output adapter (`processedUSecs()` or verified
  equivalent), plus the segment-relative seek base and explicit sink epoch.
- Distinguish frames decoded/written from frames processed; do not display the
  decoder's read-ahead position or use elapsed wall time across pause/underrun.
- Reset counters/base on sink recreation; clamp known segment positions. Treat
  Qt's processed position as an estimate, not hardware-accurate DAC timing.
- Refresh UI position at a modest rate (for example 10 Hz) independently of the
  data pump, and suppress slider feedback loops during user scrubbing.
- Volume uses `QAudioSink::setVolume()` with a documented logarithmic UI mapping.
  Clamp values and persist the user value. Do not touch system device volume or
  fooyin's main volume. Start with a conservative default, e.g. 25% UI volume.

## Tests and acceptance

- [ ] Converter tests check rate changes, mono/stereo mapping, sample amplitudes,
  resampler tail flushing, discontinuities, and bounded memory.
- [ ] Fake sink tests cover short/zero writes, write errors, pause, underrun,
  draining EOF, immediate reset, and stale QIODevice epochs.
- [ ] Device tests cover duplicate descriptions, missing IDs, hotplug during
  Loading/Playing/Paused, default changes, and no reconnect auto-resume.
- [ ] Play real MP3/FLAC/WAV inputs at 44.1/48/96 kHz as supported by available
  decoders and verify correct speed, pitch, level, and channels.
- [ ] Prove concurrent main/headphone playback on separate PipeWire devices,
  including a disconnect test that checks actual routing.
