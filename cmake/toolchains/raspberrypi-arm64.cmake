set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

if(NOT DEFINED CROSS_ARM64_PREFIX)
  set(CROSS_ARM64_PREFIX "aarch64-linux-gnu-")
endif()

find_program(CMAKE_C_COMPILER NAMES "${CROSS_ARM64_PREFIX}gcc" REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES "${CROSS_ARM64_PREFIX}g++" REQUIRED)
find_program(CMAKE_AR NAMES "${CROSS_ARM64_PREFIX}ar" REQUIRED)
find_program(CMAKE_RANLIB NAMES "${CROSS_ARM64_PREFIX}ranlib" REQUIRED)
find_program(CMAKE_STRIP NAMES "${CROSS_ARM64_PREFIX}strip" REQUIRED)

set(_svx_pkgconfig_libdir
    "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
if(DEFINED CROSS_ARM64_PKG_CONFIG_LIBDIR AND
   NOT CROSS_ARM64_PKG_CONFIG_LIBDIR STREQUAL "")
  set(_svx_pkgconfig_libdir "${CROSS_ARM64_PKG_CONFIG_LIBDIR}")
endif()
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${_svx_pkgconfig_libdir}")

if(DEFINED SVXLINK_SYSROOT AND NOT SVXLINK_SYSROOT STREQUAL "")
  file(TO_CMAKE_PATH "${SVXLINK_SYSROOT}" _svx_sysroot)
  set(CMAKE_SYSROOT "${_svx_sysroot}")
  set(CMAKE_FIND_ROOT_PATH "${_svx_sysroot}")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_svx_sysroot}")
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
else()
  # Debian/Ubuntu multiarch installs target libraries here while sharing
  # architecture-independent headers under /usr/include.
  set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu;/usr/lib/aarch64-linux-gnu")
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

