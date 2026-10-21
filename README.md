# Headphones Preview for fooyin

Preview a track through headphones while [fooyin](https://github.com/fooyin/fooyin)
keeps playing through its main output. Listen ahead without replacing the playing
track or changing the queue, volume, or output settings.

## Features

- **Independent playback:** preview, play/pause, stop, seek, and adjust headphone
  volume separately from the main player.
- **Explicit output selection:** choose the headphone device separate from the
  main output. The selection is persisted and does not fallback to other devices,
  so there is no risk of accidentally playing headphone preview on main output.
- **Flexible layout:** five independently placeable widgets for transport,
  seeking, volume, output selection, and track information.
- **Track actions and shortcuts:** preview from a track's context menu or assign
  your own keyboard bindings.
- **Configurable start offset:** skip a fixed number of seconds at the start of
  each preview, default 0s.
- **ReplayGain:** previews reuse fooyin's main ReplayGain configuration, including
  gain selection, preamps, and peak-based clipping prevention.
- **Multichannel downmix:** audition supported 3.0, quad, 5.0, 5.1, and 7.1 tracks
  through stereo headphones using the decoder's speaker layout.
- **Script variables:** display preview metadata, playback state, and a headphone
  indicator in playlist columns.

The plugin has been validated to work on Linux with fooyin 0.14.0-pre and Qt 6.8.2.
See [audio routing and troubleshooting](#audio-routing-and-troubleshooting) for known backend limitations.

## Install

### Download a release

Download a plugin build from the **Assets** section on the
[Releases page](https://github.com/piotrwicijowski/fooyin-headphones-plugin/releases).

Extract the download and follow the included installation instructions. Check
the included compatibility information to ensure the build matches your system
and fooyin installation.

### Build requirements

- A C++23 compiler compatible with your fooyin build.
- CMake 3.19+, or 3.20+ for the supplied presets; Ninja for the preset builds.
- Qt 6.4+ on Linux/macOS, or 6.8+ on Windows: Core, Widgets, Multimedia, Sql,
  Concurrent, and Network. Qt Test is needed only when building tests.
- Matching fooyin sources and built Core/Gui/Utils libraries, or an installed
  fooyin SDK built with `INSTALL_HEADERS=ON`.
- FFmpeg 5+ development libraries: `libswresample` 4+ and `libavutil` 57+,
  discoverable through pkg-config or `CMAKE_PREFIX_PATH`.

For Debian 13, the plugin's build dependencies are:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-multimedia-dev libswresample-dev libavutil-dev
```

Building fooyin itself requires additional dependencies; follow its
[build instructions](https://github.com/fooyin/fooyin/blob/master/BUILD.md).
The runtime also needs Qt Multimedia's platform audio backend and dependencies
(`libqt6multimedia6` on Debian 13).

### Using a fooyin source tree

Build fooyin first. From this repository's root, configure against that matching
source and build tree:

```sh
cmake --preset dev-release \
  -DFOOYIN_SRC_DIR=/path/to/fooyin \
  -DFOOYIN_BUILD_DIR=/path/to/fooyin/build
cmake --build --preset dev-release
```

The defaults are `../fooyin` and `../fooyin/build`. CMake uses the existing fooyin
build without reconfiguring it. On Linux, the plugin is
`build-release/fyplugin_headphones.so`.

To try it without installing, close existing fooyin instances and launch the
matching executable with the plugin directory on its search path:

```sh
FOOYIN_PLUGIN_PATH="$PWD/build-release" /path/to/fooyin --skip-single
```

To install into a fooyin prefix:

```sh
cmake --install build-release --prefix /path/to/fooyin-prefix
```

Development-tree builds default to `${CMAKE_INSTALL_LIBDIR}/fooyin/plugins` on
Linux, `plugins` on Windows, and `fooyin.app/Contents/PlugIns` on macOS. Set
`FOOYIN_PLUGIN_INSTALL_DIR` at configuration time for a different plugin directory.
Restart fooyin after installing or replacing the plugin.

### Using an installed fooyin SDK

Build and install matching fooyin headers with `INSTALL_HEADERS=ON`, then use:

```sh
cmake -S . -B build-sdk -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFOOYIN_FORCE_DEV_TREE=OFF \
  -DCMAKE_PREFIX_PATH=/path/to/fooyin-sdk \
  -DCMAKE_INSTALL_PREFIX=/path/to/fooyin-sdk
cmake --build build-sdk
cmake --install build-sdk
```

## Quick start

1. Select the main output device explicitly in fooyin's playback/output settings.
2. Open fooyin's plugin settings, select **Headphones Preview → Configure**, and
   choose a different, separately addressable headphone device. Apply the change.
3. Select a local track, right-click it, and choose **Preview in headphones**.
4. For on-screen controls, enable layout editing and add widgets from
   **Headphones plugin**. **Headphones Player Controls** includes a
   **Preview selected** button.
5. Adjust the independent headphone volume with **Headphones Volume Controls**.

At the end of a track, preview stops without advancing the selection or playlist.
Stop retains the track for an explicit restart. Device, volume, and start-offset
settings are saved, but playback never starts automatically.

### ReplayGain

There are no headphone-specific ReplayGain settings. The plugin reuses the
processing mode, Track/Album/playback-order selection, and both preamps
configured through fooyin's **Playback → ReplayGain** settings.

### Multichannel tracks

Downmixing is automatic for supported layouts with identified speaker roles:

- **3.0:** front left/right and centre.
- **Quad:** front left/right plus a rear or side surround pair.
- **5.0 / 5.1:** front left/right, centre, a rear or side surround pair, and
  optionally LFE.
- **7.1:** front left/right, centre, LFE, and both rear and side surround pairs.
  Wide variants replace either surround pair with front-left/right-of-centre.

## Layout widgets

Add any combination of these widgets under **Headphones plugin**:

| Widget | Purpose and right-click options |
| --- | --- |
| **Headphones Player Controls** | Preview selected, Stop, and Play/Pause; optionally show separate Play, Pause, and Configure buttons. Automatic, Horizontal, or Vertical orientation. |
| **Headphones Seekbar** | Seek within the preview; configure labels, remaining time, and orientation. |
| **Headphones Volume Controls** | Independent volume and mute; show the slider, mute icon, or both, and choose orientation. |
| **Headphones Output Device** | Select the headphone output; changes apply immediately. |
| **Headphones Track Info** | Show the preview's Artist - Title, or just its title when no artist is available. |

All layout widget instances control the same preview.

## Keyboard shortcuts

Assign bindings in fooyin's shortcut settings under **Headphones Preview**.
There are no default bindings.

| Group | Command | Action ID |
| --- | --- | --- |
| — | Configure | `Headphones.Configure` |
| Headphone playback | Pause | `Headphones.Playback.Pause` |
| Headphone playback | Play | `Headphones.Playback.Play` |
| Headphone playback | Play/Pause | `Headphones.Playback.PlayPause` |
| Headphone playback | Stop | `Headphones.Playback.Stop` |
| Headphone seek | Seek backward (large step) | `Headphones.Seek.Backward.Large` |
| Headphone seek | Seek backward (small step) | `Headphones.Seek.Backward.Small` |
| Headphone seek | Seek forward (large step) | `Headphones.Seek.Forward.Large` |
| Headphone seek | Seek forward (small step) | `Headphones.Seek.Forward.Small` |
| Headphone tracks | Preview selected | `Headphones.Tracks.PreviewSelected` |
| Headphone tracks | Show previewed track | `Headphones.Tracks.FocusPreviewed` |
| Headphone volume | Mute | `Headphones.Volume.Mute` |
| Headphone volume | Toggle mute | `Headphones.Volume.ToggleMute` |
| Headphone volume | Unmute | `Headphones.Volume.Unmute` |
| Headphone volume | Volume down | `Headphones.Volume.Down` |
| Headphone volume | Volume up | `Headphones.Volume.Up` |

Seek commands use fooyin's small and large increments from **Playback → General**.

Volume commands adjust the headphone volume by **5 percentage points**, clamped
to 0–100%.

**Show previewed track** switches to the preview track's playlist, selects matching
tracks, and reveals the retained entry, including while paused and after Stop.

## Script variables

These variables appear under **Headphones Preview** in fooyin's script catalogue.

| Variable | Value |
| --- | --- |
| `%headphones_albumartist%` | Preview album artist, falling back to artist/composer/performer |
| `%headphones_artist%` | Preview artist, falling back to album artist/composer/performer |
| `%headphones_album%` | Preview album |
| `%headphones_title%` | Preview title, falling back to the filename without extension |
| `%headphones_disk%` | Preview disc number; alias: `%headphones_disc%` |
| `%headphones_track%` | Preview track number; alias: `%headphones_tracknumber%` |
| `%headphonesicon%` | `🎧` on tracks matching the playing or paused preview; otherwise empty |
| `%headphones_isplaying%` | `1` while active and not paused; otherwise empty |
| `%headphones_isstopped%` | `1` when stopped, idle, or in an error state; otherwise empty |
| `%headphones_ispaused%` | `1` while paused, including a pending pause during loading; otherwise empty |

Metadata is empty before the first preview and retained after stopping.

### Example variable usage

For a playlist column showing both main-playback and preview indicators:

```text
[%playingicon% ][%headphonesicon% ]%title%
```

For a preview status display:

```text
$if(%headphones_isstopped%,Stopped,$if(%headphones_ispaused%,Paused,Playing))[: %headphones_artist% - ][%headphones_title%]
```

## Audio routing and troubleshooting

Use separately addressable main and headphone devices. Using the same device
results in both main track and headphones preview to be mixed.

The preview pipeline uses Qt Multimedia output and its own volume. It applies
ReplayGain using fooyin's shared configuration, independently of the main audio
pipeline; main DSP and main volume are not applied. Remote streams, virtual/CD
sources, archive-backed tracks, multichannel audio, and decoders that prohibit
concurrent decoding are unsupported.

### Device disconnects and unexpected routing

When Qt reports loss of used device, preview stops. Reconnecting the device
does not restart playback. The plugin never requests a default-device fallback.

### Bluetooth disconnect pauses the main player

Using the plugin with WirePlumber that has the [`linking.pause-playback` policy](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html)
enabled can result in pausing of the main output when the headphones preview is
playing, but the headphones device disappears.

If you are experiencing these symptoms, you can inspect the policy and
temporarily disable it:

```sh
wpctl settings linking.pause-playback
wpctl settings linking.pause-playback false
```

If that resolves the problem, persist the setting with:

```sh
wpctl settings --save linking.pause-playback false
```

This disables unplug-triggered auto-pause for **all media players in the session**.
To restore the usual default, use `wpctl settings --save linking.pause-playback true`.

### Plugin loading or playback problems

- Check that the plugin was built against the running fooyin and matching Qt
  libraries, and restart fooyin after replacing it.
- If preview is disabled, select a supported local track and configure an
  available headphone output.
- If no audio devices are listed, check the Qt Multimedia platform audio backend
  and its runtime dependencies.
- Enable debug output with `QT_LOGGING_RULES='fy.headphones.debug=true'` when
  launching fooyin. See the [development guide](docs/development.md) for logging
  and integration diagnostics.

## Contributing

Bug reports, hardware compatibility reports, and patches are welcome. For audio
issues, include your fooyin and Qt versions, operating system, audio server,
selected devices, reproduction steps, and relevant `fy.headphones` logs.

See the [development guide](docs/development.md) for build presets, Make targets,
automated tests, integration validation, and an overview of the preview engine.
