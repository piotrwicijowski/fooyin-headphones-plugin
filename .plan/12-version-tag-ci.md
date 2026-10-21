# Ticket 12: build the plugin on version tags

Status: implemented; clean local Ubuntu 24.04 build, tests, and packaging pass.
Hosted tag/manual-dispatch runs and download/host-install acceptance remain to be
exercised after the workflow is pushed and a version tag is created.

## Goal and proposed defaults

Add GitHub Actions CI that builds and tests Headphones Preview whenever a plugin
version tag is pushed, and publishes the resulting plugin as a GitHub Release.

- Workflow: `.github/workflows/build-tag.yml`.
- Tag convention: `vMAJOR.MINOR.PATCH`, with optional SemVer prerelease/build
  suffixes; examples: `v0.1.0`, `v0.2.0-rc.1`.
- Initial target: Linux x86-64, using the explicit `ubuntu-24.04` runner and its
  distribution Qt/FFmpeg packages. Record the actual toolchain/library versions.
- Build type: `Release`, with the plugin's existing automated tests enabled.
- Dependency mode: build a pinned fooyin source revision, then use the plugin's
  existing development-tree integration.
- Delivery: a versioned `.tar.gz` plus SHA-256 checksum attached to the tag's
  GitHub Release. Prerelease tags create prereleases; reruns update existing assets.

This target follows the project's initial Linux deployment scope. Additional
platforms can extend this workflow once their compatibility requirements are
established.

## Implementation and verification

- Workflow: [build-tag.yml](../.github/workflows/build-tag.yml).
- Validation, packaging, and release publishing: [release.py](../.github/scripts/release.py), with
  eleven standard-library unit tests in
  [test_release.py](../.github/scripts/test_release.py).
- User instructions: [README](../README.md#download-a-release); maintainer
  procedure: [development guide](../docs/development.md#version-tag-builds).
- Pinned upstream fooyin SHA: `bb8d9f0371b254571fffec310dce3ee1ce005c87`
  (development revision, CMake project version `0.13.1`).
- On 2026-10-21, a fresh Ubuntu 24.04 userland verified the reduced fooyin build,
  plugin Release build, and all eight CTest suites with GCC 13.3, CMake 3.28.3,
  Qt 6.4.2, and FFmpeg 6.1.1. CTest completed with zero failures in 13.89 seconds.
- Staging, ELF dependency/RPATH checks, archive generation, checksum verification,
  and the extracted plugin's 13 lifecycle checks passed. The workflow summary
  script and `actionlint` validation also passed locally.
- The install-directory override uses `:STRING` to retain a relative destination.
  A clean packaging run caught CMake converting an untyped `PATH` override into
  an absolute directory, bypassing the staging prefix; the workflow corrects this.
- Release publishing uses the workflow token with `contents: write`. Eleven
  helper tests pass, including mocked GitHub CLI checks for stable/prerelease
  creation, rerun uploads, missing assets, and publishing failure propagation.
  Live release creation remains part of the first hosted run's acceptance.

## Repository constraints

- `CMakeLists.txt` currently declares plugin version `0.1.0`; `headphones.json.in`
  embeds `PROJECT_VERSION` in the plugin metadata.
- The project requires C++23, Qt Multimedia, and FFmpeg development libraries in
  addition to fooyin Core/Gui/Utils.
- `FOOYIN_FORCE_DEV_TREE=ON` requires both fooyin source headers and generated
  export headers/libraries from a matching build. A source checkout alone is
  insufficient.
- The README reports validation against fooyin `0.14.0-pre` and Qt `6.8.2`.
  Compatibility with Ubuntu 24.04's packaged Qt must be verified during workflow
  implementation, despite the declared Qt 6.4 minimum.
- Eight tests are registered with CTest, including a dynamically loaded plugin
  lifecycle test. The in-process audio driver is an opt-in integration tool;
  hardware/backend validation remains a separate development procedure.

## Implementation plan

### 1. Establish a reproducible dependency baseline

1. Select an upstream fooyin commit compatible with the plugin's public API usage.
   Record its full commit SHA in the workflow as `FOOYIN_REF`, with a comment
   identifying the corresponding fooyin version. Verify this choice with a clean
   configure/build/test run before accepting the workflow.
2. Check out the plugin at the triggering tag and fooyin at `FOOYIN_REF`, including
   fooyin's recursive submodules. Use distinct workspace directories, for example
   `plugin/` and `fooyin/`.
3. Install build tools and dependencies using apt. Start from fooyin's `BUILD.md`
   and add the plugin-specific Qt Multimedia package. The reduced build needs:
   - `build-essential`, `cmake`, `ninja-build`, `pkg-config`, and `git`;
   - `qt6-base-dev`, `qt6-multimedia-dev`, `libqt6svg6-dev`, and
     `libqt6sql6-sqlite`;
   - `libtag1-dev`, `libicu-dev`, `zlib1g-dev`, and `libarchive-dev`;
   - `libavcodec-dev`, `libavfilter-dev`, `libavformat-dev`, `libavutil-dev`,
     and `libswresample-dev`;
   - any additional packages required by the selected fooyin revision, resolved
     during the clean-run validation.
4. Configure fooyin with `BUILD_SHARED_LIBS=ON`, `BUILD_TESTING=OFF`,
   `BUILD_PLUGINS=OFF`, `BUILD_ALSA=OFF`, and `BUILD_TRANSLATIONS=OFF`. Build its
   `fooyin_utils`, `fooyin_core`, and `fooyin_gui` targets, confirming target names
   and options against the pinned revision.
5. If the plugin needs a newer Qt than the proposed runner supplies, select an
   explicit supported Qt version for both fooyin and the plugin and document that
   baseline in the workflow and artifact metadata.

Use an explicit runner image and pinned fooyin source revision to control the
main compatibility inputs. Apt packages and runner images still receive updates;
the recorded versions describe each build rather than implying bit-for-bit
reproducibility.

### 2. Define triggering and version validation

Use the following event shape:

```yaml
on:
  push:
    tags:
      - 'v[0-9]*'
  workflow_dispatch:
```

- GitHub tag filters are globs. Add an early validation step that checks the full
  tag against the agreed SemVer convention before installing/building dependencies.
- For manual runs, dispatch against an existing version tag, for example with
  `gh workflow run build-tag.yml --ref v0.1.0`, and apply the same validation.
  Check out that exact ref. Keep the workflow on the default branch so manual
  dispatch is available, and include it in each tagged commit for push triggers.
- Compare the numeric tag version with the CMake project version and fail clearly
  on a mismatch. For `v0.2.0-rc.1`, the CMake version must be `0.2.0`; preserve the
  full tag in archive names and build metadata. Embedded plugin metadata continues
  to carry the numeric project version.
- Set `permissions: contents: write` for release publishing, a suitable job timeout (initially 45 minutes),
  and concurrency keyed by workflow plus tag. Allow different tags to finish;
  use `cancel-in-progress: false`.
- Pin checkout/upload actions to reviewed full commit SHAs, with version comments.

### 3. Configure, build, and test the plugin

Use dedicated CI paths and explicit CMake arguments rather than local agent
presets. With both repositories checked out beneath the workspace, the core
plugin commands are:

```sh
cmake -S plugin -B build-headphones -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DFOOYIN_FORCE_DEV_TREE=ON \
  -DFOOYIN_SRC_DIR="$GITHUB_WORKSPACE/fooyin" \
  -DFOOYIN_BUILD_DIR="$GITHUB_WORKSPACE/build-fooyin" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DFOOYIN_PLUGIN_INSTALL_DIR:STRING=lib/fooyin/plugins
cmake --build build-headphones --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-headphones \
  --output-on-failure --no-tests=error
```

- Use the same compiler, Qt installation, and build configuration for fooyin and
  the plugin. Confirm the reduced fooyin build provides every linked runtime
  dependency needed by the lifecycle test.
- Verify all eight registered tests execute and pass on a clean hosted runner.
  Resolve shared-library lookup through build RPATH or an explicit CI runtime
  library path if needed.
- Upload CTest failure logs with `if: failure()` so failures remain diagnosable.
- Start with a clean build. Add caching only if measured run times justify it;
  include the fooyin SHA, toolchain, Qt/FFmpeg versions, and CMake options in any
  future dependency-build cache key.

### 4. Stage and publish build artifacts

1. Install the plugin into a clean staging prefix using `cmake --install
   build-headphones --prefix "$GITHUB_WORKSPACE/stage"`.
2. Require `stage/lib/fooyin/plugins/fyplugin_headphones.so` to exist and be
   nonempty. Check ELF dependencies with `ldd` in the configured build environment
   and fail on unresolved dependencies. Record the dependency listing and inspect
   installed RUNPATH/RPATH to catch leaked workspace build paths.
3. Include `LICENSE`, a short installation/compatibility note, and build metadata:
   full plugin tag/commit, fooyin SHA/version, runner OS/architecture, compiler,
   Qt/FFmpeg versions, and build configuration.
4. Package the staged plugin and accompanying files as
   `fooyin-headphones-<tag>-linux-x86_64-ubuntu24.04-fooyin-<short-sha>.tar.gz`.
   Create a sibling `.sha256` file.
5. Publish both files with `gh release create --verify-tag` only after tests and
   staging checks pass. Use `--prerelease` for tags containing a prerelease suffix;
   on reruns, use `gh release upload --clobber` to update existing release assets.
   Authenticate using the workflow token and generate notes for new releases.
6. Add a job summary showing the tag, supported build baseline, test result, and
   artifact name/download location. Document downloading and installation in the
   README when the workflow is implemented.

The archive contains the plugin and accompanying metadata/license files. Its
installation note must identify the matching fooyin/Qt/FFmpeg requirements and
explain extraction into a compatible fooyin prefix. A successful runner build is
evidence for that recorded baseline, not universal Linux binary compatibility.

## Acceptance criteria

- [ ] Pushing `v0.1.0` schedules a build of that tag's commit; prerelease tags also
  schedule builds. Ordinary branch pushes and unrelated tags do not schedule it.
- [x] Malformed version tags and tag/CMake version mismatches fail before the
  dependency build, with actionable errors.
- [ ] A manual run against an existing version tag executes the same pipeline.
- [ ] A clean hosted runner checks out the pinned fooyin revision/submodules,
  builds its required shared libraries, and builds the plugin successfully.
- [x] All eight existing CTest tests pass using the built plugin and matching
  fooyin libraries in a headless environment.
- [x] A successful local build provides a nonempty plugin archive, checksum, license,
  compatibility/install note, and complete build metadata.
- [ ] Failed builds/tests expose diagnostics and do not publish a release.
- [ ] Stable and prerelease tags create the corresponding release type with both
  files attached; reruns update the files on the existing release.
- [ ] Downloading the release files, verifying the checksum, and installing into a
  matching fooyin environment results in a discoverable, loadable plugin.
- [x] The README links to release downloads and included installation/compatibility
  information; the development guide documents tagging, publishing, and reruns.

## Suggested implementation order

1. Confirm the pinned fooyin revision and Linux dependency/toolchain baseline.
2. Add the workflow's tag validation, fooyin build, plugin build, and CTest steps.
3. Add staging checks, versioned packaging, checksums, and release publishing.
4. Exercise manual and tag-triggered runs in GitHub Actions, then document the
   verified release procedure and mark this ticket complete.
