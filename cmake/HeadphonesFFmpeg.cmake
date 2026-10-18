# Prefer pkg-config (including cross-toolchain configuration). The fallback also
# supports SDKs supplied through CMAKE_PREFIX_PATH, e.g. Windows FFmpeg packages.
find_package(PkgConfig QUIET)
foreach(component swresample avutil)
    if(PkgConfig_FOUND)
        pkg_check_modules(HEADPHONES_${component} QUIET IMPORTED_TARGET "lib${component}")
    endif()
    add_library(Headphones::${component} INTERFACE IMPORTED)
    if(TARGET PkgConfig::HEADPHONES_${component})
        set_property(TARGET Headphones::${component} PROPERTY INTERFACE_LINK_LIBRARIES
            PkgConfig::HEADPHONES_${component})
    else()
        if(component STREQUAL "swresample")
            set(header libswresample/swresample.h)
        else()
            set(header libavutil/avutil.h)
        endif()
        find_path(HEADPHONES_${component}_INCLUDE_DIR NAMES "${header}")
        find_library(HEADPHONES_${component}_LIBRARY NAMES "${component}")
        if(NOT HEADPHONES_${component}_INCLUDE_DIR OR NOT HEADPHONES_${component}_LIBRARY)
            message(FATAL_ERROR
                "Missing FFmpeg lib${component} development files. Install libswresample-dev and libavutil-dev "
                "(Debian/Ubuntu), or provide an FFmpeg SDK using CMAKE_PREFIX_PATH/pkg-config.")
        endif()
        set_target_properties(Headphones::${component} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${HEADPHONES_${component}_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${HEADPHONES_${component}_LIBRARY}")
    endif()
endforeach()
