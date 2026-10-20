# Phase 6: delivery, documentation, and completion checklist

Depends on the previous phases. This document tracks implementation completion;
phase-specific verification evidence is recorded in the corresponding plan.

## Installed-SDK verification

- [ ] Build/install a matching fooyin SDK with public headers enabled into a
  separate development prefix, or use an existing matching installed SDK.
- [ ] Configure this plugin with installed mode explicitly selected and
  `CMAKE_PREFIX_PATH` pointing at that prefix. Development-tree fallback must
  be disabled for this check.
- [ ] Build/test without adding any fooyin `src` include directories or depending
  on source/build-tree-only paths. Check include/link commands if necessary.
- [ ] Use CMake's real install rules and verify the plugin binary lands in the
  intended fooyin plugin directory.
- [ ] Load with the matching installed fooyin and validate runtime discovery of
  Qt Multimedia's backend and explicit FFmpeg conversion dependencies.
- [ ] Document build/API compatibility honestly; the plugin IID alone does not
  make one binary compatible with every fooyin/Qt/compiler combination.

Prefer supported fooyin installation/bundle mechanisms for user distribution.
The Makefile's copy target is primarily a local development convenience. Do not
bundle arbitrary host shared libraries or claim a self-contained binary without
testing its runtime dependency resolution and licence obligations.

## README deliverables

- [ ] Purpose and screenshot/example of the Headphones Preview panel.
- [ ] Supported first-release sources: ordinary local mono/stereo tracks and
  supported seekable CUE/chapter segments. List deferred source/layout features.
- [ ] Requirements: matching fooyin SDK/API, C++23, CMake, supported Qt with
  Multimedia/platform backend, and explicit swresample/avutil development/runtime
  packages. Give Linux package examples after checking actual distribution names.
- [ ] Both build modes, preset/Make usage, install location, and agent versus
  host build-directory conventions.
- [ ] PipeWire two-device setup: choose explicit main device in fooyin, choose
  a different headphone device in plugin settings, add the layout widget, select
  a track, and invoke Preview in headphones.
- [ ] Action IDs and shortcut configuration; no default bindings initially.
- [ ] Exact play/pause/stop/re-preview/seek/multi-selection semantics.
- [ ] Preview volume is separate; preview does not apply the main DSP/ReplayGain
  chain or affect the queue/play counts/scrobbling.
- [ ] Missing-device/disconnect/reconnect behaviour and troubleshooting for no
  output, unsupported formats, backend packages, and OS-level rerouting.
- [ ] Explain that multiple jacks on one card may be mutually exclusive ports;
  initial support expects separately addressable devices, not DJ channel pairs.
- [ ] Commands for tests, debug logs (`fy.headphones.debug=true`), and a supported
  isolated fooyin configuration for manual validation.
- [ ] Exact tested fooyin/Qt/platform/backend/device configuration, including
  limits not yet validated. Keep README aligned as features land.

## Implementation milestones

### M1: loadable project

- [x] Phase 1 build modes, Make/presets, metadata, lifecycle, and smoke test.

### M2: headless preview engine

- [x] Independent controller, worker/source ownership, bounded buffer, fake output.
- [x] Replacement/cancellation/segment/seek tests pass without hardware.

### M3: independent audible output

- [x] QAudioSink format negotiation, streaming conversion, device selection,
  output pumping, drain/reset/position/volume behaviour.
- [x] Two-device Linux playback proof before polishing the complete UI.

  User-reported physical main A/Bluetooth B playback/disconnect works with
  WirePlumber's `linking.pause-playback` disabled. The separate Qt 6.8.2/PulseAudio
  17 null-sink failure remains documented. Exact environment details and wider
  physical coverage remain Phase 5 validation; see the Phase 3 evidence.

### M4: complete user workflow

- [x] Public selection/context-menu/shortcut integration, preview widget,
  settings provider, persistence, and clear missing-device state.
- [x] UI/integration tests.

  Verified through the actual loaded plugin and real main engine in one fooyin
  process using explicit software sinks. The subsequent physical Bluetooth result
  is recorded in Phase 3; wider hardware coverage remains Phase 5 work.

### M5: first-release readiness

- [ ] Automated suites, installed-SDK build, routing/hotplug/long-run/shutdown
  validation, formatting, and complete user documentation.

## Checkpoints requiring evidence during implementation

These are technical verification points, not blockers to writing the plugin:

1. **Device-loss routing:** can the selected Qt/backend combination keep an
   explicitly selected stream isolated when a device disappears? Record actual
   OS behaviour; do not describe application ID checks as a physical guarantee.
2. **Early shutdown:** validate `aboutToQuit` cleanup against actual fooyin exit
   ordering before relying on plugin/provider unload order.
3. **Loader cancellation:** measure delayed local open/initialisation and retain
   safe ownership until it finishes; the public AudioLoader open has no token.
4. **Logical segments:** verify actual decoder offset semantics for CUE/chapters,
   and test exact source-boundary clipping through the converter.
5. **Qt baseline and backend packaging:** avoid newer-only QAudioSink methods
   and validate that installed users actually receive a working audio backend.
6. **Conversion compatibility:** choose supported FFmpeg versions and validate
   sample-container scaling and streaming resampler delay with signal fixtures.

If one of these reveals a need for a core API extension, document the smallest
public contract needed and revisit that part of the plan. Do not work around it
by importing fooyin's internal engine/plugin-manager headers.

## Definition of done

The first release is ready when the user can keep music playing on the main
device, explicitly preview/seek/pause another local track on headphones, stop
or change preview without old audio leaking into the new session, and shut down
cleanly. Main playback and its queue remain independent, device failure produces
clear recoverable state, and automated plus real-device checks support those
claims on the documented tested setup.
