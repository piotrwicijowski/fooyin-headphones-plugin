# Headphones Preview for fooyin

An external plugin for independent headphone preview while fooyin continues
playing its main output. **Current status: Phase 2 headless preview engine.** The
plugin owns one independent session controller, decoder worker, and bounded PCM
pipeline. Qt audio output/device selection follow in Phase 3; actions, settings,
and the layout panel follow in Phase 4. See [the implementation plan](.plan/00-overview.md).
The installed plugin currently loads idle with no output attached; its engine is
exercised through deterministic fake output and real decoder integration checks.

## Preview engine contract

The engine accepts ordinary local mono/stereo files, including seekable CUE and
chapter segments. It rejects remote, virtual/CD, and archive-backed tracks,
nonconcurrent decoders, unsupported channel layouts, malformed PCM, and runtime
format changes. Phase 2 carries the decoder's native PCM format unchanged;
device format negotiation and streaming conversion/resampling are Phase 3 work.

| Command/event | Behaviour |
| --- | --- |
| `previewTrack(track)` | Replace the previous session and start at logical position zero; the same track also restarts |
| `playPause()` | Pause/resume; while loading/buffering, remember the requested pause; from Stopped/Error, explicitly restart the retained track if output is available |
| `stop()` | Immediately reset output and discard pending/queued audio, cancel work, reset position to zero, retain display/restart metadata |
| `seek(relativeMs)` | Accept only an active seekable preview, clamp to the known duration and safe timeline, reopen at the latest requested target, preserve paused state |
| Natural EOF | Drain only current-session PCM, then stop at position zero; never advance selection or playlists |
| Output change/loss | Discard preview; reconnecting or applying output settings never starts playback |

Seeking reports whether the request was accepted; its applied position is
published after worker startup. The public decoder's `seek()` returns no backend
success/failure result, so this API cannot diagnose every decoder-specific seek
failure. To avoid fractional-sample millisecond rounding, the worker seeks to a
preceding frame-aligned millisecond anchor and discards exact preroll frames.
Bounded segment endpoints use source frame coordinates, clipped before any
future conversion. Whole files end at actual decoder EOF, independently of
approximate metadata duration.

The GUI-thread controller exposes copied state snapshots and has no dependency
on the main player, queue, main volume, or output profiles. Source construction,
file I/O, decoding, seeking, and teardown all run on one dedicated worker. Each
generation owns a frame-aligned 500 ms ring buffer (hard capacity limit 8 MiB),
with a 40 ms startup reserve and cancellation-aware producer backpressure. Output
pumping is bounded and never waits for input; zero/partial writes retain their
unwritten bytes. Persistent local-source starvation fails after two seconds
instead of spinning. Native sample rates above 768 kHz are rejected.

Stop/replacement/seek invalidate generations immediately, and queued stale
results cannot resurrect a session. Rapid seeks replace the pending worker
request. A published decoder can be aborted safely without racing destruction.
AudioLoader's synchronous open/init has no public cancellation token: ordinary
controls silence output immediately while a delayed open finishes, and shutdown
waits for that work to finish before releasing loader/provider references.
`aboutToQuit`, explicit plugin shutdown, and destruction share idempotent cleanup.

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
- FFmpeg `libswresample` and `libavutil` development files and runtime libraries.
  These are explicit plugin dependencies, discovered using pkg-config or an SDK
  on `CMAKE_PREFIX_PATH`.

Debian 13 package examples: `build-essential cmake ninja-build pkg-config
qt6-base-dev qt6-multimedia-dev libswresample-dev libavutil-dev`. Building fooyin
itself requires its additional dependencies; see its `BUILD.md`.
Qt Widgets alone is insufficient for audio: deployment must also include Qt
Multimedia's platform audio backend and its runtime dependencies. On Debian 13,
`libqt6multimedia6` supplies the multimedia backend plugins. Audible output and
backend packaging will be validated when the output phase is implemented.

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

Three CTest suites run without audio hardware or an audio server:

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

Phases 1–2 were validated on Debian 13 x86_64 with GCC 14.2.0, CMake 3.31.6,
Qt 6.8.2, libswresample 5.3.100, libavutil 59.39.100, and fooyin 0.13.1
(`bb8d9f037`, sibling `build-rebase-agent`). Debug and RelWithDebInfo builds,
all three automated suites (53 Qt Test checks passed), parallel Make workflows,
path overrides, and a real fooyin load/normal exit passed. A temporary host probe
also used fooyin's registered decoder on a generated 44.1 kHz WAV: CUE and chapter
segments matched all expected samples (749 and 574 frames), and shutdown with a
fresh open queued completed normally. The container's missing
Qt Multimedia packages were staged outside the project for these checks.
The installed-SDK delivery build, Qt 6.4 runtime, other platforms, and actual
audio/device routing remain later-phase validation. See the
[Phase 1 evidence](.plan/01-project-and-build.md#completion-evidence-2026-10-18)
and [Phase 2 evidence](.plan/02-session-and-decoding.md#completion-evidence-2026-10-18)
for details, including the tested host's restart-based plugin-disable policy.
