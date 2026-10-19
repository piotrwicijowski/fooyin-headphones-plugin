# Phase 5: automated tests and real-device validation

Status: automated regression consolidation and repeatable software-backend
validation completed on 2026-10-19. Physical release acceptance remains open:
the prior user-reported Bluetooth result lacks exact environment details, and
wider PipeWire/hotplug/listening checks are unavailable in this container.

Tests are implementation deliverables introduced with each feature. This phase
assembles the end-to-end evidence needed to use the plugin during a party.

## Test infrastructure

- Qt Test executables registered with CTest; use `QSignalSpy` and explicit worker
  synchronisation rather than timing-only sleeps.
- Headless tests use `QT_QPA_PLATFORM=offscreen` and require no sound device or
  audio daemon.
- Narrow fake source, output, and device-catalogue adapters supply deterministic
  decode delays/errors, PCM frames, consumption clocks, and device changes.
- Build the real QAudioSink/converter implementation even when hardware tests
  are skipped, so dependency/API errors are caught.
- Separate optional real-output smoke tests from default CTest. Never silently
  pick a developer's default speakers for an automated audio test.
- Fixtures should be short generated PCM/WAV or appropriately licensed files.
  Use distinct signal patterns/channel content to identify stale or misrouted
  audio, rather than asserting merely that some bytes were written.

## Suggested automated suites

Current suites: `headphones.plugin-lifecycle`,
`headphones.preview-engine`, `headphones.preview-source`,
`headphones.preview-converter`, `headphones.preview-output`, and `headphones.gui` are implemented
and registered with CTest. They cover transport, gated cancellation, segment/seek
samples, loader ownership, streaming conversion, fake sink/catalogue policy,
device/volume persistence, UI integration, and shutdown. Device catalogue coverage
is included in the output suite. The primary opt-in integration path is the
included `HeadphonesInProcessDriver` module loaded into the actual matching
fooyin alongside the actual Headphones plugin, exercising main playback and
registered UI/actions in the same process. It passed with Vim Motions loaded.
`previewaudiosmoke` remains only a low-level explicit-device diagnostic, not a
main-player acceptance test. Phase 5 adds sample-checked converted replacement and
coalesced-seek isolation, long-source queue bounds, and a reproducible isolated
same-process stress runner. See the evidence below and Phases 2–4, including the
unresolved real software-backend disconnect assertion from Phase 3.

### `previewcontrollertest.cpp`

- Load, replace, stop, repeated stop, play/pause, restart retained track, error
  recovery, and pause requested before asynchronous startup completes.
- Stale load/seek/EOF/error notifications cannot affect a newer generation.
- Stop immediately resets fake output while delayed open/reads finish later.
- Natural EOF is distinguished from underrun and drains only current audio.
- Device changes/reconnect do not autoplay; main playback is not a prerequisite.

### `previewdecodertest.cpp`

- Correct ownership and teardown of decoder/source bundle on the worker thread.
- Start/stop once, malformed buffers, empty data, NeedMoreInput retry policy,
  decode error, concurrent-decoding rejection, and unsupported source checks.
- Segment offset, clipped final block, adjacent segments in one file, seeking
  while paused, unseekable offsets, and safe arithmetic at invalid extremes.
- Full-buffer cancellation, delayed open, abort/read/destruction ordering,
  rapid A -> B -> C replacement, and shutdown without GUI callback delivery.
- Real AudioLoader adapter smoke test with a tiny local file and a known
  available decoder, independently of opening real audio output.

### `previewconvertertest.cpp`

- Mono/stereo mapping and amplitude preservation across supported sample types,
  including verified S24In32 semantics.
- 44.1 -> 48 kHz and 96 -> 48 kHz frame/time behaviour with tolerances reflecting
  resampler delay, plus same-rate passthrough/conversion.
- Resampler tail at EOF, reset on seek/replacement, and no following-segment
  samples crossing a bounded input endpoint.
- Unsupported formats/layouts fail explicitly; invalid inputs and short blocks
  cannot cause unbounded allocation or out-of-range reads.

### `previewoutputtest.cpp` and `headphonedevicestest.cpp`

- Zero and short writes reconstruct the expected byte stream without duplicates,
  drops, or mismatched frame accounting; write errors reset only preview output.
- Old pending writes cannot reach a recreated sink after seek/replacement.
- Pause/underrun/drain position accounting uses the processed clock, not decode
  progress or time spent suspended.
- ID byte-preserving persistence, duplicate descriptions, no configured device,
  missing device, system default changes, and selected-device loss in every
  active/startup state.
- Test that no unavailable-device path calls an output factory with a null or
  default device selection. Treat OS-level rerouting as a hardware test below.

### `headphonesintegrationtest.cpp`

- Public selection/menu registration, actions, widget recreation, settings
  Apply/Cancel, missing-device display, and no-session-start on load.
- Context-menu track snapshot survives focus/selection changes.
- Through public host dependencies, spy on main-player transport, track, and
  volume/output changes and snapshot queue/playlist state before/after preview.
  Verify absence of mutation/events rather than only checking plugin internals.
- Where full host construction is impractical without private headers, keep
  isolated UI tests public-only and cover remaining host wiring in the manual
  checklist. Do not introduce private dependencies just to make tests easier.
- End-to-end fake-source -> real converter -> fake-output test checks actual
  audio patterns for replacement/seek isolation and bounded memory.

## Build/test commands

From the plugin directory, using a compatible existing sibling build:

```bash
cmake -S . -B build-agent -G Ninja \
  -DFOOYIN_FORCE_DEV_TREE=ON \
  -DFOOYIN_SRC_DIR=/workspace/fooyin \
  -DFOOYIN_BUILD_DIR=/workspace/fooyin/build-agent
cmake --build build-agent
cmake -S . -B build-test-agent -G Ninja \
  -DBUILD_TESTING=ON -DFOOYIN_FORCE_DEV_TREE=ON \
  -DFOOYIN_SRC_DIR=/workspace/fooyin \
  -DFOOYIN_BUILD_DIR=/workspace/fooyin/build-agent
cmake --build build-test-agent
QT_QPA_PLATFORM=offscreen ctest --test-dir build-test-agent --output-on-failure
```

`/workspace/fooyin/build-agent` is an example override; verify it contains a
matching built Core/Gui/Utils before use. Do not overwrite another build cache
to make this example work. Document equivalent presets/Make targets after they
exist. Format touched C++ sources before final checks.

## Linux/PipeWire hardware checklist

Use two independently selectable devices, main **A** and headphones **B**. Pin
fooyin's main output to A explicitly. Inspect stream routing with a suitable
PipeWire graph/monitor tool; listen to or capture both sinks where practical.

| Scenario | Expected result |
| --- | --- |
| Main track X on A, preview Y on B | X continues uninterrupted; only B gets Y |
| Preview play/pause/stop/seek/volume | Main transport, level, queue, and output unchanged |
| Main pause/stop/track change | Preview continues independently |
| Preview with main stopped | B plays normally; main remains stopped |
| Rapid preview A/B/C track replacement | Only latest requested audio survives replacement |
| CUE/chapter near endpoint | Preview stops at logical boundary, not the next track |
| Repeated pause/resume and scrubbing | No stale samples, accidental resume, or counter jumps |
| 44.1/48/96 kHz source changes | Correct pitch/speed/channels and no cross-track tail |
| Change configured headphone device | Preview stops; no automatic restart |
| Unplug B during load/play/pause | Preview stops with status; check whether OS moves it to A |
| Reconnect B | UI recovers availability; explicit play required |
| Change system default | No intentional preview switch; record actual backend routing |
| Device/server error or PipeWire restart | Preview fails cleanly; main state is not changed by plugin |
| Close/reopen preview widget | Session remains consistent; no duplicate stream |
| Quit during load/play/full queue | Clean exit, no lingering worker, deadlock, or crash |
| Restart fooyin | Device/volume retained, preview remains stopped |
| Vim Motions also loaded | Selection and assigned preview shortcuts work as documented |

Verify preview does not create play-count/scrobble/notification events by checking
the relevant host integration in an isolated test configuration. A plugin can
avoid emitting main-player events, but final behaviour should be observed too.

## Performance and portability evidence

- Run an extended simultaneous-playback session with repeated previews and seek
  bursts; record CPU, memory, glitches, startup/stop latency, and actual buffer
  sizes. Memory must remain bounded rather than scaling with track length.
- A useful initial target is responsiveness within roughly 100 ms after data is
  available; measure rather than promising a fixed latency across all devices.
- Run focused sanitiser/thread checks if concurrency failures or lifecycle issues
  appear. Avoid replacing useful content/lifecycle tests with stress alone.
- Linux/PipeWire is the initial hardware validation target. Compile and, when
  available, smoke-test another Qt-supported desktop platform. Report exact
  tested versions/backends; portable code does not itself establish hardware
  support for every platform.

## Acceptance

- [x] Default automated tests pass without an audio server.
- [ ] Matching fooyin loads the plugin and the full two-device workflow works.
  The loaded-plugin software two-sink workflow passes; full physical acceptance
  still needs the hardware evidence listed below.
- [x] Main-player isolation and old-audio discard have concrete test evidence.
- [x] Device-loss behaviour and any OS rerouting limits are recorded accurately.
- [x] Normal shutdown releases preview before decoder-provider teardown.
- [x] Unavailable hardware/platform checks are recorded as untested, not passed.

## Phase 5 verification evidence (2026-10-19)

### Automated regression and reproducibility

- Debian 13 x86_64, GCC 14.2.0, CMake 3.31.6, Qt 6.8.2, libswresample
  5.3.100/libavutil 59.39.100; matching fooyin 0.13.1 `bb8d9f037` in
  `/workspace/fooyin/build-rebase-agent`. Audio tools/Qt Multimedia/SQLite driver
  were staged under `/tmp/opencode`, without system installation.
- `cmake --build --preset test-agent -j 2` and
  `PULSE_SERVER=unix:/tmp/opencode/nonexistent-audio-server ctest --preset
  test-agent -V` passed all six suites. Counts including setup/cleanup:
  lifecycle 10, engine 43, source 6, converter 14, output 13, GUI 8 = **94 checks**.
- New engine cases verify every mono-to-stereo Float output sample after
  replacement with a partial frame/pending old PCM, and after three coalesced
  paused seeks. Both 44.1 -> 48 kHz and 96 -> 48 kHz paths preserve the new ramp,
  channel equality, exact segment frame counts and endpoint/tail isolation.
- One-hour and one-week synthetic source timelines both fill the same
  **192,000-byte (500 ms)** converted queue, limit source read-ahead, and wake the
  blocked producer on shutdown. This is a bounded-memory test without allocating
  long fixtures or relying on process RSS alone.
- The four new data cases also passed the existing AddressSanitizer + UBSan build:
  `ASAN_OPTIONS=detect_leaks=0 build-asan-agent/tests/previewenginetest
  convertedReplacementAndSeekIsolation convertedQueueBoundedForLongSources`.
  Touched C++ files passed `clang-format --dry-run --Werror`; `git diff --check`
  passed.
- `tests/run-inprocess-validation.py` uses Python's standard library, a private
  foreground PulseAudio server with only two null sinks, explicit IDs, generated
  WAV signals, isolated HOME/XDG configuration and no desktop session bus. It
  preserves `fooyin.log`, `pulse.log`, fixtures and `report.json`, samples real
  server routing and whole-host RSS/CPU, and always stops its child processes.
  It is opt-in and outside CTest. See README for the ordinary command; staged
  installations can supply `--pulseaudio`, `--pactl`, `--pulse-module-dir` and the
  required `LD_LIBRARY_PATH`/`QT_PLUGIN_PATH` environment.
- Fresh configurations exposed fooyin's modal Quick Setup intercepting shortcuts.
  The explicitly enabled driver now sets public `Settings::Core::FirstRun` false
  in its isolated configuration. Extended playback also legitimately emits a
  main-track `trackPlayed` event; the test verifies that every such event belongs
  to the main fixture, while preview tracks never enter that accounting path.

### Loaded-plugin extended playback

The runner loaded the real Headphones module and Vim Motions with the matching
fooyin. Main input was stereo Float WAV at 48 kHz; alternating previews were
mono S16 WAV at 44.1 kHz and stereo S16 WAV at 96 kHz. The actual negotiated preview
streams were stereo Float at their source rates; PulseAudio resampled to the
48 kHz null sinks. Forced plugin resampling is covered by the sample tests above.

The `--seconds 300 --vim-plugin-dir /workspace/fooyin-vim-plugin/build-agent` run
finished normally after **306.82 seconds**, including:

- **635 cycles** of alternating preview, pause, three seek requests, volume,
  resume and synchronous UI Stop, with **14,507,480 main frames** submitted.
- **1,818 observations** of concurrent streams on separate explicit sinks;
  duplicate streams or routing collisions fail the runner.
- Unchanged main track, playlist contents, queue, output/volume settings and zero
  main transport/seek/play-state/track/output events during preview-only work.
  One normal main-track `trackPlayed` event occurred; no preview-track event did.
- Main pause and stop at the end did not stop preview; its visible position
  advanced while main was stopped. Fooyin then quit with preview active, returned
  exit code 0, and logged exactly one Headphones `Shutdown complete`.
- Maximum observed command-to-Playing time **193 ms**, sampled by a 50 ms timer.
  This includes worker/backend startup and is not an acoustic latency measurement
  or evidence for a universal 100 ms response guarantee. Stop was checked
  synchronously at the UI; acoustic stop latency/glitches were not measured.
- **73.97 CPU seconds** for the entire host over 306.82 elapsed seconds (about
  24% of one CPU). Whole-process RSS after 10 seconds ranged **32,452–93,912 KiB**;
  a late sample was 47,960 KiB. These include fooyin/Qt/Vim/decoder allocations
  and residency changes, not plugin-only memory; build checks ran concurrently.
  Structural queue/converter allocation bounds support the memory claim.
- Both output paths request 100 ms sink buffers. Initial PulseAudio-reported
  queued buffer latency was 70 ms for the observed stream formats; this is not
  the negotiated capacity or end-to-end latency. Hardware buffer sizes remain
  a deployment measurement.

Artifacts for that run are in
`/tmp/opencode/headphones-validation-pnx349s4/{fooyin.log,pulse.log,report.json}`.
A separate `--seconds 0` run without Vim also passed (17 separate-routing
observations, 91,508 main frames, clean active-preview shutdown).

### Default-device routing diagnostic: failed backend assertion

The optional `--change-default` run changes only the private server's default
from main to headphones after observing separate concurrent streams. On Qt 6.8.2
and PulseAudio 17.0, the existing **main test stream moved from sink 0 to sink 1**;
the preview remained on sink 1. Both streams still had the same original
`media.name` identifiers. The runner failed with exit 1 and preserved the stream
records in `/tmp/opencode/headphones-validation-kme4_v19/report.json`.

This is a routing-policy failure of this tested main-QAudioSink/server setup,
not evidence that preview commands changed main output settings. Changing default
is therefore a separate diagnostic rather than part of the passing fixed-device
stress scenario. It is distinct from the previously recorded selected-headphone
removal/rerouting failure. Neither result is labelled passed or worked around with
private APIs; explicit IDs alone do not provide physical routing guarantees.

### Remaining physical/platform acceptance

- Record exact fooyin/Qt/PipeWire/WirePlumber versions, device models and routing
  for the prior user-confirmed main A/Bluetooth B setup and policy change.
- Exercise the Linux/PipeWire checklist above with listening/capture on physical
  A/B: unplug during load/play/pause, reconnect/no autoplay, default changes,
  server restart, main track changes, segment endpoints and audible stale samples.
- Run a party-length physical session; record dropouts, acoustic startup/stop
  latency and negotiated hardware buffer sizes. Five minutes of software-sink
  stress does not establish those measurements.
- Observe actual notification/scrobble integrations when enabled. The automated
  test proves host event isolation, but does not load external scrobble services.
- Qt 6.4 runtime and another desktop platform are untested. Installed-SDK build,
  installation and delivery remain Phase 6 work.
