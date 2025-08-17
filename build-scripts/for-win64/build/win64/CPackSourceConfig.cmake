# This file will be configured to contain variables for CPack. These variables
# should be set in the CMake list file of the project before CPack module is
# included. The list of available CPACK_xxx variables and their associated
# documentation may be obtained using
#  cpack --help-variable-list
#
# Some variables are common to all generators (e.g. CPACK_PACKAGE_NAME)
# and some are specific to a generator
# (e.g. CPACK_NSIS_EXTRA_INSTALL_COMMANDS). The generator specific variables
# usually begin with CPACK_<GENNAME>_xxxx.


set(CPACK_BUILD_SOURCE_DIRS "/home/vboxuser/grandorgue;/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64")
set(CPACK_CMAKE_GENERATOR "Unix Makefiles")
set(CPACK_COMPONENTS_ALL "Unspecified;resources;demo")
set(CPACK_COMPONENTS_ALL_SET_BY_USER "TRUE")
set(CPACK_COMPONENT_DEMO_DEPENDS "Unspecified")
set(CPACK_COMPONENT_DEMO_DESCRIPTION "This package contains the demo sampleset for GrandOrgue")
set(CPACK_COMPONENT_DEMO_DISPLAY_NAME "GrandOrgue Demo Sampleset")
set(CPACK_COMPONENT_RESOURCES_DESCRIPTION "This package contains the various resource files for GrandOrgue")
set(CPACK_COMPONENT_RESOURCES_DISPLAY_NAME "GrandOrgue Resource Files")
set(CPACK_COMPONENT_UNSPECIFIED_DEPENDS "resources")
set(CPACK_COMPONENT_UNSPECIFIED_DISPLAY_NAME "GrandOrgue")
set(CPACK_COMPONENT_UNSPECIFIED_HIDDEN "TRUE")
set(CPACK_COMPONENT_UNSPECIFIED_REQUIRED "TRUE")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_FILE "/usr/share/cmake-3.25/Templates/CPack.GenericDescription.txt")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "GrandOrgue built using CMake")
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE "ON")
set(CPACK_GENERATOR "7Z;ZIP")
set(CPACK_IGNORE_FILES "/\\.git/;/build/")
set(CPACK_INSTALLED_DIRECTORIES "/home/vboxuser/grandorgue;/")
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INSTALL_PREFIX "/usr/local")
set(CPACK_MODULE_PATH "")
set(CPACK_NSIS_DISPLAY_NAME "GrandOrgue")
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    WriteRegStr HKCR \".organ\" \"\" \"GrandOrgue.odf\"
    WriteRegStr HKCR \".orgue\" \"\" \"GrandOrgue.package\"
    WriteRegStr HKCR \"GrandOrgue.odf\" \"\" \"GrandOrgue organ definition file\"
    WriteRegStr HKCR \"GrandOrgue.odf\\DefaultIcon\" \"\" \"$INSTDIR\\bin\\GrandOrgue.exe,0\"
    WriteRegStr HKCR \"GrandOrgue.odf\\shell\" \"\" \"open\"
    WriteRegStr HKCR \"GrandOrgue.odf\\shell\\open\\command\" \"\" '$INSTDIR\\bin\\GrandOrgue.exe \"%1\"'
    WriteRegStr HKCR \"GrandOrgue.package\" \"\" \"GrandOrgue organ package\"
    WriteRegStr HKCR \"GrandOrgue.package\\DefaultIcon\" \"\" \"$INSTDIR\\bin\\GrandOrgue.exe,0\"
    WriteRegStr HKCR \"GrandOrgue.package\\shell\" \"\" \"open\"
    WriteRegStr HKCR \"GrandOrgue.package\\shell\\open\\command\" \"\" '$INSTDIR\\bin\\GrandOrgue.exe \"%1\"'
  ")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    ReadRegStr $R0 HKCR \".organ\" \"\"
    StrCmp $R0 \"GrandOrgue.odf\" 0 +2
      DeleteRegKey HKCR \".organ\"
    ReadRegStr $R0 HKCR \".orgue\" \"\"
    StrCmp $R0 \"GrandOrgue.package\" 0 +2
      DeleteRegKey HKCR \".orgue\"

    DeleteRegKey HKCR \"GrandOrgue.odf\"
    DeleteRegKey HKCR \"GrandOrgue.package\"
  ")
set(CPACK_NSIS_INSTALLER_ICON_CODE "")
set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_PACKAGE_NAME "GrandOrgue")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(CPACK_OBJCOPY_EXECUTABLE "/usr/bin/x86_64-w64-mingw32-objcopy")
set(CPACK_OBJDUMP_EXECUTABLE "/usr/bin/x86_64-w64-mingw32-objdump")
set(CPACK_OUTPUT_CONFIG_FILE "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/CPackConfig.cmake")
set(CPACK_PACKAGE_CONTACT "osamarin68@gmail.com")
set(CPACK_PACKAGE_DEFAULT_LOCATION "/")
set(CPACK_PACKAGE_DESCRIPTION "GrandOrgue is a virtual pipe organ sample player application")
set(CPACK_PACKAGE_DESCRIPTION_FILE "/usr/share/cmake-3.25/Templates/CPack.GenericDescription.txt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OpenSource Virtual Pipe Organ Software")
set(CPACK_PACKAGE_EXECUTABLES "GrandOrgue;GrandOrgue")
set(CPACK_PACKAGE_FILE_NAME "grandorgue-3.16.0-Source")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/GrandOrgue/grandorgue")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "GrandOrgue")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "GrandOrgue")
set(CPACK_PACKAGE_NAME "grandorgue")
set(CPACK_PACKAGE_RELEASE "0.local")
set(CPACK_PACKAGE_RELOCATABLE "true")
set(CPACK_PACKAGE_VENDOR "GrandOrgue contributors")
set(CPACK_PACKAGE_VERSION "3.16.0")
set(CPACK_PACKAGE_VERSION_MAJOR "3")
set(CPACK_PACKAGE_VERSION_MINOR "16")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_READELF_EXECUTABLE "/usr/bin/x86_64-w64-mingw32-readelf")
set(CPACK_RESOURCE_FILE_LICENSE "/home/vboxuser/grandorgue/LICENSE")
set(CPACK_RESOURCE_FILE_README "/usr/share/cmake-3.25/Templates/CPack.GenericDescription.txt")
set(CPACK_RESOURCE_FILE_WELCOME "/usr/share/cmake-3.25/Templates/CPack.GenericWelcome.txt")
set(CPACK_RPM_PACKAGE_SOURCES "ON")
set(CPACK_SET_DESTDIR "OFF")
set(CPACK_SOURCE_7Z "ON")
set(CPACK_SOURCE_GENERATOR "7Z;ZIP")
set(CPACK_SOURCE_IGNORE_FILES "/\\.git/;/build/")
set(CPACK_SOURCE_INSTALLED_DIRECTORIES "/home/vboxuser/grandorgue;/")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/CPackSourceConfig.cmake")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "grandorgue-3.16.0-Source")
set(CPACK_SOURCE_TOPLEVEL_TAG "windows-Source")
set(CPACK_SOURCE_ZIP "ON")
set(CPACK_STRIP_FILES "")
set(CPACK_SYSTEM_NAME "windows")
set(CPACK_THREADS "1")
set(CPACK_TOPLEVEL_TAG "windows-Source")
set(CPACK_WIX_SIZEOF_VOID_P "8")

if(NOT CPACK_PROPERTIES_FILE)
  set(CPACK_PROPERTIES_FILE "/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/CPackProperties.cmake")
endif()

if(EXISTS ${CPACK_PROPERTIES_FILE})
  include(${CPACK_PROPERTIES_FILE})
endif()
