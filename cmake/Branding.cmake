function(rm_configure_branding target)
    set(assets "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../app/assets")
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.remotemanager.RemoteManager"
        MACOSX_BUNDLE_BUNDLE_NAME "CMRemote"
        MACOSX_BUNDLE_ICON_FILE "cmremote.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    if(APPLE)
        target_sources(${target} PRIVATE "${assets}/branding/cmremote.icns")
        set_source_files_properties("${assets}/branding/cmremote.icns" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        # CMake's default plist omits CFBundleDisplayName. Keep the executable
        # and bundle identifier stable while changing both visible names.
        set(plist "${CMAKE_CURRENT_BINARY_DIR}/cmremote-Info.plist.in")
        # Release builds set MACOSX_DEPLOYMENT_TARGET; local builds target the host macOS.
        set(RM_MINIMUM_MACOS_ENTRY "")
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(RM_MINIMUM_MACOS_ENTRY "    <key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
        endif()
        set(content [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>${MACOSX_BUNDLE_EXECUTABLE_NAME}</string>
    <key>CFBundleIdentifier</key><string>${MACOSX_BUNDLE_GUI_IDENTIFIER}</string>
    <key>CFBundleName</key><string>${MACOSX_BUNDLE_BUNDLE_NAME}</string>
    <key>CFBundleDisplayName</key><string>CMRemote</string>
    <key>CFBundleIconFile</key><string>${MACOSX_BUNDLE_ICON_FILE}</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleShortVersionString</key><string>${MACOSX_BUNDLE_SHORT_VERSION_STRING}</string>
    <key>CFBundleVersion</key><string>${MACOSX_BUNDLE_BUNDLE_VERSION}</string>
    <key>NSHighResolutionCapable</key><true/>
@RM_MINIMUM_MACOS_ENTRY@
    <key>NSLocalNetworkUsageDescription</key><string>CMRemote connects to the RDP, SSH and VNC computers you open on your local network.</string>
    <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
]=])
        string(REPLACE "@RM_MINIMUM_MACOS_ENTRY@" "${RM_MINIMUM_MACOS_ENTRY}" content "${content}")
        file(WRITE "${plist}" "${content}")
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${plist}")
    elseif(WIN32)
        # Explorer/taskbar icon and file properties; the executable name stays stable.
        string(REPLACE "." "," numeric "${PROJECT_VERSION}")
        set(rc "${CMAKE_CURRENT_BINARY_DIR}/cmremote.rc")
        file(WRITE "${rc}" "#include <winver.h>
IDI_ICON1 ICON \"${assets}/branding/cmremote.ico\"
VS_VERSION_INFO VERSIONINFO
FILEVERSION ${numeric},0
PRODUCTVERSION ${numeric},0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"FileDescription\", \"CMRemote\"
      VALUE \"ProductName\", \"CMRemote\"
      VALUE \"FileVersion\", \"${PROJECT_VERSION}\"
      VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"
      VALUE \"OriginalFilename\", \"RemoteManager.exe\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
        target_sources(${target} PRIVATE "${rc}")
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(UNIX)
        include(GNUInstallDirs)
        install(FILES "${assets}/cmremote.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            RENAME org.remotemanager.RemoteManager.desktop)
        foreach(size 16 24 32 48 64 128 256 512 1024)
            install(FILES "${assets}/branding/cmremote-${size}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
                RENAME cmremote.png)
        endforeach()
    endif()
endfunction()
