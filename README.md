# Headphones Preview for fooyin

An external plugin for independent headphone preview while fooyin continues
playing its main output. **Current status: Phase 3 output engine implemented;
physical routing acceptance remains open.** The plugin owns an independent
controller, decoder worker, streaming converter, and explicit-device QAudioSink
output. Track actions, the settings dialog, and the layout panel follow in Phase 4.
See [the implementation plan](.plan/00-overview.md).
Plugin load remains idle. The engine is exercised by automated tests and an
explicit-device audio smoke tool; the complete user-facing workflow is still in
development. See [routing validation](#routing-validation-and-known-limit).

## Preview engine contract

The engine accepts ordinary local mono/stereo files, including seekable CUE and
chapter segments. It rejects remote, virtual/CD, and archive-backed tracks,
nonconcurrent decoders, unsupported channel layouts, malformed PCM, and runtime
format changes. The selected device is tested for concrete Float/Int16/Int32
formats, preferring stereo Float at the source rate, then supported preferred and
44.1/48 kHz candidates, with mono fallback. The worker normalises input through
fooyin's public float conversion helper and uses libswresample for streaming
rate/sample-format conversion and explicit mono duplication/stereo averaging.
Matching native formats can pass through unchanged. Resampler tails are flushed
once at EOF and discarded on replacement, seek, stop, or error. Main DSP,
ReplayGain, main volume, and system device volume are not part of this pipeline.

| Command/event | Behaviour |
| --- | --- |
| `previewTrack(track)` | Replace the previous session and start at logical position zero; the same track also restarts |
| `playPause()` | Pause/resume; while loading/buffering, remember the requested pause; from Stopped/Error, explicitly restart the retained track if output is available |
| `stop()` | Immediately reset output and discard pending/queued audio, cancel work, reset position to zero, retain display/restart metadata |
| `seek(relativeMs)` | Accept only an active seekable preview, clamp to the known duration and safe timeline, reopen at the latest requested target, preserve paused state |
| Natural EOF | Drain only current-session PCM, then stop at position zero; never advance selection or playlists |
| Output change/reported loss | Discard preview; reconnecting or applying output settings never starts playback |

Seeking reports whether the request was accepted; its applied position is
published after worker startup. The public decoder's `seek()` returns no backend
success/failure result, so this API cannot diagnose every decoder-specific seek
failure. To avoid fractional-sample millisecond rounding, the worker seeks to a
preceding frame-aligned millisecond anchor and discards exact preroll frames.
Bounded segment endpoints use source frame coordinates, clipped before
conversion. Whole files end at actual decoder EOF, independently of
approximate metadata duration.

The GUI-thread controller exposes copied state snapshots and has no dependency
on the main player, queue, main volume, or output profiles. Source construction,
file I/O, decoding, seeking, and teardown all run on one dedicated worker. Each
generation owns a frame-aligned 500 ms ring buffer (hard capacity limit 8 MiB),
with a 40 ms startup reserve and cancellation-aware producer backpressure.
Conversion staging is also bounded to 8 MiB. Output pumping runs at 10 ms and
never waits for input; zero/partial writes retain their unwritten bytes. A 100 ms
sink buffer is requested, but the backend may adjust it. Position uses processed
output time (an estimate) and a separate 100 ms snapshot timer, with pause and
sink-epoch accounting. Persistent local-source starvation fails after two seconds
instead of spinning. Sample rates above 768 kHz are rejected.

Stop/replacement/seek invalidate generations immediately, and queued stale
results cannot resurrect a session. Rapid seeks replace the pending worker
request. A published decoder can be aborted safely without racing destruction.
AudioLoader's synchronous open/init has no public cancellation token: ordinary
controls silence output immediately while a delayed open finishes, and shutdown
waits for that work to finish before releasing loader/provider references.
`aboutToQuit`, explicit plugin shutdown, and destruction share idempotent cleanup.

## Explicit device and volume policy

`Headphones/DeviceId` stores the opaque Qt device ID as Base64;
`Headphones/DeviceDescription` is display text only. An empty, malformed, or
unmatched ID is unavailable: there is no default-device fallback. The ID is
resolved at configuration and again immediately before opening the sink.
Unrelated devices/default changes do not replace a valid active session.
Qt-reported disappearance resets output before notifying the controller, and
reconnection only updates readiness. Applying the same ID preserves a session.

`Headphones/Volume` is a normalised UI value in `[0, 1]`, default `0.25`. The
adapter uses Qt's `LogarithmicVolumeScale` to `LinearVolumeScale` conversion before
calling `QAudioSink::setVolume()`. Device and volume are persisted through the
public fooyin SettingsManager; playback state is never persisted. The Phase 4
dialog/widget will expose these settings. For now, development tests can set
them through SettingsManager or in an isolated fooyin configuration while it is
closed; use the smoke tool below to obtain the exact Base64 device ID.

## Routing validation and known limit

Initial deployment expects separately addressable main and headphone devices,
with actual stream routing checked in PipeWire/PulseAudio. Multiple jacks on one
card can be mutually exclusive ports rather than separate devices. Qt IDs and
fooyin backend IDs are different namespaces and do not prove physical separation.

**Observed limitation: Qt 6.8.2's PulseAudio backend did not report removal of a
selected software sink, and PulseAudio 17 moved its stream to the main sink.**
The cached Qt catalogue still advertised the old ID, so neither ID checks nor
the device-change signal could detect that loss in this test. The application
never requested a default device, but the stream nevertheless changed routes.
This tested combination does not satisfy the device-loss isolation requirement.
The corresponding Qt backend removal path omits a device-change notification;
correct cache invalidation and backend/server routing policy (or a public
no-move stream option) are needed before claiming leak-free disconnect handling.
No private Qt/fooyin workaround is used.

Fake-device tests verify stop/reset and no reconnect autoplay when loss is
reported. Physical two-device concurrent main/headphone playback, unplug tests,
and a suitable PipeWire/Qt policy remain required release checks. Software null
sinks and successful unit tests do not establish those hardware guarantees.

## Requirements and compatibility

- C++23 compiler compatible with the matching fooyin build.
- CMake 3.19+ for direct configuration; 3.20+ for the supplied configure/build/test
  presets. Ninja is required by the presets.
- Qt 6.4+ on Linux/macOS, 6.8+ on Windows (following fooyin's baseline): Core,
  Widgets, Multimedia, Sql, Concurrent, and Network. Qt Test is required only
  with `BUILD_TESTING=ON`.
- A matching fooyin SDK built with `INSTALL_HEADERS=ON`, or matching fooyin
  source and built Core/Gui/Utils libraries. Public API baseline: fooyin 0.13.1,
  plugin IID `org.fooyin.fooyin.plugin/2`. The IID is not a binary compatibility
  guarantee across different fooyin, Qt, or compiler builds; rebuild to match.
- FFmpeg 5+ `libswresample` (4+) and `libavutil` (57+) development/runtime libraries.
  These are explicit plugin dependencies, discovered using pkg-config or an SDK
  on `CMAKE_PREFIX_PATH`.

Debian 13 package examples: `build-essential cmake ninja-build pkg-config
qt6-base-dev qt6-multimedia-dev libswresample-dev libavutil-dev`. Building fooyin
itself requires its additional dependencies; see its `BUILD.md`.
Qt Widgets alone is insufficient for audio: deployment must also include Qt
Multimedia's platform audio backend and its runtime dependencies. On Debian 13,
`libqt6multimedia6` supplies Qt Multimedia including its platform audio support.
The tested PulseAudio audio path worked even with the Qt diagnostic that only
QMediaDevices/QAudioSink/etc. were available and no general media backend was
found. Physical backend packaging/routing validation remains required.

## Development-tree build

Build fooyin first using its documented build procedure. The plugin defaults to
`../fooyin` and `../fooyin/build`; it never reconfigures that tree itself.

```sh
cmake --preset dev -DFOOYIN_BUILD_DIR=/path/to/fooyin/build
cmake --build --preset dev
```

The same `-DFOOYIN_SRC_DIR` and `-DFOOYIN_BUILD_DIR` overrides work with **every**
configure preset. Presets deliberately leave those paths to the CMake defaults
so changing `FOOYIN_SRC_DIR` also changes the default build path in a fresh cache.
For persistent local overrides, use `CMakeUserPresets.json`.

| Preset | Plugin directory | Configuration |
| --- | --- | --- |
| `dev` | `build/` | Local Debug |
| `dev-release` | `build-release/` | Local RelWithDebInfo |
| `agent` | `build-agent/` | Container Debug |
| `test-agent` | `build-test-agent/` | Container Debug + tests |

Keep host/container caches and Debug/release caches separate. Reuse a compatible
existing sibling build or create a dedicated one; do not change another build's
compiler/generator. Development discovery exposes only public `include/` headers
and generated export headers, with no private fooyin source include paths.

CMake 3.19 users can configure directly:

```sh
cmake -S . -B build-agent -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DFOOYIN_FORCE_DEV_TREE=ON -DFOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
cmake --build build-agent
```

## Installed-SDK build and installation

Build/install a matching fooyin with `INSTALL_HEADERS=ON`, then select installed
mode explicitly. `FOOYIN_FORCE_DEV_TREE=OFF` requires the SDK and fails if it is
absent; there is no development-tree fallback.

```sh
cmake -S . -B build-sdk -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DFOOYIN_FORCE_DEV_TREE=OFF -DCMAKE_PREFIX_PATH=/path/to/fooyin-sdk \
  -DCMAKE_INSTALL_PREFIX=/path/to/fooyin-sdk
cmake --build build-sdk
cmake --install build-sdk
```

Installed mode uses fooyin's `create_fooyin_plugin(Headphones ...)` and exported
targets/install location. Development mode defaults to
`${CMAKE_INSTALL_LIBDIR}/fooyin/plugins` on Linux, `plugins` on Windows, and
`fooyin.app/Contents/PlugIns` on macOS; override `FOOYIN_PLUGIN_INSTALL_DIR` as
needed. CMake is the portable build/install interface. The output basename is
`fyplugin_headphones` (`.so` on Linux).

## Make workflow (Linux convenience)

```sh
make help
make -j dev FOOYIN_BUILD_DIR=/path/to/fooyin/build
make -j dev-release FOOYIN_BUILD_DIR=/path/to/fooyin/build
make run-debug
```

`dev` builds the selected, already-configured fooyin tree, then configures,
builds, and copies the current plugin. `dev-release` uses `build-release/`
throughout. `install-plugin`/`sync-plugin` also configure and build before copying;
only `fyplugin_headphones.so` is copied, and unrelated plugins are preserved.
Dependencies and recursive Make pipelines keep these operations ordered under
`make -j`. `fooyin-configure` is an explicit optional step that honours an
existing generator; `FOOYIN_CMAKE_ARGS` supplies extra fooyin options.

For container work:

```sh
make check PLUGIN_CONFIG_PRESET=agent FOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
make install-plugin PLUGIN_CONFIG_PRESET=agent FOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
```

Overrides include `PLUGIN_BUILD_DIR`, `PLUGIN_RELEASE_BUILD_DIR`,
`PLUGIN_TEST_BUILD_DIR`, all three configure-preset variables (see `make help`),
`FOOYIN_SRC_DIR`, `FOOYIN_BUILD_DIR`, `FOOYIN_PLUGIN_DIR`, `FOOYIN_BIN`,
`CMAKE_ARGS`, `QT_LOGGING_RULES`, and optional `XDG_CONFIG_HOME`. Make passes the
fooyin path overrides to all CMake configurations, after the presets. For
container release builds, set `PLUGIN_RELEASE_BUILD_DIR=build-release-agent`.

The default copy location is `<fooyin-build>/run/lib/fooyin/plugins`. Override it
for other runtime layouts. Run targets detect `run/fooyin` or `run/bin/fooyin`,
or accept an explicit executable, and fail clearly if none exists. They only set
`XDG_CONFIG_HOME` when a nonempty value is supplied. Close any existing fooyin
instance before plugin-load validation: another instance can receive the launch
request instead of loading the updated plugin.

## Tests and logging

```sh
cmake --preset test-agent -DFOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
cmake --build --preset test-agent
ctest --preset test-agent
# Or: make test FOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
```

Five CTest suites run without audio hardware or an audio server:

- `headphones.plugin-lifecycle` loads the real module using `QPluginLoader`,
  checking metadata/interfaces, partial/duplicate initialisation, retained loader
  lifetime, late callbacks, destructor cleanup, and `aboutToQuit` cleanup.
- `headphones.preview-engine` uses gated fake sources/output for transport,
  delayed-open/read cancellation, full-buffer shutdown, stale results/EOF,
  replacement, exact segment boundaries, fractional-frame seek rounding,
  pause-preserving/coalesced seeks, starvation, malformed formats, partial-frame
  writes, device readiness, errors/recovery, and processed-output positioning.
- `headphones.preview-source` registers a WAV fixture decoder with the real public
  AudioLoader, verifies CUE/chapter samples, and checks worker-thread teardown,
  decoder options, concurrent-decoding rejection, and input/loader ownership.
- `headphones.preview-converter` checks all source sample containers (including
  left-aligned S24In32), integer scaling, channel gains, streaming rate changes,
  signal continuity, one-time tail flushing, reset, and allocation bounds.
- `headphones.preview-output` uses injectable catalogue/sink boundaries for exact
  format negotiation, duplicate descriptions/opaque IDs, missing devices, hotplug
  in every active state, buffer adjustment, reset/device epochs, pause/clock/drain,
  backend recovery, and persistent logarithmic volume/device settings.

An additional executable is built with tests but deliberately **not registered
with CTest**. It enumerates devices or plays a one-second 440 Hz development tone
only on an explicitly supplied ID:

```sh
build-test-agent/tests/previewaudiosmoke --list-devices
build-test-agent/tests/previewaudiosmoke --device-id 'BASE64_ID_FROM_THE_LIST'
```

This tool requires a working audio backend. An unavailable ID fails instead of
opening the default. It can also be used with explicitly configured software
sinks; those checks do not replace listening/routing tests on actual hardware.

The `fy.headphones` category disables debug messages by default. Enable them with
`QT_LOGGING_RULES='fy.headphones.debug=true'` or `make run-debug` (which defaults
to that rule). A load/exit should log one each of `Core initialised`,
`GUI initialised`, and `Shutdown complete`.

For isolated manual validation, set `XDG_CONFIG_HOME`, `XDG_DATA_HOME`,
`XDG_CACHE_HOME`, and `XDG_STATE_HOME` to temporary directories and use fooyin's
`--skip-single` option. `FOOYIN_PLUGIN_PATH` can point to the plugin build
directory for testing without copying it into the sibling build.

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE).

## Verified development setup

Phases 1–3 software was validated on Debian 13 x86_64 with GCC 14.2.0, CMake 3.31.6,
Qt 6.8.2, libswresample 5.3.100, libavutil 59.39.100, and fooyin 0.13.1
(`bb8d9f037`, sibling `build-rebase-agent`). Debug and RelWithDebInfo builds,
all five automated suites (81 Qt Test checks passed), parallel Make workflows,
path overrides, and a real fooyin load/normal exit passed. A temporary host probe
also used fooyin's registered decoder on a generated 44.1 kHz WAV: CUE and chapter
segments matched all expected samples (749 and 574 frames), and shutdown with a
fresh open queued completed normally. Phase 3 then used real QAudioSink streams
on an isolated PulseAudio 17 server. Eight real host-decoder fixtures passed:
MP3/FLAC/WAV at 44.1/48 kHz and FLAC/WAV at 96 kHz, converted to 48 kHz stereo
Float. Each 250 ms file yielded 12,000 output frames; channel-specific spectral
checks verified pitch and amplitude before sink volume. Stream inspection also
confirmed simultaneous streams on two software sinks and exposed the disconnect
rerouting failure documented above. This was not a physical headphone test or
a concurrent main-PlayerController proof. Dependencies were staged outside the
project without system installation.
Installed-SDK delivery, Qt 6.4 runtime, other platforms, and physical PipeWire
main/headphone validation remain open. See the
[Phase 1 evidence](.plan/01-project-and-build.md#completion-evidence-2026-10-18)
[Phase 2 evidence](.plan/02-session-and-decoding.md#completion-evidence-2026-10-18),
and [Phase 3 evidence](.plan/03-audio-output-and-devices.md#verification-evidence-2026-10-18)
for details, including the tested host's restart-based plugin-disable policy.
