# Phase 3: QAudioSink, format conversion, and device policy

Depends on: [02-session-and-decoding.md](02-session-and-decoding.md).

Status: software implemented and automated/software-output checks passed.
The user has also confirmed successful physical main-A/Bluetooth-preview-B
disconnect behavior after disabling WirePlumber's `linking.pause-playback` policy.
Exact environment versions and the broader hardware matrix remain Phase 5 work;
the separately tested Qt 6.8.2/PulseAudio failure remains documented below.

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

- [x] Converter tests check rate changes, mono/stereo mapping, sample amplitudes,
  resampler tail flushing, discontinuities, and bounded memory.
- [x] Fake sink tests cover short/zero writes, write errors, pause, underrun,
  draining EOF, immediate reset, and stale QIODevice epochs.
- [x] Device tests cover duplicate descriptions, missing IDs, hotplug during
  Loading/Playing/Paused, default changes, and no reconnect auto-resume.
- [x] Play real MP3/FLAC/WAV inputs at 44.1/48/96 kHz as supported by available
  decoders and verify frame duration, pitch, level, and channels in output PCM
  submitted to a real software-backed QAudioSink. Physical listening is pending.
- [x] Prove concurrent main/headphone playback on separate PipeWire devices,
  including a disconnect test that checks actual routing (user-reported physical
  main A/Bluetooth B setup; WirePlumber pause policy disabled, details below).

## Implementation notes

- `HeadphoneDevices` is the Qt implementation of an injectable QObject catalogue.
  Device IDs are byte-preserving Base64 settings; descriptions are never lookup
  keys. `QtAudioOutput` resolves the selected ID twice, negotiates only concrete
  supported Float/S16/S32 mono/stereo formats, and refuses a null/mismatched Qt
  device in its real sink factory. It never uses the default-device constructor.
- Worker startup now reports input capabilities, waits for the GUI-negotiated
  output format, then constructs the streaming converter and output-format PCM
  queue. The GUI polls readiness without waiting for conversion or decoding.
- `PreviewConverter` uses public float normalisation and explicitly linked FFmpeg
  5+ swresample/avutil APIs. Mono duplication uses unity gain in each output;
  stereo downmix uses `[0.5, 0.5]`. Native matching formats pass through unchanged.
  Input blocks are limited to 4096 frames, conversion staging to 8 MiB, and EOF
  tail draining is bounded/idempotent. Segment clipping/preroll happens before
  conversion; replacing/seeking destroys all old converter state.
- `PreviewAudioSink` is the narrow injectable boundary behind the actual explicit
  QAudioSink. Reset invalidates the QIODevice pointer before backend calls.
  Sink state/error enums stay within the adapter (QAudio before Qt 6.7, QtAudio
  thereafter). Underrun is recoverable; EOF additionally requires worker/tail,
  queue, pending-write, and actual sink Idle completion. Position uses processed
  output time, frozen during pause, capped by submitted audio, and reset per epoch.
- Pumping remains 10 ms; position snapshots use a separate 100 ms timer. The
  requested sink buffer is 100 ms, the queue 500 ms and startup reserve 40 ms.
  Backend-adjusted buffer size is queryable and is tested.
- `HeadphonesSettings` registers device ID, description, and normalised UI volume
  with public SettingsManager. Defaults are empty device/25% UI volume. The plugin
  wires selection changes to stopped output replacement, subscribes volume in
  both directions, and creates device enumeration/output on GUI initialisation.
  Same-ID updates do not interrupt preview. No startup/reconnect/settings apply
  starts audio. The actual dialog/widget remains Phase 4.
- Volume uses Qt's logarithmic-to-linear conversion before QAudioSink stream gain;
  neither main/system volume nor main DSP/ReplayGain is changed.

## Verification evidence (2026-10-18)

- Matching baseline: fooyin `bb8d9f037`/0.13.1, Debian 13 x86_64, GCC 14.2.0,
  Qt 6.8.2, FFmpeg swresample 5.3.100/avutil 59.39.100. Existing sibling
  `build-rebase-agent` and the staged Multimedia prefix from Phase 1 were reused.
- `make -j4 check PLUGIN_CONFIG_PRESET=agent
  FOOYIN_BUILD_DIR=/workspace/fooyin/build-rebase-agent
  CMAKE_ARGS=-DQt6Multimedia_DIR=/tmp/opencode/headphones-deps/root/usr/lib/x86_64-linux-gnu/cmake/Qt6Multimedia
  TEST_ARGS=-V` passed all five CTest suites (9 lifecycle + 39 engine + 6 source
  + 14 converter + 13 output = 81 Qt Test checks, including suite setup/cleanup).
  Tests pass without an audio server. Debug and RelWithDebInfo plugin builds passed.
- New converter tests verify U8/S16/S24In32/S32/F32/F64 amplitudes, stereo/mono
  gains, integer scaling, 44.1↔48 and 96→48 kHz frequency/continuity, output frame
  counts, EOF tail once, reset without old tail, and bounded invalid/extreme input.
  Engine integration verifies converted segment duration and post-seek samples.
- Sink/catalogue tests verify explicit IDs with duplicate descriptions, no factory
  call for missing IDs, re-resolution after configuration, no real factory for a
  null Qt device, unrelated/default-order changes, reported device loss during
  Loading/Buffering/Playing/Paused/Draining, and no reconnect autoplay. Tests also
  exercise partial-frame/zero writes, QIODevice destruction/reset epochs, backend
  failure/recovery, requested/actual buffer sizes, paused processed clocks, underrun
  versus EOF, opaque-ID persistence, and logarithmic UI volume.
- No `/dev/snd`, PipeWire daemon, or physical output devices are available in this
  container. PulseAudio 17/FFmpeg tools and their missing dependencies were
  extracted under `/tmp/opencode/headphones-audio-deps`, with an isolated server
  socket/config under `/tmp/opencode/headphones-pulse`; no system installation.
  The server had two named software sinks, `main` and `headphones`.
- A temporary public-API probe inside matching fooyin used its registered decoder
  and this engine's actual QAudioSink adapter. A test catalogue constrained the
  real selected device to supported 48 kHz stereo Float, forcing application SRC
  for non-48 kHz sources. Generated 250 ms 440 Hz left/880 Hz right fixtures passed:
  MP3/FLAC/WAV at 44.1/48 kHz, FLAC/WAV at 96 kHz (MP3 does not support 96 kHz).
  All eight produced exactly 12,000 output frames; per-channel spectral amplitude
  checks before sink volume matched 0.2/0.1 within 10% (including lossy MP3).
  Actual sink buffer was 26,880 bytes (70 ms), adjusted from the 100 ms request.
  Elapsed startup+play+drain was 943–2213 ms in the isolated null backend; these
  numbers do not establish interactive hardware latency. Host exited normally.
- A second test ran two explicit Qt streams simultaneously. Server stream
  inspection confirmed separate software sink IDs 0 and 1. Removing the selected
  headphone sink exposed the failure below. This is **not** proof of physical
  output isolation or coexistence with fooyin's main PlayerController.
- Formatting and whitespace checks passed. Installed SDK, Qt 6.4 runtime,
  other platforms, physical listening, and actual PipeWire main/headphone
  coexistence/disconnect validation remain open.

### Observed Qt 6.8.2/PulseAudio 17 disconnect failure

The server moved the headphone stream to the main sink after the software sink
was removed. The headphone process kept playing to natural EOF instead of
receiving a missing-device error. Its Qt catalogue still advertised the old ID;
the server-side move was observed in `pactl` stream routing. The other stream
completed normally. The routing test's expected stop assertion therefore failed.

Qt 6.8.2's public API exposes no explicit stream no-move policy or catalogue
refresh/invalidation operation. Inspection of the Qt backend's
[`QPulseAudioEngine::eventCallback`](https://github.com/qt/qtmultimedia/blob/v6.8.2/src/multimedia/pulseaudio/qaudioengine_pulse.cpp)
shows sink removal updates its map without emitting `audioOutputsChanged`,
leaving the platform cache stale. A suitable Qt/backend notification fix and
server policy (or a public QAudioSink no-move contract) must be validated before
claiming leak-free device loss. No private Qt header or platform-specific routing
dependency has been added to conceal the issue. This tested backend combination
does not satisfy the first-release physical-isolation acceptance criterion.

### User-reported Bluetooth disconnect result (environment details pending)

A user tested actual main playback on output A while preview played through a
Bluetooth headset B. Disconnecting B made the headphone output unavailable and
preview did not transfer to another device. However, main playback on A paused.
The user subsequently confirmed that disabling WirePlumber's
`linking.pause-playback` policy resolved the unwanted main pause and that the
setup works. This provides user-reported physical-device evidence for concurrent
playback, loss detection/no reroute, and continued main playback under that policy.
That result does not invalidate the separately observed Qt 6.8.2/PulseAudio
null-sink failure above;
the user's Qt/backend/desktop versions have not yet been identified.

The user subsequently confirmed that the pause occurs only with preview playing
and captured an external session-bus call from `:1.6` to
`org.mpris.MediaPlayer2.fooyin`, path `/org/mpris/MediaPlayer2`, interface
`org.mpris.MediaPlayer2.Player`, member `Pause`. This establishes an external main
MPRIS pause request, not a preview-to-main transport call. The policy was confirmed
by the user's successful retest; the bus name itself is session-specific.

WirePlumber has an exact matching policy: `linking.pause-playback` (default true)
pauses MPRIS media players whose audio streams were linked to a removed sink.
Its upstream `linking/mpris-pause.lua` matches clients to players by process/PID,
so an active preview stream in the fooyin process is associated with fooyin's
main MPRIS player even while main audio is routed elsewhere. The hook exposes a
global setting, not a per-stream exemption; changing the preview stream's name
would not change this PID match.

The successful user-side remedy was disabling `linking.pause-playback`. The
documented runtime command is `wpctl settings linking.pause-playback false`;
the setting can be queried with `wpctl settings linking.pause-playback` and saved
with `wpctl settings --save linking.pause-playback false`. This changes the
session-wide unplug auto-pause policy for all players, not just fooyin. Recheck
preview stop/no-reroute as well as uninterrupted main playback after changing
policy; disabling MPRIS pausing does not itself enforce routing isolation. No
automatic main-player resume or MPRIS pause suppression has been added to
application code. Whether the user saved the setting, and the exact
headset/desktop/Qt/PipeWire/WirePlumber versions, were not reported.

References:
- [WirePlumber well-known settings](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html)
- [WirePlumber MPRIS pause hook](https://github.com/PipeWire/wireplumber/blob/master/src/scripts/linking/mpris-pause.lua)
