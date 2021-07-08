include(FindPackageHandleStandardArgs)
find_package(PkgConfig)

if(PKGCONFIG_FOUND)
  pkg_check_modules(BULLET bullet)
endif()

find_package_handle_standard_args(BULLET
                                  REQUIRED_VARS BULLET_LIBRARIES BULLET_INCLUDE_DIRS BULLET_LIBRARY_DIRS
                                  VERSION_VAR BULLET_VERSION)
