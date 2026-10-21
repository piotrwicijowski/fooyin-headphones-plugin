cmake_minimum_required(VERSION 3.19)
project(FooyinHeadphones VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(BUILD_TESTING "Build the Headphones Preview tests" OFF)
option(FOOYIN_FORCE_DEV_TREE "Use a fooyin source/build tree instead of an installed SDK" ON)
set(FOOYIN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fooyin" CACHE PATH "fooyin source directory")
set(FOOYIN_BUILD_DIR "${FOOYIN_SRC_DIR}/build" CACHE PATH "Matching fooyin build directory")

include(GNUInstallDirs)
if(WIN32)
    set(HEADPHONES_QT_MIN_VERSION 6.8)
else()
    set(HEADPHONES_QT_MIN_VERSION 6.4)
endif()
find_package(Qt6 ${HEADPHONES_QT_MIN_VERSION} REQUIRED
    COMPONENTS Core Widgets Multimedia Sql Concurrent Network)

if(FOOYIN_FORCE_DEV_TREE)
    include(cmake/FooyinDevTree.cmake)
else()
    # OFF explicitly selects installed mode: never silently fall back to source headers.
    find_package(Fooyin CONFIG REQUIRED)
    message(STATUS "Using installed Fooyin SDK: ${Fooyin_DIR}")
endif()
include(cmake/HeadphonesFFmpeg.cmake)

find_package(Threads REQUIRED)
add_library(HeadphonesPreviewEngine STATIC
    src/previewtypes.h
    src/previewoutput.h
    src/previewconverter.h src/previewconverter.cpp
    src/previewreplaygain.h src/previewreplaygain.cpp
    src/headphonedevices.h src/headphonedevices.cpp
    src/pipewirechannelmaps.h src/pipewirechannelmaps.cpp
    src/headphonessettings.h src/headphonessettings.cpp
    src/qtaudiooutput.h src/qtaudiooutput.cpp
    src/previewpcmbuffer.h src/previewpcmbuffer.cpp
    src/previewsource.h src/previewsource.cpp
    src/previewdecoderworker.h src/previewdecoderworker.cpp
    src/previewcontroller.h src/previewcontroller.cpp)
set_target_properties(HeadphonesPreviewEngine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
target_include_directories(HeadphonesPreviewEngine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(HeadphonesPreviewEngine PUBLIC Fooyin::Core Qt6::Core Qt6::Multimedia Threads::Threads
    PRIVATE Headphones::swresample Headphones::avutil)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HEADPHONES_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(HeadphonesPreviewEngine PRIVATE PkgConfig::HEADPHONES_PIPEWIRE)
    target_compile_definitions(HeadphonesPreviewEngine PRIVATE HEADPHONES_HAVE_PIPEWIRE)
endif()
target_compile_definitions(HeadphonesPreviewEngine PUBLIC QT_NO_KEYWORDS)
if(MSVC)
    target_compile_options(HeadphonesPreviewEngine PRIVATE /W4)
else()
    target_compile_options(HeadphonesPreviewEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(HEADPHONES_SOURCES
    src/headphonesplugin.cpp
    src/headphonesplugin.h
    src/headphoneslog.h)
add_library(HeadphonesGui STATIC
    src/headphonesdestinationchoice.h src/headphonesdestinationchoice.cpp
    src/headphonesactions.h src/headphonesactions.cpp
    src/headphonescontrolwidget.h src/headphonescontrolwidget.cpp
    src/headphonesoutputdevice.h src/headphonesoutputdevice.cpp
    src/headphonesplayercontrol.h src/headphonesplayercontrol.cpp
    src/headphonesseekbar.h src/headphonesseekbar.cpp
    src/headphonestrackinfo.h src/headphonestrackinfo.cpp
    src/headphonesvariableprovider.h src/headphonesvariableprovider.cpp
    src/headphonesvolumecontrol.h src/headphonesvolumecontrol.cpp
    src/headphonessettingsdialog.h src/headphonessettingsdialog.cpp)
set_target_properties(HeadphonesGui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
target_link_libraries(HeadphonesGui PUBLIC HeadphonesPreviewEngine Fooyin::Gui Qt6::Widgets)
if(MSVC)
    target_compile_options(HeadphonesGui PRIVATE /W4)
else()
    target_compile_options(HeadphonesGui PRIVATE -Wall -Wextra -Wpedantic)
endif()
set(HEADPHONES_DEPENDS
    HeadphonesGui
    HeadphonesPreviewEngine
    Fooyin::Core Fooyin::Gui Fooyin::Utils
    Qt6::Core Qt6::Widgets Qt6::Multimedia Qt6::Sql
    Headphones::swresample Headphones::avutil)

if(FOOYIN_FORCE_DEV_TREE)
    configure_file(headphones.json.in headphones.json @ONLY)
    add_library(Headphones MODULE ${HEADPHONES_SOURCES})
    target_link_libraries(Headphones PRIVATE ${HEADPHONES_DEPENDS})
    target_include_directories(Headphones PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    set_target_properties(Headphones PROPERTIES
        PREFIX ""
        OUTPUT_NAME fyplugin_headphones
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
    if(APPLE)
        set_property(TARGET Headphones PROPERTY INSTALL_RPATH "@loader_path/../Frameworks")
    elseif(UNIX)
        set_property(TARGET Headphones PROPERTY INSTALL_RPATH "$ORIGIN/..")
    endif()
    install(TARGETS Headphones
        LIBRARY DESTINATION "${FOOYIN_PLUGIN_INSTALL_DIR}"
        RUNTIME DESTINATION "${FOOYIN_PLUGIN_INSTALL_DIR}")
else()
    create_fooyin_plugin(Headphones DEPENDS ${HEADPHONES_DEPENDS} SOURCES ${HEADPHONES_SOURCES})
endif()
target_compile_definitions(Headphones PRIVATE QT_NO_KEYWORDS)
if(MSVC)
    target_compile_options(Headphones PRIVATE /W4)
else()
    target_compile_options(Headphones PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 ${HEADPHONES_QT_MIN_VERSION} REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
