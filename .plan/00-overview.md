# Headphones preview plugin: implementation plan

Status: Phases 1–4 software implemented. Phase 5 automated consolidation and
repeatable same-process software-backend validation now pass, including a
five-minute/635-cycle run with main playback. Phase 5 physical
acceptance and Phase 6 installed-SDK/delivery checks remain. A user confirmed
physical main-A/Bluetooth-preview-B disconnect behavior after disabling
WirePlumber's `linking.pause-playback` policy. Exact environment details and wider
hardware coverage remain open; the distinct Qt 6.8.2/PulseAudio null-sink failure
remains recorded. Phase 5 also records main-stream rerouting on a software-server
default change; neither backend limit is resolved by the passing fixed-device run.

## Goal and confirmed decisions

Provide independent headphone preview of a selected track while fooyin continues
playing the party's main output. Preview is explicitly started by the user and
has its own transport, position, volume, and output device.

- Repository/directory: `/workspace/fooyin-headphones-plugin`.
- Plugin name: **Headphones Preview**; namespace: `Fooyin::Headphones`.
- Plugin binary basename: `fyplugin_headphones` (`.so` on Linux).
- Use **Qt Multimedia `QAudioSink`**, rather than a PipeWire-specific backend.
- Initial deployment: Linux with PipeWire and a **separate output device** for
  headphones. Keep application code portable to other supported Qt platforms.
- Use fooyin's installed public plugin APIs and registered decoders. Host changes
  are forbidden; do not modify fooyin or depend on private headers. Ticket 07 uses
  the existing public APIs for preview-track selection and reveal.
- Use a Makefile, dual CMake discovery modes, presets, and separate agent builds
  for the development workflow.

## First-release scope

- Preview ordinary local files, including seekable CUE/chapter segments, using
  fooyin's decoder selection and source ownership.
- Support mono/stereo PCM inputs, conversion to a supported headphone format,
  and sample-rate conversion. Reject unsupported layouts with a useful message.
- One preview session per application, independent of how many UI objects exist.
- Context-menu action and configurable fooyin shortcuts.
- Layout widget with title, device/status, play/pause, stop, position, seek, and
  headphone volume.
- Plugin configuration dialog with explicit device selection and persistence.
- Correct stop, replacement, seeking, EOF, error, and shutdown behaviour.
- Predictable behaviour when the selected device disappears: stop preview and
  require a new explicit play request after it becomes available again.

Later scope: archive-backed files, remote streams, virtual sources/CDs,
multichannel input downmixing, output-channel-pair routing on one DJ interface,
full master DSP parity, waveform displays, cue-point storage, monitoring
the main mix in headphones, and seamless preview-to-main handoff.

The first release does not automatically enqueue or play a previewed track on
the main output. Existing fooyin queue/play actions remain the way to do that.

## Core behavioural contract

1. Preview never drives the main `PlayerController`, changes its output profile,
   consumes its queue, or publishes main-player playback events.
2. Merely changing track selection never starts or switches preview.
3. There is no default-device fallback when headphone selection is missing or
   unavailable. Explicitly resolve the persisted Qt device ID before opening.
4. Playback never starts automatically at application startup, after a device
   reconnect, or after applying device settings.
5. Stop/replacement/seek discard previous queued audio. Old asynchronous results
   cannot resume or feed the new session.
6. Decoder I/O and conversion happen on a worker. The GUI does not wait for
   decoding or for output buffers to drain during ordinary controls.
7. The plugin owns every object and resource in its preview pipeline and tears
   them down before its code and decoder providers can be unloaded.

Explicit device selection is an application-level routing policy, not proof of
physical separation. OS audio policy can reroute streams; validate actual
PipeWire routing, especially on disconnect. Qt IDs and fooyin backend IDs are
different namespaces, so string comparison cannot reliably detect the same
physical main/headphone device. Document the separate-device setup procedure.

## Documents and implementation sequence

### Follow-up feature tickets

These tickets track follow-up work after the original implementation. Each tracks
its own scope, implementation steps, acceptance criteria, and completion status.

| Ticket | Scope |
| --- | --- |
| [07-keyboard-actions.md](07-keyboard-actions.md) | Implemented: volume up/down, mute/unmute/toggle mute, open configuration, and focus the previewed track |
| [08-preview-start-offset.md](08-preview-start-offset.md) | Implemented: fixed configurable start offset, default 0 seconds |
| [09-preview-replaygain.md](09-preview-replaygain.md) | Implemented: preview ReplayGain using fooyin's shared configuration |
| [10-multichannel-to-stereo.md](10-multichannel-to-stereo.md) | Channel-layout-aware multichannel input downmixing |
| [11-output-channel-pair-selection.md](11-output-channel-pair-selection.md) | Route preview to an explicit pair on a multichannel output |

### Original implementation phases

| Phase | Document | Outcome |
| --- | --- | --- |
| 1 | [01-project-and-build.md](01-project-and-build.md) | Loadable external plugin and repeatable development workflow |
| 2 | [02-session-and-decoding.md](02-session-and-decoding.md) | Independent controller, decoder worker, lifecycle, and buffering |
| 3 | [03-audio-output-and-devices.md](03-audio-output-and-devices.md) | Real QAudioSink output, conversion, device policy, and transport timing |
| 4 | [04-ui-actions-and-settings.md](04-ui-actions-and-settings.md) | Complete user-facing preview workflow |
| 5 | [05-testing-and-validation.md](05-testing-and-validation.md) | Automated regression coverage and real-device validation |
| 6 | [06-delivery-and-checklist.md](06-delivery-and-checklist.md) | Installed-SDK verification, documentation, and release readiness |

Tests belong with the phase that introduces the behaviour; phase 5 consolidates
integration/hardware validation. Each phase should leave a buildable project.

## Proposed project structure

```text
fooyin-headphones-plugin/
  .plan/
  CMakeLists.txt
  CMakePresets.json
  Makefile
  README.md
  LICENSE
  .clang-format
  .gitignore
  headphones.json.in
  cmake/                         # external fooyin discovery helpers if needed
  src/
    headphonesplugin.{h,cpp}
    headphonessettings.{h,cpp}
    headphonessettingsdialog.{h,cpp}
    headphoneswidget.{h,cpp}
    previewcontroller.{h,cpp}
    previewtypes.h
    previewdecoderworker.{h,cpp}
    previewsource.{h,cpp}         # AudioLoader/AudioDecoder adapter
    previewpcmbuffer.{h,cpp}
    previewconverter.{h,cpp}
    previewoutput.h               # narrow output abstraction for tests
    qtaudiooutput.{h,cpp}
    headphonedevices.{h,cpp}
    headphoneslog.h
  tests/
    CMakeLists.txt
    fakes/
    ...test.cpp
```

These filenames are proposed responsibilities, not a requirement to create
empty files up front. Keep abstractions limited to real ownership boundaries
and necessary test seams.

## Source/API baseline

Analysis baseline: sibling fooyin commit `bb8d9f037`, CMake version `0.13.1`,
plugin IID `org.fooyin.fooyin.plugin/2`. Recheck APIs against the actual build
used for implementation; do not equate the project version or IID with a promise
of binary compatibility across every build.

Useful references, relative to this `.plan` directory:

- `../../fooyin/include/core/plugins/{plugin,coreplugincontext}.h`
- `../../fooyin/include/core/engine/{audioloader,audioinput,audioformat,audioconverter}.h`
- `../../fooyin/include/gui/{trackselectioncontroller,widgetprovider}.h`
- `../../fooyin/include/gui/plugins/{guiplugincontext,pluginconfigguiplugin,pluginsettingsprovider}.h`
- `../../fooyin/src/plugins/spectrogram/spectrogramgenerator.cpp`: independent
  decoder, concurrent-decoding check, offset handling, and cancellation precedent.
- `../../fooyin/src/plugins/rgscanner/rgscannerplugin.cpp`: selection/menu integration.
- [Qt QAudioSink](https://doc.qt.io/qt-6/qaudiosink.html),
  [QAudioDevice](https://doc.qt.io/qt-6/qaudiodevice.html), and
  [QMediaDevices](https://doc.qt.io/qt-6/qmediadevices.html).

Private fooyin sources are implementation references only. Verify that all
included fooyin headers come from its installed public interface.
