# FindLibAndroidJNI
# -------
# Finds the LibAndroidJNI library
#
# This will define the following target:
#
#   ${APP_NAME_LC}::LibAndroidJNI   - The LibAndroidJNI library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  include(cmake/scripts/common/ModuleHelpers.cmake)

  set(${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC libandroidjni)

  SETUP_BUILD_VARS()

  set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_BUILD_TYPE Release)

  # We still need to supply SOMETHING to CMAKE_ARGS to initiate a cmake BUILD_DEP_TARGET
  # Setting cmake_build_type twice wont cause issues
  set(CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release)

  set(patches "${CORE_SOURCE_DIR}/tools/depends/target/${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}/001-fix-intent-getstringextra-signature.patch")
  generate_patchcommand("${patches}")
  unset(patches)

  BUILD_DEP_TARGET()

  SETUP_BUILD_TARGET()

  add_dependencies(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_BUILD_NAME})

  set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES
                                                                   FOLDER "External Projects")
endif()
