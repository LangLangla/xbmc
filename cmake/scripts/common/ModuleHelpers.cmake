# This script provides helper functions for FindModules

# Parse and set variables from VERSION dependency file
# Arguments:
#   module_name name of the library (currently must match tools/depends/target/${module_name})
# On return:
#   ARCHIVE will be set to parent scope
#   MODULENAME_VER will be set to parent scope (eg FFMPEG_VER, DAV1D_VER)
#   MODULENAME_BASE_URL will be set to parent scope if exists in VERSION file (eg FFMPEG_BASE_URL)
function(get_archive_name module_name)
  string(TOUPPER ${module_name} UPPER_MODULE_NAME)

  # Dependency path
  set(MODULE_PATH "${CMAKE_SOURCE_DIR}/tools/depends/target/${module_name}")
  if(NOT EXISTS "${MODULE_PATH}/${UPPER_MODULE_NAME}-VERSION")
    MESSAGE(FATAL_ERROR "${UPPER_MODULE_NAME}-VERSION does not exist at ${MODULE_PATH}.")
  else()
    set(${UPPER_MODULE_NAME}_FILE "${MODULE_PATH}/${UPPER_MODULE_NAME}-VERSION")
  endif()

  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_LNAME REGEX "^[ \t]*LIBNAME=")
  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_VER REGEX "^[ \t]*VERSION=")
  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_ARCHIVE REGEX "^[ \t]*ARCHIVE=")
  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_BASE_URL REGEX "^[ \t]*BASE_URL=")
  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_BYPRODUCT REGEX "^[ \t]*BYPRODUCT=")

  # Tarball Hash
  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_HASH_SHA256 REGEX "^[ \t]*SHA256=")
  file(STRINGS ${${UPPER_MODULE_NAME}_FILE} ${UPPER_MODULE_NAME}_HASH_SHA256 REGEX "^[ \t]*SHA512=")

  string(REGEX REPLACE ".*LIBNAME=([^ \t]*).*" "\\1" ${UPPER_MODULE_NAME}_LNAME "${${UPPER_MODULE_NAME}_LNAME}")
  string(REGEX REPLACE ".*VERSION=([^ \t]*).*" "\\1" ${UPPER_MODULE_NAME}_VER "${${UPPER_MODULE_NAME}_VER}")
  string(REGEX REPLACE ".*ARCHIVE=([^ \t]*).*" "\\1" ${UPPER_MODULE_NAME}_ARCHIVE "${${UPPER_MODULE_NAME}_ARCHIVE}")
  string(REGEX REPLACE ".*BASE_URL=([^ \t]*).*" "\\1" ${UPPER_MODULE_NAME}_BASE_URL "${${UPPER_MODULE_NAME}_BASE_URL}")
  string(REGEX REPLACE ".*BYPRODUCT=([^ \t]*).*" "\\1" ${UPPER_MODULE_NAME}_BYPRODUCT "${${UPPER_MODULE_NAME}_BYPRODUCT}")

  string(REGEX REPLACE "\\$\\(LIBNAME\\)" "${${UPPER_MODULE_NAME}_LNAME}" ${UPPER_MODULE_NAME}_ARCHIVE "${${UPPER_MODULE_NAME}_ARCHIVE}")
  string(REGEX REPLACE "\\$\\(VERSION\\)" "${${UPPER_MODULE_NAME}_VER}" ${UPPER_MODULE_NAME}_ARCHIVE "${${UPPER_MODULE_NAME}_ARCHIVE}")

  set(${UPPER_MODULE_NAME}_ARCHIVE ${${UPPER_MODULE_NAME}_ARCHIVE} PARENT_SCOPE)

  if(${UPPER_MODULE_NAME}_BYPRODUCT)
    set(${UPPER_MODULE_NAME}_LIBRARY ${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/lib/${${UPPER_MODULE_NAME}_BYPRODUCT} PARENT_SCOPE)
  endif()
  set(${UPPER_MODULE_NAME}_INCLUDE_DIR ${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/include PARENT_SCOPE)
  set(${UPPER_MODULE_NAME}_VER ${${UPPER_MODULE_NAME}_VER} PARENT_SCOPE)

  if (${UPPER_MODULE_NAME}_BASE_URL)
    set(${UPPER_MODULE_NAME}_BASE_URL ${${UPPER_MODULE_NAME}_BASE_URL} PARENT_SCOPE)
  else()
    set(${UPPER_MODULE_NAME}_BASE_URL "http://mirrors.kodi.tv/build-deps/sources" PARENT_SCOPE)
  endif()
  set(${UPPER_MODULE_NAME}_BYPRODUCT ${${UPPER_MODULE_NAME}_BYPRODUCT} PARENT_SCOPE)

  if (${UPPER_MODULE_NAME}_HASH_SHA256)
    set(${UPPER_MODULE_NAME}_HASH ${${UPPER_MODULE_NAME}_HASH_SHA256} PARENT_SCOPE)
  elseif(${UPPER_MODULE_NAME}_HASH_SHA512)
    set(${UPPER_MODULE_NAME}_HASH ${${UPPER_MODULE_NAME}_HASH_SHA512} PARENT_SCOPE)
  endif()
endfunction()

# Macro to factor out the repetitive URL setup
macro(SETUP_BUILD_VARS)
  get_archive_name(${MODULE_LC})
  string(TOUPPER ${MODULE_LC} MODULE)

  # allow user to override the download URL with a local tarball
  # needed for offline build envs
  if(${MODULE}_URL)
    get_filename_component(${MODULE}_URL "${${MODULE}_URL}" ABSOLUTE)
  else()
    set(${MODULE}_URL ${${MODULE}_BASE_URL}/${${MODULE}_ARCHIVE})
  endif()
  if(VERBOSE)
    message(STATUS "${MODULE}_URL: ${${MODULE}_URL}")
  endif()

  # unset all build_dep_target variables to insure clean state
  unset(CMAKE_ARGS)
  unset(PATCH_COMMAND)
  unset(CONFIGURE_COMMAND)
  unset(BUILD_COMMAND)
  unset(INSTALL_COMMAND)
  unset(BUILD_IN_SOURCE)
  unset(BUILD_BYPRODUCTS)
endmacro()

# Macro to create externalproject_add target
# 
# Common usage
#
# CMAKE_ARGS: cmake(required)
# PATCH_COMMAND: ALL(optional)
# CONFIGURE_COMMAND: autoconf(required), meson(required)
# BUILD_COMMAND: autoconf(required), meson(required), cmake(optional)
# INSTALL_COMMAND: autoconf(required), meson(required), cmake(optional)
# BUILD_IN_SOURCE: ALL(optional)
# BUILD_BYPRODUCTS: ALL(optional)
#
macro(BUILD_DEP_TARGET)
  include(ExternalProject)

  if(CMAKE_ARGS)
    set(CMAKE_ARGS CMAKE_ARGS ${CMAKE_ARGS})
    if(CMAKE_TOOLCHAIN_FILE)
      list(APPEND CMAKE_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
  endif()

  if(PATCH_COMMAND)
    set(PATCH_COMMAND PATCH_COMMAND ${PATCH_COMMAND})
  endif()

  if(CONFIGURE_COMMAND)
    set(CONFIGURE_COMMAND CONFIGURE_COMMAND ${CONFIGURE_COMMAND})
  endif()

  if(BUILD_COMMAND)
    set(BUILD_COMMAND BUILD_COMMAND ${BUILD_COMMAND})
  endif()

  if(INSTALL_COMMAND)
    set(INSTALL_COMMAND INSTALL_COMMAND ${INSTALL_COMMAND})
  endif()

  if(BUILD_IN_SOURCE)
    set(BUILD_IN_SOURCE BUILD_IN_SOURCE ${BUILD_IN_SOURCE})
  endif()

  if(BUILD_BYPRODUCTS)
    set(BUILD_BYPRODUCTS BUILD_BYPRODUCTS ${BUILD_BYPRODUCTS})
  else()
    if(${MODULE}_BYPRODUCT)
      set(BUILD_BYPRODUCTS BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/lib/${${MODULE}_BYPRODUCT})
    endif()
  endif()

  externalproject_add(${MODULE_LC}
                      URL ${${MODULE}_URL}
                      URL_HASH ${${MODULE}_HASH}
                      DOWNLOAD_DIR ${TARBALL_DIR}
                      DOWNLOAD_NAME ${${MODULE}_ARCHIVE}
                      PREFIX ${CORE_BUILD_DIR}/${MODULE_LC}
                      ${CMAKE_ARGS}
                      ${PATCH_COMMAND}
                      ${CONFIGURE_COMMAND}
                      ${BUILD_COMMAND}
                      ${INSTALL_COMMAND}
                      ${BUILD_BYPRODUCTS}
                      ${BUILD_IN_SOURCE})

  set_target_properties(${MODULE_LC} PROPERTIES FOLDER "External Projects")
endmacro()
