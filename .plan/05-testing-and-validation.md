# Phase 5: automated tests and real-device validation

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

- [ ] Default automated tests pass without an audio server.
- [ ] Matching fooyin loads the plugin and the full two-device workflow works.
- [ ] Main-player isolation and old-audio discard have concrete test evidence.
- [ ] Device-loss behaviour and any OS rerouting limits are recorded accurately.
- [ ] Normal shutdown releases preview before decoder-provider teardown.
- [ ] Unavailable hardware/platform checks are recorded as untested, not passed.
