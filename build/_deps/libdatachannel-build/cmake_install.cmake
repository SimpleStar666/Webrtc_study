# Install script for directory: /workspace/build/_deps/libdatachannel-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdatachannel.so.0.21.2"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdatachannel.so.0.21"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      file(RPATH_CHECK
           FILE "${file}"
           RPATH "")
    endif()
  endforeach()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES
    "/workspace/build/_deps/libdatachannel-build/libdatachannel.so.0.21.2"
    "/workspace/build/_deps/libdatachannel-build/libdatachannel.so.0.21"
    )
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdatachannel.so.0.21.2"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdatachannel.so.0.21"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/workspace/build/_deps/libdatachannel-build/libdatachannel.so")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/rtc" TYPE FILE FILES
    "/workspace/build/_deps/libdatachannel-src/include/rtc/candidate.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/channel.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/configuration.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/datachannel.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/description.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/mediahandler.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtcpreceivingsession.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/common.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/global.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/message.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/frameinfo.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/peerconnection.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/reliability.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtc.h"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtc.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtp.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/track.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/websocket.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/websocketserver.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtppacketizationconfig.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtcpsrreporter.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtppacketizer.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtpdepacketizer.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/h264rtppacketizer.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/h264rtpdepacketizer.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/nalunit.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/h265rtppacketizer.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/h265nalunit.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/av1rtppacketizer.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/nalunit.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/rtcpnackresponder.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/utils.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/plihandler.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/pacinghandler.hpp"
    "/workspace/build/_deps/libdatachannel-src/include/rtc/version.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets.cmake"
         "/workspace/build/_deps/libdatachannel-build/CMakeFiles/Export/32c821eb1e7b36c3a3818aec162f7fd2/LibDataChannelTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel/LibDataChannelTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES "/workspace/build/_deps/libdatachannel-build/CMakeFiles/Export/32c821eb1e7b36c3a3818aec162f7fd2/LibDataChannelTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES "/workspace/build/_deps/libdatachannel-build/CMakeFiles/Export/32c821eb1e7b36c3a3818aec162f7fd2/LibDataChannelTargets-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES "/workspace/build/_deps/libdatachannel-src/cmake/LibDataChannelConfig.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/LibDataChannel" TYPE FILE FILES "/workspace/build/LibDataChannelConfigVersion.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/workspace/build/_deps/libdatachannel-build/examples/client/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/client-benchmark/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/media-receiver/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/media-sender/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/media-sfu/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/streamer/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/copy-paste/cmake_install.cmake")
  include("/workspace/build/_deps/libdatachannel-build/examples/copy-paste-capi/cmake_install.cmake")

endif()

