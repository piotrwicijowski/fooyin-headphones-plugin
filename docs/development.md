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

Release-helper tests use only Python's standard library:

```sh
python3 -m unittest discover -s .github/scripts -p 'test_*.py' -v
```

| Suite | Coverage |
| --- | --- |
| `headphones.plugin-lifecycle` | Load the real module; verify interfaces, registration, partial/duplicate initialisation, widget persistence, and shutdown ordering. |
| `headphones.preview-engine` | Transport, cancellation, stale results, replacement, exact segment/seek boundaries, queue bounds, starvation, errors, and output positioning. |
| `headphones.preview-source` | Public AudioLoader integration, CUE/chapter samples, decoder options, concurrent-decoding rejection, ownership, and worker-thread teardown. |
| `headphones.preview-converter` | Sample containers and scaling, mono/stereo baselines, per-channel impulses for native/permuted multichannel layouts, headroom/LFE policy, mono fallback, integer outputs, streaming resampling, tails, and allocation bounds. |
| `headphones.preview-replaygain` | Shared configuration, all processing/selection modes, gain/preamp/peak PCM, invalid metadata, session snapshots, muted playback, replacement, and resampled tails. Optional real native-Opus header-gain check. |
| `headphones.preview-output` | Format negotiation, opaque device IDs, hotplug, sink epochs, pause/drain clocks, backend recovery, and persisted volume/device settings. |
| `headphones.gui` | Actions and selection targets, volume-step bounds and shared mute restoration, settings/dialog-provider lifetime, hotplug, scrubbing, and widget recreation. |
| `headphones.script-variables` | Real script-parser evaluation, metadata fallbacks, state, track/segment indicators, refresh notifications, and concurrent callbacks. |

To include the real native-Opus check, supply a matching fooyin Opus input plugin
and a local Opus fixture with nonzero audio in its first 50 ms:

```sh
HEADPHONES_TEST_OPUS_PLUGIN=/path/to/fooyin/plugins/fyplugin_opus.so \
HEADPHONES_TEST_OPUS_FILE=/path/to/audio.opus \
ctest --preset test-agent -R preview-replaygain
```

The check generates temporary zero/+6 dB header-gain variants, repairs their Ogg
CRCs, and verifies decoder/header gain with every processing and gain-selection
mode. It uses the public `InputPlugin` interface and is skipped if these variables
are absent. The plugin, fixture, and dependencies are test-only inputs.

## Version-tag builds

`.github/workflows/build-tag.yml` builds Linux x86-64 Release artifacts on
`ubuntu-24.04`. It builds the pinned upstream fooyin Core/Gui/Utils libraries,
builds the plugin and its tests, runs CTest offscreen, installs into a temporary
prefix, and checks the installed ELF dependencies and library search paths. The
packaging helper then loads the plugin extracted from the archive with the
existing lifecycle test. Successful builds create a GitHub Release for the tag
with a `.tar.gz` and its SHA-256 checksum attached. Prerelease tags such as
`v0.2.0-rc.1` create prereleases. Failed runs upload available CMake/CTest
diagnostics as workflow artifacts for 14 days.

Publishing uses GitHub CLI (`gh`) and the workflow's `GITHUB_TOKEN` with
`contents: write` permission. New releases receive automatically generated release
notes. Reruns replace assets with matching filenames on an existing release.

The workflow passes the relative plugin install directory as a CMake `STRING`.
This preserves staging with `cmake --install --prefix`; an untyped command-line
override of the project's `PATH` cache entry would become an absolute path.

### Create a version tag

1. Set `project(FooyinHeadphones VERSION ...)` in `CMakeLists.txt` to the intended
   numeric version and commit the release changes, including the workflow.
2. Use `vMAJOR.MINOR.PATCH` with optional SemVer prerelease/build suffixes. A tag
   such as `v0.2.0-rc.1` requires CMake version `0.2.0`. The full tag identifies
   the artifact; the embedded plugin metadata carries the numeric CMake version.
3. Validate locally, then create and push the tag. For the current version:

   ```sh
   python3 .github/scripts/release.py validate --source . --ref refs/tags/v0.1.0
   git tag -a v0.1.0 -m "Headphones Preview 0.1.0"
   git push origin v0.1.0
   ```

4. Open **Actions → Build version tag** and check the test result and release
   link in the run summary. Download and install the archive into a matching
   fooyin environment as described in the [README](../README.md#download-a-release).

The push filter accepts tags beginning with `v` and a digit. Strict validation
rejects malformed SemVer, leading-zero numeric identifiers, and CMake version
mismatches before checking out/building fooyin. Branch pushes do not run this
workflow.

### Rerun a tagged build

Use **Re-run jobs** on its Actions run, or dispatch against an existing tag:

```sh
gh workflow run build-tag.yml --ref v0.1.0
```

Manual dispatch requires the workflow to exist on the default branch and at the
selected tag. Dispatching a branch is rejected by version validation. Rerunning
rebuilds and uploads the release files again. Runner images and apt package
updates mean the new binary may differ; consult its build metadata.

### Update the fooyin baseline

`FOOYIN_REF` in the workflow pins the full upstream commit SHA. The current
revision is `bb8d9f0371b254571fffec310dce3ee1ce005c87`; its CMake version is
`0.13.1`, so the SHA is the precise compatibility identifier. Build both projects
with the same compiler, Qt, and FFmpeg installation. When changing this pin,
verify a clean build, all CTest suites, and the packaged lifecycle check, then
update the compatibility information here. The archive records actual
versions of the compiler, CMake, Qt, FFmpeg libraries, and other principal
dependencies in `build-metadata.json`.

## Same-process integration validation

The test-only integration driver loads into a matching fooyin process alongside
Headphones Preview. It invokes the plugin's real context-menu, shortcut, widget,
and configuration paths while fooyin's main player runs through an explicit-device
Qt test output backend. It checks main track, playlist, queue, output, and volume
state; continued main output during preview pause; device changes; widget
recreation; and shutdown with preview active. Keyboard navigation checks matching
selection and the retained entry's visible rectangle after reorder, while paused
and after Stop, then exercises segment-aware target resolution, cancellation,
entry/playlist removal, and a missing target. Main playback, active playback
playlist, queue, and paused preview position must remain unchanged.

Preview-track navigation uses the existing public
`CurrentPlaylistController::changeCurrentPlaylist()`/`selectTracks()` and
`PlaylistHandler::ensurePlaylistItemVisible()` APIs. A bounded, cancellable retry
handles deferred playlist-view loading, re-resolving entry identity on each try.
Selection is confirmed through public `TrackSelectionController` metadata.
Fooyin's matching rules may select additional duplicate/segment rows, and its
public API does not guarantee playlist keyboard focus. Existing filters are
respected.

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

Add `--multichannel` to generate explicit-layout WAVE_FORMAT_EXTENSIBLE preview
fixtures: side 5.1 at 44.1 kHz and standard 7.1 at 96 kHz. The driver verifies the
real decoder's ordered speaker roles, then exercises these tracks through the
stereo headphone sink while checking main playback/queue/settings isolation.
Per-channel routing and audible-sample expectations are covered separately by
the converter and engine suites.

The runner checks live routing, rejects duplicate/colliding streams, samples
whole-process CPU/RSS, and retains logs, fixtures, and `report.json` in the printed
temporary directory. `--artifacts /path/to/new-directory` selects that location.
The runner stops the fooyin and PulseAudio child processes on success or failure.

For staged dependencies, use `--pulseaudio`, `--pactl`, `--pulse-module-dir`,
`LD_LIBRARY_PATH`, and `QT_PLUGIN_PATH`. `--change-default` adds a server-default
change diagnostic.
Software sinks do not establish audible quality, hardware latency, or physical
disconnect isolation.

### Output channel-pair validation

The opt-in `channelpairprobe` uses the real device catalogue, `QtAudioOutput`,
`QAudioSink`, and streaming converter. Its private PulseAudio runner captures a
quad null sink's monitor and verifies channels 1/2 alone, channels 3/4 alone, and
two simultaneous streams on those separate pairs. Distinct 220/330 and 440/660 Hz
tones identify individual channels. Unselected capture lanes must be exactly
zero; spectral checks reject cross-channel mixing during concurrent output.
The probe also pauses/resumes and replaces the preview sink while its concurrent
main-like stream continues. It does not use fooyin's main player or change its
output settings.

```sh
python3 tests/run-channel-pair-validation.py \
  --probe build-test-agent/tests/channelpairprobe
```

This requires `pulseaudio`, `pactl`, and `parec` from PulseAudio/PulseAudio Utils.
For staged dependencies, pass `--pulseaudio`, `--pactl`, `--parec`, and
`--pulse-module-dir`, with the required `LD_LIBRARY_PATH`.
`--artifacts /path/to/new-directory` retains raw four-channel Float captures,
probe/server logs, and `report.json` at a chosen location. No desktop audio server
or physical device is used.

The verified software setup is Qt 6.8.2's PulseAudio backend with PulseAudio 17.0,
48 kHz quad `front-left,front-right,rear-left,rear-right`, converting preview
stereo from 44.1 kHz. The public APIs used (`channelConfiguration`, `channelConfig`,
`channelOffset`, and explicit-device sink construction) are present in Qt 6.4;
a Qt 6.4 runtime has not been tested. Sink creation alone is not routing evidence:
the monitor capture establishes the delivered software mapping.

The real catalogue offers pairs only with a concrete supported format and a
verified map. With native Qt PipeWire (Qt 6.10+ at runtime), a read-only native
registry connection discovers the endpoint's actual `audio.position` metadata.
Qt can advertise a guessed 5.1 layout for a Pro Audio device whose real channels
are `AUX0..AUX5`; transmitting that surround layout makes PipeWire mix the preview
onto AUX0/1. Sequential AUX layouts instead use a count-only, unknown-layout Qt
format, which the native Qt backend transmits as AUX channels. Conversion still
produces stereo first, then copies it into the selected numbered slots.

The six-channel Pro Audio regression runner creates a private PipeWire/WirePlumber
session with `AUX0..AUX5`, reproduces the old 5.1-to-first-pair remapping, then
checks pairs 1/2, 3/4, 5/6 and concurrent streams. It exercises the native metadata
reader, Qt format construction, and actual converter, using `pw-cat` transport so
it also runs on older Qt builds:

```sh
python3 tests/run-pipewire-aux-validation.py
```

Add `--qt-output` when built/running with Qt 6.10+ to use real native `QAudioSink`
playback for the corrected cases. The runner requires PipeWire, WirePlumber,
`pw-cat`, `pactl`, and `parec`; `--artifacts` retains captures and `report.json`.
The protocol regression was validated with Qt 6.8.2, PipeWire 1.4.2, and
WirePlumber 0.5.8.

To inspect routing in a live session, run `pactl list sink-inputs` during preview
and compare the preview stream's channel map with `pactl list sinks`. Debug logs
from `QT_LOGGING_RULES='fy.headphones*.debug=true'` also show the selected pair,
Qt-advertised layout, whether a verified numbered AUX map is in use, and the
requested Qt stream format/configuration.

For physical validation, configure the interface's exposed multichannel profile,
set fooyin's main pair separately (e.g. 1/2), and select the preview pair (e.g. 3/4).
Confirm the actual connectors with independently identifiable main/preview audio,
then exercise pause, seek, replacement, stop, and disconnect.

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
  Explicit pair mode instead requires a concrete known-layout physical format;
  it never falls back to automatic routing. The converter first produces logical
  stereo, then copies left/right into the selected physical slots with every
  other slot zero. Queue capacities, frame alignment, writes, and output clocks
  use physical frame sizes, including conversion tails.
- Input is normalised with fooyin's public float conversion helper. libswresample
  handles streaming rate/sample-format conversion, with explicit mono duplication
  and stereo averaging. ReplayGain is applied to float PCM before mixing,
  resampling, and final integer quantisation. Matching native formats pass through
  unchanged when the computed gain is unity.
- `PreviewReplayGain` reads only the public main ReplayGain settings, capturing
  processing flags, playback-order-resolved gain selection, and both preamps at
  fresh/restarted preview creation. Each `PreviewSession` retains that immutable
  policy across seek generations. Configuration changes do not modify active
  sessions, and no ReplayGain configuration is registered or persisted by the
  plugin.
- Gain and peak fallback follow fooyin's main processor using raw `Track::rg*`
  metadata, not `effectiveRG*` getters. Source gain belongs to the selected
  decoder. In particular, fooyin's native Opus decoder uses `OP_ABSOLUTE_GAIN` to
  suppress header gain even with ReplayGain disabled; preview preserves that
  decoder behaviour rather than adding header gain independently. Peak-based
  prevention constrains source gain, not later resampling overshoot.
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

`Headphones/OutputRoute` stores an empty map for Automatic (also the migration
default), or a map containing the Base64 `DeviceId` and 1-based odd `FirstChannel`.
Device and route changes publish one complete destination notification. Invalid
routes or routes bound to another device are unavailable, never automatic.
Changing devices through the legacy device-only setter resets routing to
Automatic; complete dropdown destinations always specify their own pair/mode.

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
