# Developing Headphones Preview

See the [README](../README.md) for dependencies, installation, and user-facing
behaviour. Run the commands below from the repository root.

## Build presets

Build a matching fooyin first. All configure presets accept `FOOYIN_SRC_DIR` and
`FOOYIN_BUILD_DIR`; the defaults are `../fooyin` and `../fooyin/build`.

| Preset | Build directory | Configuration |
| --- | --- | --- |
| `dev` | `build/` | Debug |
| `dev-release` | `build-release/` | RelWithDebInfo |
| `agent` | `build-agent/` | Debug, separate cache for container work |
| `test-agent` | `build-test-agent/` | Debug with tests |

```sh
cmake --preset dev -DFOOYIN_BUILD_DIR=/path/to/fooyin/build
cmake --build --preset dev
```

Use `CMakeUserPresets.json` for persistent local overrides. Keep host/container
and Debug/release caches separate. CMake never reconfigures the fooyin tree;
development discovery uses only public headers and generated export headers.
`FOOYIN_FORCE_DEV_TREE=OFF` explicitly requires an installed SDK, without a
development-tree fallback.

For CMake 3.19, configure directly instead of using presets:

```sh
cmake -S . -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DFOOYIN_FORCE_DEV_TREE=ON \
  -DFOOYIN_SRC_DIR=/path/to/fooyin \
  -DFOOYIN_BUILD_DIR=/path/to/fooyin/build
cmake --build build-dev
```

## Make workflow (Linux)

The Makefile provides convenience targets around CMake:

```sh
make help
make -j dev FOOYIN_BUILD_DIR=/path/to/fooyin/build
make -j dev-release FOOYIN_BUILD_DIR=/path/to/fooyin/build
make run-debug FOOYIN_BUILD_DIR=/path/to/fooyin/build
```

`dev` and `dev-release` build the selected, already-configured fooyin tree, then
configure, build, and copy the plugin. `install-plugin`/`sync-plugin` also
configure and build before copying.

The default copy destination is `<fooyin-build>/run/lib/fooyin/plugins`.
Override `FOOYIN_PLUGIN_DIR` for other runtime layouts. Run targets detect
`run/fooyin` or `run/bin/fooyin`, or accept `FOOYIN_BIN=/path/to/fooyin`.
`fooyin-configure` is an explicit optional step that honours an existing generator;
`FOOYIN_CMAKE_ARGS` supplies extra fooyin options.

For separate container caches:

```sh
make check PLUGIN_CONFIG_PRESET=agent FOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
make install-plugin PLUGIN_CONFIG_PRESET=agent FOOYIN_BUILD_DIR=/path/to/fooyin/build-agent
```

Other overrides include `PLUGIN_BUILD_DIR`, `PLUGIN_RELEASE_BUILD_DIR`,
`PLUGIN_TEST_BUILD_DIR`, `PLUGIN_CONFIG_PRESET`, `PLUGIN_CONFIG_PRESET_RELEASE`,
`PLUGIN_TEST_PRESET`, `FOOYIN_SRC_DIR`, `CMAKE_ARGS`, `TEST_ARGS`,
`QT_LOGGING_RULES`, and `XDG_CONFIG_HOME`. Make passes fooyin paths after the
presets. Use `PLUGIN_RELEASE_BUILD_DIR=build-release-agent` for a separate
container release cache.

## Automated tests

Qt Test is required with `BUILD_TESTING=ON`. The CTest suites run without
audio hardware or an audio server:

```sh
cmake --preset test-agent -DFOOYIN_BUILD_DIR=/path/to/fooyin/build
cmake --build --preset test-agent
ctest --preset test-agent
```

Alternatively, run `make test FOOYIN_BUILD_DIR=/path/to/fooyin/build`.

| Suite | Coverage |
| --- | --- |
| `headphones.plugin-lifecycle` | Load the real module; verify interfaces, registration, partial/duplicate initialisation, widget persistence, and shutdown ordering. |
| `headphones.preview-engine` | Transport, cancellation, stale results, replacement, exact segment/seek boundaries, queue bounds, starvation, errors, and output positioning. |
| `headphones.preview-source` | Public AudioLoader integration, CUE/chapter samples, decoder options, concurrent-decoding rejection, ownership, and worker-thread teardown. |
| `headphones.preview-converter` | Sample containers and scaling, channel gains, streaming resampling, continuity, tail flushing, reset, and allocation bounds. |
| `headphones.preview-output` | Format negotiation, opaque device IDs, hotplug, sink epochs, pause/drain clocks, backend recovery, and persisted volume/device settings. |
| `headphones.gui` | Actions and selection targets, settings Apply/Cancel, hotplug, scrubbing, volume, and widget recreation. |
| `headphones.script-variables` | Real script-parser evaluation, metadata fallbacks, state, track/segment indicators, refresh notifications, and concurrent callbacks. |

## Same-process integration validation

The test-only integration driver loads into a matching fooyin process alongside
Headphones Preview. It invokes the plugin's real context-menu, shortcut, widget,
and configuration paths while fooyin's main player runs through an explicit-device
Qt test output backend. It checks main track, playlist, queue, output, and volume
state; continued main output during preview pause; device changes; widget
recreation; and shutdown with preview active.

The driver is built with `BUILD_TESTING=ON` under
`build-test-agent/tests/inprocess/`. It is not installed or run by ordinary CTest.

### Repeatable software-sink validation (Linux)

The Python 3 runner starts a private PulseAudio server with two null sinks,
generates WAV fixtures, and launches fooyin with fresh HOME/XDG directories and
no desktop session bus. It requires `pulseaudio` and `pactl` (Debian packages
`pulseaudio` and `pulseaudio-utils`), plus the matching test-enabled plugin build:

```sh
python3 tests/run-inprocess-validation.py \
  --fooyin /path/to/matching/fooyin \
  --plugin-dir build-test-agent \
  --seconds 300
```

`--seconds 0` runs only the integration workflow; the default is 60 seconds of
repeated replacement, paused seek bursts, resume, volume, and stop.

The runner checks live routing, rejects duplicate/colliding streams, samples
whole-process CPU/RSS, and retains logs, fixtures, and `report.json` in the printed
temporary directory. `--artifacts /path/to/new-directory` selects that location.
The runner stops the fooyin and PulseAudio child processes on success or failure.

For staged dependencies, use `--pulseaudio`, `--pactl`, `--pulse-module-dir`,
`LD_LIBRARY_PATH`, and `QT_PLUGIN_PATH`. `--change-default` adds a server-default
change diagnostic.
Software sinks do not establish audible quality, hardware latency, or physical
disconnect isolation.

### Manual integration-driver launch

Use a fresh isolated fooyin configuration, two explicit Qt output IDs, and three
distinct local WAV fixtures of at least 20 seconds. The main fixture must be
48 kHz stereo Float; preview and other fixtures must be seekable. The driver uses
the default English UI.

```sh
env FOOYIN_PLUGIN_PATH="$PWD/build-test-agent:$PWD/build-test-agent/tests/inprocess" \
  HEADPHONES_INPROCESS_TEST=1 \
  HEADPHONES_TEST_MAIN_ID='BASE64_MAIN_QT_ID' \
  HEADPHONES_TEST_ID='BASE64_HEADPHONE_QT_ID' \
  HEADPHONES_TEST_MAIN_FILE='/path/to/main-20s.wav' \
  HEADPHONES_TEST_PREVIEW_FILE='/path/to/preview-20s.wav' \
  HEADPHONES_TEST_OTHER_FILE='/path/to/other-20s.wav' \
  XDG_CONFIG_HOME='/tmp/headphones-validation/config' \
  XDG_DATA_HOME='/tmp/headphones-validation/data' \
  XDG_CACHE_HOME='/tmp/headphones-validation/cache' \
  XDG_STATE_HOME='/tmp/headphones-validation/state' \
  /path/to/matching/fooyin --skip-single
```

The driver assigns test shortcuts, selects the test main backend, suppresses
first-run Quick Setup, and exits after printing
`HEADPHONES SAME-PROCESS PASS`. The failure timeout is 45 seconds, extended by
`HEADPHONES_TEST_STRESS_SECONDS` when set (0–3600). For stress runs, the main fixture
must be at least that duration plus 20 seconds.

## Logging and debugging

Debug messages in the `fy.headphones` category are disabled by default. Enable
them with `QT_LOGGING_RULES='fy.headphones.debug=true'` or `make run-debug`.
A normal load/exit logs one each of `Core initialised`, `GUI initialised`,
`Preview resources stopped`, and `Shutdown complete`.

For an isolated manual session, set `XDG_CONFIG_HOME`, `XDG_DATA_HOME`,
`XDG_CACHE_HOME`, and `XDG_STATE_HOME` to temporary directories and use fooyin's
`--skip-single`. `FOOYIN_PLUGIN_PATH` can point to the plugin build directory.
Otherwise, close an existing fooyin instance before plugin-load validation so it
does not receive the launch request instead of loading the new module.

`make run-gdb FOOYIN_BUILD_DIR=/path/to/fooyin/build` launches with the same path
and logging overrides, using `--skip-single`.

## Preview engine overview

- The GUI-thread controller exposes copied state snapshots. It has no dependency
  on the main player, queue, main volume, or output profiles.
- Source construction, file I/O, decoding, seeking, and teardown run on one
  dedicated worker using fooyin's public AudioLoader. Decoders must permit
  concurrent decoding. Remote, virtual/CD, archive, unsupported channel layouts,
  malformed PCM, and runtime format changes are rejected.
- Output uses an explicit-device QAudioSink. Negotiation tests concrete Float,
  Int16, and Int32 formats, preferring stereo Float at the source rate, then
  supported preferred and 44.1/48 kHz candidates, with mono fallback.
- Input is normalised with fooyin's public float conversion helper. libswresample
  handles streaming rate/sample-format conversion, with explicit mono duplication
  and stereo averaging. Matching native formats can pass through unchanged.
- Each generation owns a frame-aligned 500 ms ring buffer, capped at 8 MiB, with
  a 40 ms startup reserve and cancellation-aware producer backpressure. Conversion
  staging is also capped at 8 MiB. Rates above 768 kHz are rejected.
- Output pumps every 10 ms without waiting for input; partial writes retain the
  unwritten bytes. The sink requests a 100 ms buffer, subject to backend adjustment.
  Position estimates use processed output time and 100 ms snapshots, accounting
  for pause and sink epochs. Persistent local-source starvation fails after two
  seconds.
- Replacement, stop, and seek immediately invalidate old generations. Stale
  results cannot revive a session; rapid seeks replace the pending request.
  Resampler tails flush once at EOF and are discarded on replacement, seek, stop,
  or error. Natural EOF drains only current-session audio and stops at zero.
- Seeking preserves pause state. The worker seeks to a preceding frame-aligned
  millisecond anchor and discards exact preroll frames. Segment endpoints use
  source-frame coordinates; whole files end at decoder EOF rather than approximate
  metadata duration. The public decoder's void `seek()` cannot report every
  backend-specific seek failure.
- Decoder open/init has no public cancellation token. Controls silence output
  immediately while a delayed open finishes; shutdown waits before releasing
  loader/provider references. `aboutToQuit` stops preview resources while leaving
  widgets alive for host layout saving, followed by final plugin cleanup.

### Persisted settings

`Headphones/DeviceId` stores the opaque Qt ID as Base64;
`Headphones/DeviceDescription` is display text only. Empty, malformed, or unmatched
IDs are unavailable. The ID is resolved during configuration and before sink
creation, with no default-device fallback.

`Headphones/Volume` is a normalised UI value in `[0, 1]`, default `0.25`.
Qt's logarithmic-to-linear conversion is applied before `QAudioSink::setVolume()`.
Settings use fooyin's public SettingsManager; playback state is never persisted.

## Validation baseline

Development validation used Debian 13 x86_64, GCC 14.2.0, CMake 3.31.6, Qt 6.8.2,
libswresample 5.3.100, libavutil 59.39.100, and fooyin 0.13.1. Recorded checks
include Debug/RelWithDebInfo builds, module lifecycle, ASan/UBSan checks, real
fooyin decoder fixtures, and same-process integration validation.

Installed-SDK delivery, the Qt 6.4 runtime, other platforms, and wider physical
device coverage remain unverified. The user-tested Bluetooth setup is described
in the [README](../README.md#bluetooth-disconnect-pauses-the-main-player).
