.DEFAULT_GOAL := help

PLUGIN_CONFIG_PRESET ?= dev
PLUGIN_CONFIG_PRESET_RELEASE ?= dev-release
PLUGIN_TEST_PRESET ?= test-agent
PLUGIN_BUILD_DIR ?= $(if $(filter agent,$(PLUGIN_CONFIG_PRESET)),build-agent,build)
PLUGIN_RELEASE_BUILD_DIR ?= build-release
PLUGIN_TEST_BUILD_DIR ?= build-test-agent
FOOYIN_SRC_DIR ?= $(abspath ../fooyin)
FOOYIN_BUILD_DIR ?= $(FOOYIN_SRC_DIR)/build
FOOYIN_PLUGIN_DIR ?= $(FOOYIN_BUILD_DIR)/run/lib/fooyin/plugins
# Evaluate at launch time, including after a build in the same make invocation.
FOOYIN_BIN ?= $(shell if test -x "$(FOOYIN_BUILD_DIR)/run/fooyin"; then printf '%s' "$(FOOYIN_BUILD_DIR)/run/fooyin"; elif test -x "$(FOOYIN_BUILD_DIR)/run/bin/fooyin"; then printf '%s' "$(FOOYIN_BUILD_DIR)/run/bin/fooyin"; fi)
QT_LOGGING_RULES ?= fy.headphones.debug=true
CMAKE_ARGS ?=
FOOYIN_CMAKE_ARGS ?= -DBUILD_TRANSLATIONS=OFF
TEST_ARGS ?= --output-on-failure

# Command-line overrides are passed after the preset, for every configuration.
FOOYIN_CMAKE_PATHS = -DFOOYIN_SRC_DIR="$(FOOYIN_SRC_DIR)" -DFOOYIN_BUILD_DIR="$(FOOYIN_BUILD_DIR)"
CONFIG_ENV = $(if $(strip $(XDG_CONFIG_HOME)),XDG_CONFIG_HOME="$(XDG_CONFIG_HOME)")

.PHONY: help fooyin-configure fooyin-build configure configure-release configure-tests \
    build build-release install-plugin sync-plugin install-plugin-release \
    run run-debug dev dev-release test check

help:
	@printf '%s\n' \
	  'Headphones Preview (Linux development convenience targets):' \
	  '  fooyin-configure / fooyin-build  Configure or build the selected sibling tree' \
	  '  configure / build              Configure and build Debug' \
	  '  configure-release / build-release  Configure and build RelWithDebInfo' \
	  '  install-plugin / sync-plugin   Build, then copy only fyplugin_headphones.so' \
	  '  dev / dev-release              Build fooyin, configure/build/copy the plugin' \
	  '  configure-tests / test / check Build and run Qt lifecycle tests' \
	  '  run / run-debug                Launch fooyin (debug enables fy.headphones)' \
	  '' \
	  'Overrides: PLUGIN_CONFIG_PRESET, PLUGIN_CONFIG_PRESET_RELEASE, PLUGIN_TEST_PRESET,' \
	  '  PLUGIN_BUILD_DIR, PLUGIN_RELEASE_BUILD_DIR, PLUGIN_TEST_BUILD_DIR,' \
	  '  FOOYIN_SRC_DIR, FOOYIN_BUILD_DIR, FOOYIN_PLUGIN_DIR, FOOYIN_BIN,' \
	  '  CMAKE_ARGS, FOOYIN_CMAKE_ARGS, TEST_ARGS, QT_LOGGING_RULES, XDG_CONFIG_HOME.' \
	  'Container: make check PLUGIN_CONFIG_PRESET=agent FOOYIN_BUILD_DIR=/path/to/fooyin/build-agent' \
	  'Close existing fooyin instances before validating a newly copied plugin.'

fooyin-configure:
	cmake -S "$(FOOYIN_SRC_DIR)" -B "$(FOOYIN_BUILD_DIR)" $(FOOYIN_CMAKE_ARGS)

fooyin-build:
	cmake --build "$(FOOYIN_BUILD_DIR)"

configure:
	cmake --preset "$(PLUGIN_CONFIG_PRESET)" -B "$(PLUGIN_BUILD_DIR)" $(FOOYIN_CMAKE_PATHS) $(CMAKE_ARGS)

configure-release:
	cmake --preset "$(PLUGIN_CONFIG_PRESET_RELEASE)" -B "$(PLUGIN_RELEASE_BUILD_DIR)" $(FOOYIN_CMAKE_PATHS) $(CMAKE_ARGS)

configure-tests:
	cmake --preset "$(PLUGIN_TEST_PRESET)" -B "$(PLUGIN_TEST_BUILD_DIR)" $(FOOYIN_CMAKE_PATHS) $(CMAKE_ARGS) -DBUILD_TESTING=ON

build: configure
	cmake --build "$(PLUGIN_BUILD_DIR)"

build-release: configure-release
	cmake --build "$(PLUGIN_RELEASE_BUILD_DIR)" --config RelWithDebInfo

install-plugin: build
	install -d "$(FOOYIN_PLUGIN_DIR)"
	install -m 755 "$(PLUGIN_BUILD_DIR)/fyplugin_headphones.so" "$(FOOYIN_PLUGIN_DIR)/fyplugin_headphones.so"

sync-plugin: install-plugin

install-plugin-release: build-release
	install -d "$(FOOYIN_PLUGIN_DIR)"
	install -m 755 "$(PLUGIN_RELEASE_BUILD_DIR)/fyplugin_headphones.so" "$(FOOYIN_PLUGIN_DIR)/fyplugin_headphones.so"

# Recursive invocations form an ordered pipeline even under make -j. Existing
# sibling caches are used as-is; configuring fooyin is an explicit operation.
dev: fooyin-build
	$(MAKE) install-plugin

dev-release: fooyin-build
	$(MAKE) install-plugin-release

test: configure-tests
	cmake --build "$(PLUGIN_TEST_BUILD_DIR)"
	env QT_QPA_PLATFORM=offscreen ctest --test-dir "$(PLUGIN_TEST_BUILD_DIR)" $(TEST_ARGS)

check: build test

run:
	@test -n "$(FOOYIN_BIN)" && test -x "$(FOOYIN_BIN)" || { printf '%s\n' 'No fooyin executable found. Build fooyin or set FOOYIN_BIN=/path/to/fooyin.' >&2; exit 1; }
	env $(CONFIG_ENV) "$(FOOYIN_BIN)"

run-debug:
	@test -n "$(FOOYIN_BIN)" && test -x "$(FOOYIN_BIN)" || { printf '%s\n' 'No fooyin executable found. Build fooyin or set FOOYIN_BIN=/path/to/fooyin.' >&2; exit 1; }
	env $(CONFIG_ENV) QT_LOGGING_RULES="$(QT_LOGGING_RULES)" "$(FOOYIN_BIN)"
