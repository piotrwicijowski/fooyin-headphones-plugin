# Phase 1: project scaffold and build workflow

Depends on: [00-overview.md](00-overview.md).

## Deliverables

- [x] C++23/CMake external plugin project, Qt AUTOMOC, metadata template,
  formatting configuration, licence, and build-artifact ignores.
- [x] `HeadphonesPlugin` implements `QObject`, `Plugin`, `CorePlugin`,
  and `GuiPlugin`; matching
  `Q_PLUGIN_METADATA` and `Q_INTERFACES` declarations.
  `PluginConfigGuiPlugin` follows in Phase 4 with the real settings provider.
- [x] Separate core/GUI initialisation and idempotent shutdown; no audio starts
  just because the plugin loads.
- [x] `fy.headphones` logging category with opt-in debug output.
- [x] CMake discovery, presets, Makefile workflow, and Qt Test/CTest setup.

## Dependencies and compatibility

- CMake 3.19+ and a compiler compatible with the target fooyin's C++23 build.
- Qt 6.4+ baseline for plugin code, subject to fooyin's platform-specific higher
  minimum (the inspected Windows build requires 6.8).
- Qt Core, Widgets, Multimedia, and any Qt modules required by the exported
  fooyin targets/public headers. Qt Test only with `BUILD_TESTING=ON`.
- Fooyin Core/Gui/Utils from a matching installed SDK or sibling build.
- FFmpeg `libswresample` and `libavutil` for streaming sample-rate conversion,
  linked explicitly by this plugin. Do not rely on fooyin's private/transitive
  FFmpeg linkage. Use package discovery compatible with intended platforms and
  report missing development packages clearly.
- Include packaging/runtime requirements for Qt Multimedia's platform audio
  backend; installing Qt Widgets alone is insufficient.

Use the QAudioSink `QIODevice` interface available at the baseline. Do not use
the newer callback API or frame-buffer helpers introduced in later Qt versions.
Handle the Qt 6.4-era `QAudio` namespace versus later `QtAudio` naming with a
small compatibility boundary if needed.

## Fooyin discovery modes

Adapt the Vim plugin's two-mode approach:

1. **Installed SDK:** `find_package(Fooyin CONFIG ...)`, imported targets, and
   `create_fooyin_plugin(Headphones ...)`. Fooyin needs `INSTALL_HEADERS=ON`.
2. **Development tree:** configurable `FOOYIN_SRC_DIR` and `FOOYIN_BUILD_DIR`,
   defaulting to sibling `../fooyin` and its `build` directory. Import the built
   Core/Gui/Utils libraries and generated export-header directories.

Expose an explicit `FOOYIN_FORCE_DEV_TREE` switch. When installed mode is
explicitly requested, failure to find that SDK must fail rather than silently
falling back; this makes the installed-header verification meaningful.

Development mode may include `fooyin/include` and generated export headers; it
must not add fooyin's private `src` directories as general include paths. Use
platform-aware library discovery and CMake installation paths rather than
hardcoding Unix `.so` names into CMake.

Do not reconfigure existing sibling build directories with another compiler or
generator. Honour explicit overrides and use an existing compatible fooyin
build, or a dedicated build directory.

## Presets

Provide named configure/build presets with Ninja and explicit binary dirs:

| Preset | Plugin binary dir | Purpose |
| --- | --- | --- |
| `dev` | `build/` | Host/local Debug |
| `dev-release` | `build-release/` | Host/local RelWithDebInfo |
| `agent` | `build-agent/` | Container/agent Debug |
| `test-agent` | `build-test-agent/` | Agent tests, `BUILD_TESTING=ON` |

Do not share CMake caches between host/container paths or between Debug and
release presets. Pass the sibling fooyin build override consistently to all
presets. A test preset should set `QT_QPA_PLATFORM=offscreen`.

## Makefile

Retain the useful names/conventions of the Vim plugin:

- `help`, `fooyin-configure`, `fooyin-build`.
- `configure`, `configure-release`, `build`, `build-release`.
- `install-plugin`, `sync-plugin`, `run`, `run-debug`, `dev`, `dev-release`.
- Add `configure-tests`, `test`, and `check` for the plugin's actual test targets.

Expose `PLUGIN_BUILD_DIR`, configure presets, `FOOYIN_SRC_DIR`,
`FOOYIN_BUILD_DIR`, `FOOYIN_PLUGIN_DIR`, `FOOYIN_BIN`, `QT_LOGGING_RULES`, and
optional `XDG_CONFIG_HOME`. Default debug logging to `fy.headphones.debug=true`.
Prefer the actual sibling directory in this workspace, with overrides for other
mount layouts. Auto-detect the known `run/fooyin` and `run/bin/fooyin` layouts.

Improve on the reference where necessary:

- Encode configure -> build -> copy dependencies so `make -j dev` cannot race.
- Have `install-plugin` build or verify the current target, rather than copying
  a stale binary because an old `.so` happens to exist.
- Propagate Make overrides to CMake configure commands; avoid presets silently
  choosing a different fooyin build than `fooyin-build`/`install-plugin`.
- Give release builds a matching configure/build/copy directory throughout.
- Only set `XDG_CONFIG_HOME` when supplied; do not export it as an empty string.
- Keep Linux-specific convenience copying in Make clearly documented. CMake
  remains the portable build/install interface.
- Quote paths; plugin install targets copy only this plugin and never remove
  or replace unrelated plugin files.
- Run targets should fail clearly when no executable exists. Explain that an
  already-running fooyin may receive the launch request instead of reloading
  the newly built plugin; close it before plugin-load validation.

## Verification and acceptance

- [x] Build the scaffold against the sibling development tree.
- [x] Load it into the matching fooyin build and confirm one core and one GUI
  initialisation, with no output opened.
- [x] Confirm shutdown is safe if GUI/audio initialisation never completed.
- [x] Exercise preset/build-dir overrides and dry-run parallel Make workflows.
- [x] Add a first lifecycle smoke test with fake dependencies; subsequent
  phases extend it with meaningful transport behaviour.
- [x] Document the chosen minimum Qt/fooyin API baseline and build commands.

The installed-SDK build is a required delivery check in phase 6, not an excuse
to defer fixing private-header dependencies discovered earlier.

## Completion evidence (2026-10-18)

- Debian 13 x86_64, GCC 14.2.0, CMake 3.31.6, Qt 6.8.2, libswresample
  5.3.100 and libavutil 59.39.100; fooyin 0.13.1 at `bb8d9f037`, using
  `/workspace/fooyin/build-rebase-agent`. Existing build/compiler/generator
  settings were retained; the `fooyin` target was built to obtain its executable.
- Container Qt Multimedia development/runtime packages were absent. Matching
  Debian `6.8.2-8` packages were extracted under
  `/tmp/opencode/headphones-deps/root` (no system installation). Verification
  configurations passed
  `-DQt6Multimedia_DIR=/tmp/opencode/headphones-deps/root/usr/lib/x86_64-linux-gnu/cmake/Qt6Multimedia`.
- `cmake --preset agent`, `cmake --build --preset agent`, and the corresponding
  `test-agent` configure/build succeeded with the explicit sibling-build override.
  `ctest --preset test-agent -V`: 1 CTest suite passed, Qt Test reports 8 passed,
  0 failed. Tests load/unload the actual module with inert public host contexts,
  including no/core-only/full initialisation, duplicate calls, late callbacks,
  GUI-before-core, destructor cleanup, and application-quit cleanup.
- Actual fooyin launch used `QT_QPA_PLATFORM=offscreen`, `--skip-single`, isolated
  HOME/XDG directories, and `FOOYIN_PLUGIN_PATH` pointing to `build-agent`.
  A temporary public-API probe plugin outside this project scheduled a normal
  application quit after GUI startup. Exit status was 0 and the headphones log
  contained exactly one each of `Core initialised`, `GUI initialised`, and
  `Shutdown complete`. No decoder or audio output is constructed by the scaffold.
- `make -j8 check PLUGIN_CONFIG_PRESET=agent` passed with the same sibling and
  Qt overrides. `make -j8 install-plugin-release` configured/built/copied from
  `build-release-agent` throughout. Debug copying also passed with spaces in both
  `PLUGIN_BUILD_DIR` and `FOOYIN_PLUGIN_DIR`; copied files matched their sources
  and an unrelated destination entry remained intact.
- `make -n -j8 dev` and `dev-release` showed ordered sibling-build then plugin
  configure/build/copy pipelines with propagated overrides. Run-command dry runs
  confirmed executable detection and optional XDG handling; an explicitly missing
  executable failed with the intended message.
- `cmake --install build-agent --prefix /tmp/opencode/headphones-install`
  installed only `lib/fooyin/plugins/fyplugin_headphones.so` with relative RPATH.
  FFmpeg's non-pkg-config discovery path also configured/built successfully
  (Qt's optional XKB probe was disabled for that environment-only check).
- Explicit installed mode with no SDK failed at `find_package(Fooyin CONFIG
  REQUIRED)`, confirming fallback is disabled. A real installed-SDK build remains
  the Phase 6 delivery check. Qt 6.4, other platforms, and audible/backend/device
  behaviour have not been validated by this scaffold ticket.
- Touched C++ files pass `clang-format --dry-run --Werror`.
