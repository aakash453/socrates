# FindQNN.cmake
# Locates the Qualcomm QNN SDK for Hexagon NPU acceleration.
#
# Requires QNN_SDK_ROOT to be set (environment or CMake variable).
#
# Provides:
#   QNN_FOUND           — TRUE if SDK found
#   QNN_INCLUDE_DIRS    — QNN include directories
#   QNN_LIBRARIES       — QNN libraries (QnnHtp, QnnSystem, QnnCpu)
#   QNN_VERSION         — detected SDK version

if(NOT DEFINED QNN_SDK_ROOT)
  set(QNN_SDK_ROOT "$ENV{QNN_SDK_ROOT}" CACHE PATH "Qualcomm QNN SDK root directory")
endif()

if(NOT QNN_SDK_ROOT)
  set(QNN_FOUND FALSE)
  return()
endif()

set(QNN_INCLUDE_DIRS "${QNN_SDK_ROOT}/include/QNN"
    "${QNN_SDK_ROOT}/include")

find_library(QNN_HTP_LIBRARY
  NAMES QnnHtp QnnHtpV75 QnnHtpV73
  HINTS "${QNN_SDK_ROOT}/lib/aarch64-android"
        "${QNN_SDK_ROOT}/lib/arm64x-windows-msvc"
        "${QNN_SDK_ROOT}/lib/x86_64-windows-msvc"
        "${QNN_SDK_ROOT}/lib"
  NO_DEFAULT_PATH)

find_library(QNN_SYSTEM_LIBRARY
  NAMES QnnSystem
  HINTS "${QNN_SDK_ROOT}/lib/aarch64-android"
        "${QNN_SDK_ROOT}/lib/arm64x-windows-msvc"
        "${QNN_SDK_ROOT}/lib/x86_64-windows-msvc"
        "${QNN_SDK_ROOT}/lib"
  NO_DEFAULT_PATH)

find_library(QNN_CPU_LIBRARY
  NAMES QnnCpu QnnCPU
  HINTS "${QNN_SDK_ROOT}/lib/aarch64-android"
        "${QNN_SDK_ROOT}/lib/arm64x-windows-msvc"
        "${QNN_SDK_ROOT}/lib/x86_64-windows-msvc"
        "${QNN_SDK_ROOT}/lib"
  NO_DEFAULT_PATH)

if(QNN_HTP_LIBRARY)
  set(QNN_LIBRARIES ${QNN_HTP_LIBRARY})
  # Only add .lib import libraries to the linker, never .dll files
  if(QNN_SYSTEM_LIBRARY)
    get_filename_component(_qnn_sys_ext "${QNN_SYSTEM_LIBRARY}" LAST_EXT)
    if(_qnn_sys_ext STREQUAL ".lib")
      list(APPEND QNN_LIBRARIES ${QNN_SYSTEM_LIBRARY})
    endif()
  endif()
  if(QNN_CPU_LIBRARY)
    get_filename_component(_qnn_cpu_ext "${QNN_CPU_LIBRARY}" LAST_EXT)
    if(_qnn_cpu_ext STREQUAL ".lib")
      list(APPEND QNN_LIBRARIES ${QNN_CPU_LIBRARY})
    endif()
  endif()

  # Detect version from SDK directory name or version.txt
  if(EXISTS "${QNN_SDK_ROOT}/version.txt")
    file(READ "${QNN_SDK_ROOT}/version.txt" QNN_VERSION_FILE)
    string(STRIP "${QNN_VERSION_FILE}" QNN_VERSION)
  else()
    get_filename_component(QNN_DIR_NAME "${QNN_SDK_ROOT}" NAME)
    if(QNN_DIR_NAME MATCHES "([0-9]+\\.[0-9]+\\.[0-9]+)")
      set(QNN_VERSION "${CMAKE_MATCH_1}")
    else()
      set(QNN_VERSION "unknown")
    endif()
  endif()

  set(QNN_FOUND TRUE)
  message(STATUS "QNN SDK found: ${QNN_SDK_ROOT} (${QNN_VERSION})")
else()
  set(QNN_FOUND FALSE)
  message(WARNING "QNN SDK not found in ${QNN_SDK_ROOT}. Set QNN_SDK_ROOT to the SDK path.")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(QNN
  REQUIRED_VARS QNN_SDK_ROOT QNN_INCLUDE_DIRS QNN_HTP_LIBRARY
  VERSION_VAR QNN_VERSION)
