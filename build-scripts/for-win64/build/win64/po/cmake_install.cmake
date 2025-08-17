# Install script for directory: /home/vboxuser/grandorgue/po

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
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
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

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/x86_64-w64-mingw32-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/de/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/de/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/es/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/es/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/fr/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/fr/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/hu/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/hu/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/it/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/it/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/nl/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/nl/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/pl/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/pl/LC_MESSAGES/GrandOrgue.mo")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "resources" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/sv/LC_MESSAGES" TYPE FILE FILES "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/share/locale/sv/LC_MESSAGES/GrandOrgue.mo")
endif()

