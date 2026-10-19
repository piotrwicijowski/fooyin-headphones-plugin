# Phase 2: preview session, decoding, and ownership

Depends on: [01-project-and-build.md](01-project-and-build.md).

## Component boundaries

| Component | Responsibility |
| --- | --- |
| `HeadphonesPlugin` | Host interfaces, dependency wiring, actions/widget registration, early shutdown |
| `PreviewController` | Single preview session, commands, state, generations, errors, UI snapshots |
| `PreviewSource` | Small adapter around AudioLoader/AudioDecoder, replaceable with a fake |
| `PreviewDecoderWorker` | Source ownership, decode/seek, conversion, bounded production |
| `PreviewPcmBuffer` | Bounded frame-aligned PCM queue with cancellation/backpressure |
| `PreviewOutput` | Minimal output/clock interface, implemented by QAudioSink in phase 3 |

The controller exposes `previewTrack(track)`, `playPause()`, `stop()`,
`seek(relativeMs)`, and `setVolume(...)`, plus immutable state snapshots for UI.
Widgets never own decoders or call the main player transport.

## Session/state model

Use a small explicit state machine:

`Stopped -> Loading -> Buffering -> Playing <-> Paused -> Draining -> Stopped`.

`Error` is a recoverable terminal state with a message; a new explicit preview
request can leave it. Device readiness is separate from transport state.

Define these behaviours before implementation:

- `previewTrack`: replace any old session and start the supplied track at zero
  relative to its logical segment. Re-previewing the same track restarts it.
- `playPause`: pause/resume the loaded preview. From Stopped/Error, restart the
  retained last preview track only if one exists and the device is available.
- `stop`: immediately discard output/queued audio, reset position to zero,
  retain track metadata for display/restart, and cancel pending work.
- Natural EOF: drain only this session's remaining PCM, then stop; never
  automatically preview another selection or advance a playlist.
- Pause while loading/buffering: remember requested paused state so asynchronous
  completion cannot unexpectedly start playback.
- Seek while paused: remain paused. Reject seeking when the decoder says it is
  unsupported; avoid pretending it succeeded.
- Changing the configured device stops the preview; it does not resume on the
  new device automatically.

Every replacement, stop, seek, output reconfiguration, and shutdown invalidates
the previous generation. Tag worker results, buffered data, EOF, and errors so
stale events cannot change current state. Coalesce rapid seeks to the latest
target. Distinguish the retained display track from a live decoding session.

## Threading and data flow

Chosen baseline design:

- GUI thread: controller, device enumeration, QAudioSink adapter, output pump,
  settings, and widgets.
- One dedicated decoder worker thread: construct/use/destroy the source and
  decoder, perform file I/O, seek, sample conversion, and resampling.
- A bounded PCM queue between them, with short metadata/copy critical sections
  and worker-side condition-variable backpressure. No custom realtime callback
  is required by the chosen QAudioSink push interface.

The output pump must not wait for data or hold a queue lock during a sink write.
The producer must release locks before decoder/converter calls. A stopped or
full-queue worker must wake on cancellation. Ensure no queue reset races with a
producer still writing an old generation; use generation-owned queues or an
explicit acknowledgement before reuse.

Suggested startup handshake:

1. Validate track and selected device; create a new generation.
2. Worker opens the decoder and reports its input format/capabilities.
3. GUI chooses a supported output format on the selected device.
4. Worker receives the output format, configures conversion, and fills a bounded
   prebuffer; GUI then starts/pumps output respecting the requested pause state.

Use queued, receiver-lifetime-safe Qt notifications. Do not capture raw widget
pointers in worker completions. Shutdown joins the worker without depending on
delivery of a GUI-thread callback.

## AudioLoader/AudioDecoder use

- Obtain and retain `CorePluginContext::audioLoader` in core initialisation.
- Reject invalid, remote, virtual, and archive-backed sources before opening in
  v1. This excludes CD/custom-device sources and keeps the first release local.
- Open with `loadDecoderForTrack()` on the worker, with looping disabled and
  seeking allowed. Do not request `NoSeeking` or conversion/offline policies.
- Check both `LoadedDecoder::decoder` and `format`; retain the **whole bundle**
  for the session, not just the decoder. Honour its `isDecoding` state.
- Keep source/decoder construction and destruction in their owning worker
  thread. When transferring/moving input bundles, honour `LoadedSource::rebind()`
  and preserve source lifetime through decoder teardown.
- Check `allowsConcurrentDecoding()` and reject a decoder that opts out. The v1
  source restrictions also avoid opening shared physical-media sources before
  this post-initialisation check is available.
- Start once, honour seekability, and use `readAudio()` statuses: data,
  NeedMoreInput, EOF, and error. No busy-loop on empty/temporarily unavailable
  input; bounded waits must wake on cancel. Give persistent starvation a clear
  local-source error rather than retrying forever.
- Reject invalid formats, malformed frame data, or unexpected runtime format
  changes with a controlled preview error in v1.
- Always call `stop()` and release resources on the worker, including partial
  startup failure paths.

### Cancellation limitation to preserve in implementation

`AudioDecoder::requestAbort()` can interrupt an already accessible decoder.
However, `AudioLoader::loadDecoderForTrack()` is synchronous and does not expose
the candidate decoder or a cancellation token during its open/init call.

Consequently, Stop must silence output immediately and invalidate the request,
even if a local-file open is still finishing on the worker. Discard its result
when it returns. Do not promise immediate cancellation of loader initialisation,
use thread termination, or detach a thread that still references plugin code.
Exercise delayed-open tests and assess shutdown latency with local sources.

For published decoders, synchronise cross-thread abort access with ownership so
`requestAbort()` cannot race destruction. If abort poisons a decoder's future
reads, reopen it for the next request instead of reusing it blindly.

## Logical-track positioning

- UI time starts at zero for each logical track.
- Translate preview seek positions to `track.offset() + relativePositionMs` only
  where the selected decoder/source expects absolute file time; verify against
  fooyin's existing segment handling with real CUE/chapter fixtures.
- For `isBoundedSegment()`, clip decoded frames at its logical duration, including
  a block straddling the boundary. Do not preview the next song in the same file.
- For an ordinary whole-file track, use actual decoder EOF instead of truncating
  solely at possibly approximate metadata duration.
- Reject nonzero segment offsets on unseekable sources in v1 rather than playing
  the wrong part of the file. Clamp seek arithmetic and prevent overflow.
- Flush decoder-side pending PCM, resampler state, queue, and sink on seek. The
  new position epoch must not count old audio or leak old EOF notifications.

## Lifecycle

Provide one idempotent shutdown path: stop accepting commands, invalidate
generations, reset/destroy sink on its owning thread, wake/abort worker, join it,
release decoder/source resources, then release loader references.

Hook `QCoreApplication::aboutToQuit` early, and invoke the same cleanup from
`Plugin::shutdown()`/destruction as a fallback. Inspect actual host exit ordering:
the current plugin manager unloads providers in unspecified map order, so do not
assume a decoder plugin is always shut down after this plugin. Validate normal
quit, partial startup, and any host-supported disable/unload flow. If the host
allows unloading decoder providers while clients remain active, record the need
for a public lifetime contract rather than relying on private plugin-manager APIs.

## Tests and acceptance

- [x] Fake-source/output tests cover load, pause/resume, stop, replacement, error,
  delayed open, stale completion, and cancellation while the PCM queue is full.
- [x] Segment tests cover offsets, boundary-straddling blocks, seek clamping, and
  unseekable input.
- [x] Loader bundle survives until decoder stop; teardown occurs once on the
  owning worker thread.
- [x] Slow decoder operations do not prevent immediate output reset or GUI events.
- [x] Main transport/queue APIs are absent from the preview controller's dependency
  surface; main playback is not required for preview to operate.

## Implementation decisions

- `HeadphonesPreviewEngine` is a position-independent static implementation
  library shared by the plugin and its tests. `HeadphonesPlugin` owns exactly one
  controller and retains the core context's public AudioLoader through the
  source factory. No output is attached until Phase 3, so plugin load remains idle.
- One dedicated `std::thread` owns all source/decoder operations. A replaceable
  pending request coalesces rapid controls; each generation owns its cancellation
  state, configuration handshake, and PCM queue. Worker notifications are queued
  to the lifetime-bound controller; stale open notifications hold weak session
  references, so they do not retain abandoned PCM allocations.
- The native-format ring holds 500 ms of audio, with an 8 MiB hard limit and 40 ms
  startup reserve. Only producer backpressure waits. Controller pumping copies
  outside sink calls, preserves partial-frame writes, and limits work per tick.
  Invalid mono/stereo layouts and rates above 768 kHz fail explicitly.
- Seek reopens the source rather than reusing an aborted decoder. This also
  discards pending decoder PCM and provides a clean seam for converter reset in
  Phase 3. Native PCM is transported unchanged in this phase; negotiated output
  and streaming conversion are introduced together in Phase 3.
- Direct fractional-sample millisecond seeks can round differently from PCM
  clipping: the actual host FFmpeg decoder returned frame 1191 for 27 ms at
  44.1 kHz, while floor-based frame coordinates expect 1190. The worker now seeks
  to `absoluteMs - absoluteMs % (1000 / gcd(rate, 1000))` and discards exact frames
  to the requested coordinate. This requires less than one second of worker-side
  preroll and fixes the real fixture without private headers or core changes.
  Bounded lengths subtract absolute frame coordinates, avoiding per-block or
  segment-offset rounding drift.
- NeedMoreInput retries use cancellation-aware 10 ms waits and a two-second
  persistent-starvation deadline. Failed/malformed/changing input formats become
  recoverable errors. Whole-file EOF ignores approximate duration metadata.
- A session abort mutex protects only source publication/lifetime and prompt
  `requestAbort()`, never open/read/seek. Source stop/destruction and complete
  LoadedDecoder bundle release happen on the worker before its factory releases
  AudioLoader. Loader open itself remains non-interruptible through the public
  interface; shutdown joins safely rather than detaching or terminating it.

## Completion evidence (2026-10-18)

- Baseline: Debian 13 x86_64, GCC 14.2.0, Qt 6.8.2, CMake 3.31.6, fooyin
  0.13.1 at `bb8d9f037`, existing sibling `build-rebase-agent`. The same staged
  Qt Multimedia dependency prefix documented in Phase 1 was used.
- Debug plugin/test builds and the RelWithDebInfo `build-release-agent` build
  passed. `make -j4 check PLUGIN_CONFIG_PRESET=agent
  FOOYIN_BUILD_DIR=/workspace/fooyin/build-rebase-agent
  CMAKE_ARGS=-DQt6Multimedia_DIR=/tmp/opencode/headphones-deps/root/usr/lib/x86_64-linux-gnu/cmake/Qt6Multimedia
  TEST_ARGS=-V` passed all 3 CTest suites: plugin lifecycle 9, engine 38, and
  public-source adapter 6 Qt Test checks (53 total, including suite setup/cleanup).
- Gated fake tests prove immediate output reset and GUI heartbeat delivery while
  open remains blocked; cross-thread read abort; stale queued errors and old EOF;
  latest A/B/C replacement; full-queue cancellation; and shutdown without GUI
  callback delivery. Ownership counters verify stop/destruction once per opened
  source on the owning thread. Distinct sample patterns verify reset/replacement
  and seek isolation rather than only counting writes.
- Public AudioLoader integration registers a WAV fixture decoder and verifies
  actual CUE/chapter samples, NoLooping with seeking enabled, nonconcurrent and
  invalid-format rejection, live input at decoder stop/destruction, device
  teardown, and release of the loader only after worker shutdown.
- A temporary public-API probe outside the repository ran inside the matching
  fooyin process with isolated HOME/XDG paths, offscreen Qt, and fake output. It
  decoded generated 44.1 kHz S16 mono WAV segments through the host's registered
  decoder. After the anchor fix, every sample matched: CUE offset 10 ms/duration
  17 ms yielded 749 frames (21 ms startup); chapter offset 27 ms/duration 13 ms
  yielded 574 frames (11 ms startup). These are single-run observations, not
  general latency guarantees. Exit with a new open queued returned status 0;
  joined worker shutdown measured 0 ms at millisecond timer resolution.
- Host exit ordering was inspected: plugin constructor connections to
  `aboutToQuit` are made during startup, before `main.cpp` registers GUI/core
  shutdown and provider unloading. The active host probe and plugin lifecycle
  test exercise that early cleanup path. In this host version, disabling plugins
  persists changes and offers an application restart (`pluginspage.cpp`);
  `unloadPlugins()` is used only by core application shutdown. No supported live
  decoder-provider unload path was found. A future live-unload feature would
  require an explicit public provider-lifetime contract.
- `clang-format --dry-run --Werror` and `git diff --check` passed. No private
  fooyin headers or main transport/queue dependencies were introduced. Audible
  output, rate conversion, actual routing/device loss, installed-SDK delivery,
  and broader decoder-format fixtures remain their respective later phases.
