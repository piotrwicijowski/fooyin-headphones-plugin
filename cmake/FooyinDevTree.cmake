# Only public source headers and generated export headers are exposed here.
message(STATUS "Using fooyin development tree: ${FOOYIN_BUILD_DIR}")
foreach(component core gui utils)
    if(NOT EXISTS "${FOOYIN_SRC_DIR}/include/${component}"
       OR NOT EXISTS "${FOOYIN_BUILD_DIR}/src/${component}/fy${component}_export.h")
        message(FATAL_ERROR
            "Incomplete fooyin development tree for ${component}. Build a matching fooyin first, "
            "then set FOOYIN_SRC_DIR and FOOYIN_BUILD_DIR (currently ${FOOYIN_BUILD_DIR}).")
    endif()
endforeach()

set(_fooyin_library_paths
    "${FOOYIN_BUILD_DIR}/run/${CMAKE_INSTALL_LIBDIR}/fooyin"
    "${FOOYIN_BUILD_DIR}/run/lib/fooyin"
    "${FOOYIN_BUILD_DIR}/run/lib64/fooyin"
    "${FOOYIN_BUILD_DIR}/run/fooyin.app/Contents/Frameworks"
    "${FOOYIN_BUILD_DIR}/run/bin"
    "${FOOYIN_BUILD_DIR}/run")
foreach(component Utils Core Gui)
    string(TOLOWER "${component}" lower)
    # Do not retain a cached library from a different FOOYIN_BUILD_DIR override.
    unset(_fooyin_library CACHE)
    unset(_fooyin_library)
    find_library(_fooyin_library NAMES "fooyin_${lower}"
        PATHS ${_fooyin_library_paths} "${FOOYIN_BUILD_DIR}/src/${lower}"
        PATH_SUFFIXES "" Debug Release RelWithDebInfo NO_DEFAULT_PATH)
    if(NOT _fooyin_library)
        message(FATAL_ERROR "Cannot find fooyin_${lower} in ${FOOYIN_BUILD_DIR}. Build fooyin first.")
    endif()
    add_library(Fooyin::${component} SHARED IMPORTED)
    if(WIN32)
        unset(_fooyin_dll CACHE)
        unset(_fooyin_dll)
        find_file(_fooyin_dll NAMES "fooyin_${lower}.dll" "libfooyin_${lower}.dll"
            PATHS ${_fooyin_library_paths} PATH_SUFFIXES "" Debug Release RelWithDebInfo NO_DEFAULT_PATH)
        if(NOT _fooyin_dll)
            message(FATAL_ERROR "Cannot find fooyin_${lower} runtime DLL in ${FOOYIN_BUILD_DIR}.")
        endif()
        set_target_properties(Fooyin::${component} PROPERTIES
            IMPORTED_IMPLIB "${_fooyin_library}" IMPORTED_LOCATION "${_fooyin_dll}")
    else()
        set_property(TARGET Fooyin::${component} PROPERTY IMPORTED_LOCATION "${_fooyin_library}")
    endif()
    set_target_properties(Fooyin::${component} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
        "${FOOYIN_SRC_DIR}/include;${FOOYIN_BUILD_DIR}/src/core;${FOOYIN_BUILD_DIR}/src/gui;${FOOYIN_BUILD_DIR}/src/utils")
endforeach()
set_property(TARGET Fooyin::Utils PROPERTY INTERFACE_LINK_LIBRARIES "Qt6::Core;Qt6::Sql")
set_property(TARGET Fooyin::Core PROPERTY INTERFACE_LINK_LIBRARIES
    "Fooyin::Utils;Qt6::Concurrent;Qt6::Network")
set_property(TARGET Fooyin::Gui PROPERTY INTERFACE_LINK_LIBRARIES "Fooyin::Core;Qt6::Widgets")

if(APPLE)
    set(_headphones_plugin_install_dir "fooyin.app/Contents/PlugIns")
elseif(WIN32)
    set(_headphones_plugin_install_dir plugins)
else()
    set(_headphones_plugin_install_dir "${CMAKE_INSTALL_LIBDIR}/fooyin/plugins")
endif()
set(FOOYIN_PLUGIN_INSTALL_DIR "${_headphones_plugin_install_dir}" CACHE PATH "fooyin plugin install directory")
