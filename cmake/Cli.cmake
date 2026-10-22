# Headless infrastructure is shared by the desktop and command-line front ends.
set(VIBE_CORE_SOURCES
    src/services/media/MediaServiceClient.h
    src/database/SessionRepository.cpp
    src/database/SessionTokenCipher.cpp
    src/network/NetworkClient.cpp
    src/network/TlsCertificateStore.cpp
    src/services/media/MediaServerClientBase.cpp
    src/services/emby/EmbyClient.cpp
    src/services/jellyfin/JellyfinClient.cpp
    src/services/credentials/CredentialStore.cpp
    src/services/credentials/NativeCredentialStore.cpp
    src/services/iptv/IptvParser.cpp
    src/services/iptv/IptvProbe.cpp
    src/services/iptv/IptvStreamSelector.cpp
    src/services/iptv/IptvStreamWorkflow.cpp
    src/services/local/LocalMediaService.cpp
    src/services/scheduler/ScheduledPlaybackSchedule.cpp
    src/services/iptv/IptvPlaylistStore.cpp
    src/services/webdav/WebDavClient.cpp
    src/services/webdav/WebDavPlaybackProxy.cpp
    src/services/webdav/WebDavDownloadPlanner.cpp
    src/services/webdav/WebDavUploadPlanner.cpp
    src/services/webdav/TransferManager.cpp
    src/services/webdav/TsslStore.cpp
    src/services/webdav/AesGcmDecryptor.cpp
    src/services/webdav/HlsManifestValidator.cpp
    src/services/webdav/EncryptedHlsPlaybackProxy.cpp
    src/services/encryptedhls/EncryptedHlsPackager.cpp
    src/services/encryptedhls/EncryptedHlsBatchPackager.cpp
    src/services/encryptedhls/EncryptedHlsRestorer.cpp
    src/services/encryptedhls/EncryptedHlsTarContainer.cpp
    src/services/encryptedhls/EncryptedHlsSourcePlanner.cpp
    src/services/ffmpeg/FfmpegCapability.cpp
    src/services/backup/TsslBackupService.cpp
    src/services/backup/TsslBackupArchive.cpp
    src/services/backup/S3CredentialProvider.cpp
    src/services/backup/TsslImportWorkflow.cpp
    src/services/management/ServiceOperations.cpp
    src/services/management/ServiceCardLoader.cpp
    src/services/smb/SmbClient.cpp
    src/services/management/WorkflowSettings.cpp
    src/viewmodels/TransferTaskListModel.cpp
    src/utils/NetworkTrafficCoalescer.cpp
    src/utils/AppLogger.cpp
    src/utils/ErrorPresentation.cpp
    src/utils/JsonUtils.cpp
)
qt_add_library(vibeCore STATIC ${VIBE_CORE_SOURCES})
target_include_directories(vibeCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vibeCore PUBLIC Qt6::Core Qt6::Concurrent Qt6::Network Qt6::Sql Qt6::Xml
    vibeMiniz ${VIBE_CRYPTO_LIBRARIES})
if(WIN32)
    target_link_libraries(vibeCore PUBLIC Advapi32 Mpr)
elseif(APPLE)
    target_link_libraries(vibeCore PUBLIC "-framework Security" "-framework CoreFoundation")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    target_link_libraries(vibeCore PUBLIC PkgConfig::LIBSECRET)
endif()
get_target_property(_desktop_sources vibePlayerQT SOURCES)
foreach(_source IN LISTS VIBE_CORE_SOURCES)
    string(REPLACE ".cpp" ".h" _header "${_source}")
    list(REMOVE_ITEM _desktop_sources "${_source}" "${_header}")
endforeach()
set_property(TARGET vibePlayerQT PROPERTY SOURCES "${_desktop_sources}")
target_link_libraries(vibePlayerQT PRIVATE vibeCore)

qt_add_library(vibeCli STATIC
    src/cli/CliCommand.cpp src/cli/CliConsole.cpp src/cli/CliOutput.cpp src/cli/CliEngine.cpp
    src/cli/CliTssl.cpp src/cli/CliVideo.cpp src/cli/CliPrivacy.cpp src/cli/CliSmb.cpp
    src/cli/CliIpc.cpp src/cli/CliMedia.cpp src/cli/CliDaemon.cpp src/cli/CliTasks.cpp src/cli/CliShell.cpp
    src/cli/CliDesktop.cpp src/cli/CliIptvPlayback.cpp)
target_link_libraries(vibeCli PUBLIC vibeCore)
target_compile_definitions(vibeCli PRIVATE VIBEPLAYER_VERSION="${VIBEPLAYER_SEMVER_FULL}")
qt_add_executable(vibePlayerCLI src/cli/main.cpp)
target_link_libraries(vibePlayerCLI PRIVATE vibeCli)
target_compile_definitions(vibePlayerCLI PRIVATE VIBEPLAYER_VERSION="${VIBEPLAYER_SEMVER_FULL}")
include(GNUInstallDirs)
qt_add_executable(vibePlayerPlayback src/cli/PlaybackMain.cpp src/player/PlayerController.cpp
    src/cli/CliScheduleHost.cpp src/services/scheduler/ScheduledPlaybackManager.cpp)
target_link_libraries(vibePlayerPlayback PRIVATE vibeCli Qt6::Gui ${MPV_LINK_TARGET})
target_compile_definitions(vibePlayerPlayback PRIVATE VIBEPLAYER_MPV_BUILD_INFO="${VIBEPLAYER_MPV_BUILD_INFO}")
if(WIN32)
    set_target_properties(vibePlayerPlayback PROPERTIES WIN32_EXECUTABLE TRUE)
    add_custom_command(TARGET vibePlayerPlayback POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MPV_RUNTIME_DLL}" "$<TARGET_FILE_DIR:vibePlayerPlayback>/libmpv-2.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MPV_VULKAN_RUNTIME_DLL}" "$<TARGET_FILE_DIR:vibePlayerPlayback>/vulkan-1.dll")
elseif(APPLE)
    set_target_properties(vibePlayerPlayback PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
else()
    set_target_properties(vibePlayerPlayback PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
if(WIN32)
    set_target_properties(vibePlayerCLI PROPERTIES OUTPUT_NAME vibePlayerQT SUFFIX ".com" WIN32_EXECUTABLE FALSE
        PDB_NAME vibePlayerCLI ARCHIVE_OUTPUT_NAME vibePlayerCLI)
elseif(APPLE)
    set_target_properties(vibePlayerCLI PROPERTIES OUTPUT_NAME vibePlayerQT-cli INSTALL_RPATH "@executable_path/../Frameworks")
else()
    set_target_properties(vibePlayerCLI PROPERTIES OUTPUT_NAME vibePlayerQT-cli INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
if(APPLE)
    install(TARGETS vibePlayerCLI RUNTIME DESTINATION "vibePlayerQT.app/Contents/MacOS")
    install(TARGETS vibePlayerPlayback RUNTIME DESTINATION "vibePlayerQT.app/Contents/MacOS")
else()
    install(TARGETS vibePlayerCLI RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(TARGETS vibePlayerPlayback RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
