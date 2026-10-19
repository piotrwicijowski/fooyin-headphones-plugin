# Headphones Preview for fooyin

An external plugin for independent headphone preview while fooyin continues
playing its main output. **Current status: user workflow implemented, Phase 5
automated/software-backend validation passed, with a user-validated physical
Bluetooth setup.** Broader release validation is
ongoing. The plugin provides track actions,
configurable shortcuts, a device settings dialog, and a layout panel backed by
one independent controller, decoder worker, streaming converter, and explicit-device QAudioSink.
See [the implementation plan](.plan/00-overview.md).
Plugin load remains idle. Validation now exercises the actual loaded plugin
inside fooyin while the main player is active, including coexistence with Vim
Motions. See [same-process validation](#same-process-integration-validation) and
[routing validation](#routing-validation-and-known-limit).

## Using Headphones Preview

1. Select the main device explicitly in fooyin's playback/output settings.
2. In fooyin's plugin settings, select **Headphones Preview → Configure**, then
   choose a different, separately addressable headphone device and apply it.
3. Enable layout editing and add **Headphones plugin → Headphones Preview**. The panel is
   limited to one active layout instance.
4. Select a track and invoke **Preview in headphones** from its track context
   menu, or click **Preview selected** in the panel.
5. Use the panel's Play/Pause, Stop, position/seek, and independent headphone
   volume controls. **Device…** opens the same configuration dialog as the plugin
   settings page.

For multiple selected tracks, preview uses the **first track in the selection
controller's order**. If that track is unsupported, preview is disabled; it does
not search later selections or alter the main queue. Changing selection alone
never starts or replaces preview. Context-menu actions retain the track they were
built for; keyboard actions use the focused selection. The panel uses fooyin's
retained display selection so clicking a control does not lose the intended track.

The panel shows preview title/artist, the selected headphone device, status/error,
elapsed/duration, and volume percentage. Unknown-duration and unseekable sources
have no enabled seek slider. Dragging shows a tentative position and sends one
seek on release; a paused preview stays paused. Closing/recreating the layout
panel does not stop preview; registered shortcuts remain usable without a panel.

### Separate layout widgets

Layout editing also provides three independently placeable widgets under **Headphones plugin**:

| Widget | Right-click configuration |
| --- | --- |
| **Headphones Player Controls** | Show/hide Stop, Pause, Play, and Play/Pause individually; Automatic, Horizontal, or Vertical orientation |
| **Headphones Seekbar** | Show labels, show remaining time, and orientation |
| **Headphones Volume Controls** | Show/hide the slider and mute icon; orientation |

Each instance saves its configuration with the layout. All instances control the
same headphone preview, including the original **Headphones Preview** panel,
which still contains its existing controls. The new player controls use fooyin's
standard tool-button styling and icons. Click anywhere on the new seekbar to
seek to that position, or drag and release; right-click during a drag cancels it.
Paused previews stay paused after seeking. The seekbar has no played/scrobble
threshold marker. Click its total-time label to toggle remaining time.

The separate volume slider sets volume directly at the clicked position. The
icon mutes/unmutes the preview, restoring the last nonzero headphone volume.
With only the icon enabled, hovering over it reveals a vertical volume slider.
At least one of the volume slider or icon must remain enabled.

The separate Play and Pause buttons are hidden by default. Enable **Show Play**
and **Show Pause** in the player controls' right-click menu. Like fooyin's main
controls, Play starts/resumes the retained preview or restarts it when already
playing; Pause pauses it without toggling back to playback on repeated clicks.

### Configurable commands

Assign shortcuts in fooyin's normal shortcut settings under **Headphones Preview**.
There are no default bindings, allowing coexistence with ordinary transport keys
and Vim Motions.

| Action ID | Command |
| --- | --- |
| `Headphones.PreviewSelected` | Preview in headphones |
| `Headphones.PlayPause` | Play/pause headphone preview |
| `Headphones.Play` | Play headphone preview |
| `Headphones.Pause` | Pause headphone preview |
| `Headphones.Stop` | Stop headphone preview |
| `Headphones.SeekBackward` | Seek headphone preview backward 5 seconds |
| `Headphones.SeekForward` | Seek headphone preview forward 5 seconds |

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
public fooyin SettingsManager; playback state is never persisted. The device
dialog stages edits until Apply/OK; Cancel discards unapplied changes. A missing
saved/pending device stays explicitly unavailable rather than selecting a default,
and duplicate descriptions are disambiguated by ID. Hotplug preserves pending
selection. The dialog does not contain a stale volume field: live volume belongs
to the panel, so applying device settings cannot overwrite a newer volume value.

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

Phase 5 also observed the explicit **main test QAudioSink** moving to the headphone
sink after changing the private PulseAudio server's default. The preview stream
stayed on its selected sink, producing a routing collision. The optional
`--change-default` diagnostic below reports that as a failure. This is a separate
backend/server-policy limit, even though the plugin did not change main output
settings. See [Phase 5 evidence](.plan/05-testing-and-validation.md#phase-5-verification-evidence-2026-10-19).

Fake-device tests verify stop/reset and no reconnect autoplay when loss is
reported. A user has confirmed concurrent main playback on A and Bluetooth
preview on B, correct unavailability/no transfer after disconnect, and resolution
of the unwanted main pause by disabling WirePlumber's policy below. This validates
that reported setup; exact versions and broader physical routing/hotplug coverage
remain release checks. Software null sinks alone do not establish hardware guarantees.

### Bluetooth disconnect pauses main playback

If main playback pauses when the active preview device disconnects, check whether
an external MPRIS `Pause` was sent to `org.mpris.MediaPlayer2.fooyin`. One user
reported exactly this, while preview correctly became unavailable without
transferring to another device, and confirmed that disabling the WirePlumber
policy below resolved the issue.

WirePlumber's [`linking.pause-playback` policy](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html)
defaults to true and sends MPRIS pause requests to applications that had streams
on a removed sink. The upstream hook matches by process/PID, so it can associate
the headphone stream with fooyin's main MPRIS player even when main output is on
another device. Confirm the D-Bus sender with `busctl --user status :1.N` using
the sender ID from the trace, and check/test the policy:

```sh
wpctl settings linking.pause-playback
wpctl settings linking.pause-playback false
```

The second command changes the running session. If it resolves the issue, persist
it with `wpctl settings --save linking.pause-playback false`. This disables
unplug-triggered auto-pause **for all media players in the session**; normal
explicit MPRIS pause controls remain available. Restore the previous value if
desired (normally `true`). Repeat the disconnect test to verify both uninterrupted
main playback and preview stop/no-reroute: disabling auto-pause is not a routing
guarantee. If the setting is unavailable or the sender is not WirePlumber, the
source of the external pause needs further investigation. The plugin does not
silence incoming main-player commands or automatically resume main playback.

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

Six CTest suites run without audio hardware or an audio server:

- `headphones.plugin-lifecycle` loads the real module using `QPluginLoader`,
  checking metadata/interfaces, partial/duplicate initialisation, retained loader
  lifetime, late callbacks, destructor cleanup, and `aboutToQuit` cleanup.
- `headphones.preview-engine` uses gated fake sources/output for transport,
  delayed-open/read cancellation, full-buffer shutdown, stale results/EOF,
  replacement, exact segment boundaries, fractional-frame seek rounding,
  pause-preserving/coalesced seeks, starvation, malformed formats, partial-frame
  writes, device readiness, errors/recovery, and processed-output positioning.
  Phase 5 adds every-sample checks for converted replacement/coalesced-seek
  isolation at 44.1/96 -> 48 kHz and queue bounds for hour/week source timelines.
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
- `headphones.gui` checks QAction/menu targets and first-track rules, settings
  staging/Apply/Cancel/hotplug, shared dialog lifetime, pause-preserving scrubbing,
  visible volume feedback, and panel recreation without session replacement.
  The loaded-module lifecycle suite also uses real public ActionManager and
  WidgetProvider instances to verify the actual plugin's registration, configuration,
  persistence, instance limit, and cleanup.

### Same-process integration validation

The primary integration driver is a **test-only plugin loaded into the actual
matching fooyin process alongside Headphones Preview**. It does not instantiate
another preview engine or call a test replacement for the Headphones plugin.
It creates the registered panel and invokes the actual public context-menu,
shortcut, widget and configuration paths. Fooyin's real main PlayerController and
engine play concurrently through an explicit-device Qt test output backend.

The driver checks main track/playlist/queue snapshots and spies on main transport,
seek, play-state, track/play-count, output/device, and volume/output-setting events.
Normal main-track play-count events are allowed during extended playback; any
preview-track event fails the test.
It requires main output frames to keep increasing while preview is paused. It
also exercises real configured keyboard shortcuts with Vim Motions loaded when
requested, panel recreation, same-ID Apply, changed-device stop, and no autoplay
after restoring device settings.

Built with `BUILD_TESTING=ON`, the driver lives under
`build-test-agent/tests/inprocess/` and is never installed or run by ordinary
CTest. To run it, use a fresh isolated fooyin configuration, two explicit Qt
output IDs, and three **distinct 20-second local fixtures** (main 48 kHz stereo
Float WAV, preview/other seekable WAVs). The test uses the default English UI.
After building both plugin and tests:

```sh
env FOOYIN_PLUGIN_PATH="$PWD/build-agent:$PWD/build-test-agent/tests/inprocess" \
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

For Vim coexistence, append its matching build directory to `FOOYIN_PLUGIN_PATH`
and set `HEADPHONES_TEST_EXPECT_VIM=1`; the driver then requires its widget
registration to be present. It assigns test shortcuts in the isolated config,
selects the test main backend, starts/stops test main playback, and exits normally
only after printing `HEADPHONES SAME-PROCESS PASS`. It suppresses the host's modal
first-run Quick Setup in this isolated config. It also checks preview continuing
with main paused/stopped, then quits with preview active. It has a 45-second
failure timeout, extended by `HEADPHONES_TEST_STRESS_SECONDS` when set (0..3600;
main fixture must be at least that duration plus 20 seconds). Explicit software
sinks provide same-process regression coverage;
physical disconnect/routing checks remain configuration-specific. The user's
Bluetooth setup passed with the WirePlumber policy change documented above.

#### Repeatable software-sink validation (Linux)

The optional Python 3 runner creates its own private PulseAudio server with two
null sinks, generates the distinct WAV fixtures, and launches the same real
fooyin/plugin workflow with fresh HOME/XDG directories and no desktop session bus.
It requires `pulseaudio` and `pactl` (Debian packages `pulseaudio` and
`pulseaudio-utils`), plus a matching test-enabled plugin/fooyin build:

```sh
python3 tests/run-inprocess-validation.py \
  --fooyin /path/to/matching/fooyin \
  --plugin-dir build-test-agent \
  --vim-plugin-dir /path/to/fooyin-vim-plugin/build-agent \
  --seconds 300
```

Omit `--vim-plugin-dir` for the standalone Headphones workflow. `--seconds 0`
runs just the integration workflow; the default is 60 seconds of repeated
replacement, paused seek bursts, resume, volume and stop. The runner checks live
server routing, rejects duplicate/colliding streams, samples whole-process CPU/RSS,
and retains logs/fixtures/`report.json` in the printed temporary directory. Use
`--artifacts /path/to/new-directory` to select that location. Both child processes
are stopped on success or failure. This runner is outside ordinary CTest.

For staged installations, `--pulseaudio`, `--pactl`, `--pulse-module-dir`,
`LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` can select the staged binaries/libraries.
Add `--change-default` to exercise a private-server default change; the tested
Qt 6.8.2/PulseAudio 17 setup fails this routing assertion as documented above.
Software-sink validation does not measure audible glitches, hardware latency, or
physical unplug isolation.

An additional executable is built with tests but deliberately **not registered
with CTest**. This low-level diagnostic only enumerates devices or plays a one-second 440 Hz development tone
only on an explicitly supplied ID:

```sh
build-test-agent/tests/previewaudiosmoke --list-devices
build-test-agent/tests/previewaudiosmoke --device-id 'BASE64_ID_FROM_THE_LIST'
```

An unavailable ID fails instead of opening the default. The standalone tone
does not establish plugin/main-player coexistence; use the same-process driver
above for that. Physical listening/routing tests still require actual hardware.

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

Phases 1–4 software was validated on Debian 13 x86_64 with GCC 14.2.0, CMake 3.31.6,
Qt 6.8.2, libswresample 5.3.100, libavutil 59.39.100, and fooyin 0.13.1
(`bb8d9f037`, sibling `build-rebase-agent`). Debug and RelWithDebInfo builds,
all six automated suites (90 Qt Test checks passed), parallel Make workflows,
path overrides, and a real fooyin load/normal exit passed. A temporary host probe
also used fooyin's registered decoder on a generated 44.1 kHz WAV: CUE and chapter
segments matched all expected samples (749 and 574 frames), and shutdown with a
fresh open queued completed normally. Phase 3 then used real QAudioSink streams
on an isolated PulseAudio 17 server. Eight real host-decoder fixtures passed:
MP3/FLAC/WAV at 44.1/48 kHz and FLAC/WAV at 96 kHz, converted to 48 kHz stereo
Float. Each 250 ms file yielded 12,000 output frames; channel-specific spectral
checks verified pitch and amplitude before sink volume. Stream inspection also
confirmed simultaneous streams on two software sinks and exposed the disconnect
rerouting failure documented above. Phase 4 additionally passed the included
same-process driver with the actual Headphones and Vim Motions plugins loaded.
Main playback produced over 85,000 output frames, continued while preview paused,
and retained its track, playlists, queue, volume and output selection without
preview-induced main events. GUI/module lifecycle suites also passed
AddressSanitizer/UBSan checks (Qt leak detection excluded). Dependencies were staged outside the
project without system installation.

Phase 5 on the same baseline passed all six suites (**94 Qt Test checks**), plus
the new converted-sample/queue-bound cases under ASan/UBSan. A five-minute
same-process run with Vim Motions passed **635 cycles**, **1,818 separate-routing
observations**, over **14.5 million main frames**, and clean shutdown with preview
active. Maximum observed command-to-Playing time was 193 ms (50 ms test sampling).
Whole-host CPU/RSS measurements and the separate failed default-routing diagnostic
are recorded in the [Phase 5 evidence](.plan/05-testing-and-validation.md#phase-5-verification-evidence-2026-10-19).

Installed-SDK delivery, Qt 6.4 runtime, other platforms, and wider physical
PipeWire main/headphone coverage remain open. The user-reported Bluetooth
disconnect setup is confirmed with the documented WirePlumber policy. See the
[Phase 1 evidence](.plan/01-project-and-build.md#completion-evidence-2026-10-18)
[Phase 2 evidence](.plan/02-session-and-decoding.md#completion-evidence-2026-10-18),
[Phase 3 evidence](.plan/03-audio-output-and-devices.md#verification-evidence-2026-10-18),
and [Phase 4 evidence](.plan/04-ui-actions-and-settings.md#completion-evidence)
for details, including the tested host's restart-based plugin-disable policy.
