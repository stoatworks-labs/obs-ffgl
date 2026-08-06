cmake_minimum_required(VERSION 3.21)

project(obs-ffgl VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- third-party header trees -------------------------------------------------
#
# Fetched by scripts/bootstrap.sh rather than vendored or submoduled; the
# reasoning is in that script. Failing loudly here is much kinder than three
# hundred lines of "no such file: obs-module.h".
set(OBS_TREE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-studio)
set(SIMDE_TREE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/simde)
set(OBS_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-generated)
set(OXBOW ${CMAKE_CURRENT_SOURCE_DIR}/third_party/oxbow)

if(NOT EXISTS ${OBS_TREE}/libobs/obs-module.h)
  message(FATAL_ERROR "libobs headers missing. Run ./scripts/bootstrap.sh first.")
endif()
if(NOT EXISTS ${OXBOW}/src/ffgl/ffgl_host.h)
  message(FATAL_ERROR "oxbow submodule missing. Run:\n"
                      "  git submodule update --init --recursive")
endif()
if(NOT EXISTS ${OXBOW}/third_party/ffgl/source/lib/ffgl/FFGL.h)
  message(FATAL_ERROR "The FFGL SDK is a submodule *of oxbow*. Run:\n"
                      "  git submodule update --init --recursive")
endif()

# --- the module ---------------------------------------------------------------
#
# MODULE, not SHARED: on macOS this has to be a loadable bundle for dlopen, and
# on every platform OBS loads it rather than links it.
add_library(obs-ffgl MODULE
  src/module.cpp
  src/ffgl_catalog.cpp
  src/ffgl_effect.cpp
  src/gl_state_guard.cpp

  # oxbow's FFGL host, compiled from the submodule rather than copied. It is
  # the canonical implementation; the fleet has been bitten before by a second
  # copy quietly becoming the real one.
  ${OXBOW}/src/ffgl/ffgl_host.cpp
  ${OXBOW}/src/core/dylib.cpp
)

target_include_directories(obs-ffgl PRIVATE
  src
  ${OXBOW}/src
  ${OXBOW}/third_party/ffgl/source/lib
  ${OBS_TREE}/libobs
  ${OBS_GENERATED}
  ${SIMDE_TREE}
)

set_target_properties(obs-ffgl PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-ffgl"
)

if(APPLE)
  # Link straight against the libobs inside the user's own OBS.app. Its install
  # name is @rpath/libobs.framework/Versions/A/libobs, and OBS's *executable*
  # carries the matching @executable_path/../Frameworks rpath — so the plugin
  # needs no rpath of its own, exactly like the shipping third-party plugins.
  set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS.app to link libobs from")
  set(LIBOBS ${OBS_APP}/Contents/Frameworks/libobs.framework/Versions/A/libobs)
  if(NOT EXISTS ${LIBOBS})
    message(FATAL_ERROR "No libobs at ${LIBOBS}. Set -DOBS_APP=/path/to/OBS.app")
  endif()

  target_compile_definitions(obs-ffgl PRIVATE GL_SILENCE_DEPRECATION)
  target_link_libraries(obs-ffgl PRIVATE ${LIBOBS} "-framework OpenGL")

  # OBS resolves a module's data (locale, and anything obs_module_file asks
  # for) to Contents/Resources inside the bundle. MACOSX_PACKAGE_LOCATION puts
  # them there as part of the build, so the built bundle is already complete
  # and `cmake --install` has nothing extra to do.
  file(GLOB_RECURSE OBS_FFGL_LOCALE ${CMAKE_CURRENT_SOURCE_DIR}/data/locale/*.ini)
  target_sources(obs-ffgl PRIVATE ${OBS_FFGL_LOCALE})
  set_source_files_properties(${OBS_FFGL_LOCALE} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/locale")

  set_target_properties(obs-ffgl PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "plugin"
    MACOSX_BUNDLE_BUNDLE_NAME "obs-ffgl"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.obs-ffgl"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  )
endif()

# --- install ------------------------------------------------------------------
#
# OBS scans this directory on start-up. There is no packaging step: copy the
# bundle in and restart OBS.
if(APPLE)
  set(OBS_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
  # A MODULE target needs LIBRARY DESTINATION even when BUNDLE is set — CMake
  # refuses to configure without it, and then only uses BUNDLE.
  install(TARGETS obs-ffgl
          BUNDLE DESTINATION "${OBS_PLUGIN_DIR}"
          LIBRARY DESTINATION "${OBS_PLUGIN_DIR}")
endif()
