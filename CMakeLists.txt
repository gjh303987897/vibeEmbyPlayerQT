cmake_minimum_required(VERSION 3.24)

project(vibePlayerQT
    VERSION 1.0.1
    DESCRIPTION "Cross-platform Qt Quick media center"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Qml
    Quick
    QuickControls2
    Network
    Sql
    Widgets
    Xml
)

if(WIN32)
    set(MPV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/dev" CACHE PATH "libmpv development package root")
    set(MPV_INCLUDE_DIR "${MPV_ROOT}/include")
    set(MPV_LIBRARY "${MPV_ROOT}/libmpv.dll.a")
    set(MPV_RUNTIME_DLL "${MPV_ROOT}/libmpv-2.dll")

    if(NOT EXISTS "${MPV_INCLUDE_DIR}/mpv/client.h")
        message(FATAL_ERROR "libmpv headers not found at ${MPV_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${MPV_LIBRARY}")
        message(FATAL_ERROR "libmpv import library not found at ${MPV_LIBRARY}")
    endif()

    add_library(MPV::MPV UNKNOWN IMPORTED)
    set_target_properties(MPV::MPV PROPERTIES
        IMPORTED_LOCATION "${MPV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    set(MPV_LINK_TARGET PkgConfig::MPV)
endif()

if(WIN32)
    set(MPV_LINK_TARGET MPV::MPV)
endif()

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(vibePlayerQT
    src/app/main.cpp
    src/app/TrayController.cpp
    src/app/TrayController.h
    src/app/WindowAppearanceController.cpp
    src/app/WindowAppearanceController.h
    src/database/SessionRepository.cpp
    src/database/SessionRepository.h
    src/models/IptvChannel.h
    src/models/IptvPlaylist.h
    src/models/DailyUsageStat.h
    src/models/MediaItem.h
    src/models/MediaLibrary.h
    src/models/MediaPerson.h
    src/models/ScheduledPlaybackTask.h
    src/models/ServiceCard.h
    src/models/ServerConfig.h
    src/models/ServiceType.h
    src/models/TransferTask.h
    src/models/UserSession.h
    src/models/WebDavItem.h
    src/network/NetworkClient.cpp
    src/network/NetworkClient.h
    src/network/NetworkError.h
    src/player/MpvWidget.cpp
    src/player/MpvWidget.h
    src/player/MpvVideoItem.cpp
    src/player/MpvVideoItem.h
    src/player/PlayerController.cpp
    src/player/PlayerController.h
    src/services/emby/EmbyClient.cpp
    src/services/emby/EmbyClient.h
    src/services/credentials/CredentialStore.cpp
    src/services/credentials/CredentialStore.h
    src/services/iptv/IptvParser.cpp
    src/services/iptv/IptvParser.h
    src/services/jellyfin/JellyfinClient.cpp
    src/services/jellyfin/JellyfinClient.h
    src/services/media/MediaServiceClient.h
    src/services/media/MediaServerClientBase.cpp
    src/services/media/MediaServerClientBase.h
    src/services/scheduler/ScheduledPlaybackManager.cpp
    src/services/scheduler/ScheduledPlaybackManager.h
    src/services/scheduler/ScheduledPlaybackSchedule.cpp
    src/services/scheduler/ScheduledPlaybackSchedule.h
    src/services/webdav/WebDavDownloadPlanner.cpp
    src/services/webdav/WebDavDownloadPlanner.h
    src/services/webdav/TransferManager.cpp
    src/services/webdav/TransferManager.h
    src/services/webdav/WebDavClient.cpp
    src/services/webdav/WebDavClient.h
    src/services/webdav/WebDavPlaybackProxy.cpp
    src/services/webdav/WebDavPlaybackProxy.h
    src/settings/AppSettings.h
    src/utils/AppLogger.cpp
    src/utils/AppLogger.h
    src/utils/JsonUtils.cpp
    src/utils/JsonUtils.h
    src/viewmodels/AppViewModel.cpp
    src/viewmodels/AppViewModel.h
    src/viewmodels/DailyUsageStatsListModel.cpp
    src/viewmodels/DailyUsageStatsListModel.h
    src/viewmodels/IptvChannelListModel.cpp
    src/viewmodels/IptvChannelListModel.h
    src/viewmodels/MediaItemListModel.cpp
    src/viewmodels/MediaItemListModel.h
    src/viewmodels/MediaLibraryListModel.cpp
    src/viewmodels/MediaLibraryListModel.h
    src/viewmodels/PersonListModel.cpp
    src/viewmodels/PersonListModel.h
    src/viewmodels/ServiceCardListModel.cpp
    src/viewmodels/ServiceCardListModel.h
    src/viewmodels/ScheduledPlaybackTaskListModel.cpp
    src/viewmodels/ScheduledPlaybackTaskListModel.h
    src/viewmodels/TransferTaskListModel.cpp
    src/viewmodels/TransferTaskListModel.h
    src/viewmodels/WebDavItemListModel.cpp
    src/viewmodels/WebDavItemListModel.h
)

qt_add_qml_module(vibePlayerQT
    URI VibePlayer
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

qt_add_resources(vibePlayerQT "app_icons"
    PREFIX "/app/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/icon"
    FILES
        icon/icon_black.png
        icon/icon_white.png
)

if(WIN32)
    target_sources(vibePlayerQT PRIVATE icon/app_icon.rc)
    target_include_directories(vibePlayerQT PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/icon")
endif()

if(APPLE)
    set_source_files_properties(icon/app_icon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(vibePlayerQT PRIVATE icon/app_icon.icns)
    set_target_properties(vibePlayerQT PROPERTIES
        MACOSX_BUNDLE_ICON_FILE app_icon.icns
    )
endif()

target_include_directories(vibePlayerQT PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)

target_link_libraries(vibePlayerQT PRIVATE
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
    Qt6::Xml
    ${MPV_LINK_TARGET}
)

if(WIN32)
    target_link_libraries(vibePlayerQT PRIVATE dwmapi Advapi32)
endif()

if(WIN32)
    add_custom_command(TARGET vibePlayerQT POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:vibePlayerQT>/libmpv-2.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::Xml>"
            "$<TARGET_FILE_DIR:vibePlayerQT>/$<TARGET_FILE_NAME:Qt6::Xml>"
        VERBATIM
    )
endif()

if(WIN32)
    set_target_properties(vibePlayerQT PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    set_target_properties(vibePlayerQT PROPERTIES MACOSX_BUNDLE TRUE)
endif()

include(GNUInstallDirs)

if(UNIX AND NOT APPLE)
    set_target_properties(vibePlayerQT PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
endif()

install(TARGETS vibePlayerQT
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
    install(FILES "${MPV_RUNTIME_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(UNIX AND NOT APPLE)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/vibePlayerQT.desktop.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vibePlayerQT.desktop"
        @ONLY
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibePlayerQT.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon_black.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps"
        RENAME vibePlayerQT.png
    )
endif()

set(VIBEPLAYER_DEPLOY_OPTIONS NO_TRANSLATIONS)
if(UNIX AND NOT APPLE)
    list(APPEND VIBEPLAYER_DEPLOY_OPTIONS
        POST_EXCLUDE_REGEXES
            [[^/lib.*/ld-linux.*]]
            [[^/lib.*/libc\.so.*]]
            [[^/lib.*/libm\.so.*]]
            [[^/lib.*/libpthread\.so.*]]
            [[^/lib.*/libdl\.so.*]]
            [[^/lib.*/librt\.so.*]]
            [[^/lib.*/libresolv\.so.*]]
            [[^/lib.*/libutil\.so.*]]
            [[^/usr/lib.*/libc\.so.*]]
            [[^/usr/lib.*/libm\.so.*]]
            [[^/usr/lib.*/libpthread\.so.*]]
            [[^/usr/lib.*/libdl\.so.*]]
            [[^/usr/lib.*/librt\.so.*]]
            [[^/usr/lib.*/libresolv\.so.*]]
            [[^/usr/lib.*/libutil\.so.*]]
    )
endif()

qt_generate_deploy_qml_app_script(
    TARGET vibePlayerQT
    OUTPUT_SCRIPT VIBEPLAYER_DEPLOY_SCRIPT
    ${VIBEPLAYER_DEPLOY_OPTIONS}
)
install(SCRIPT ${VIBEPLAYER_DEPLOY_SCRIPT})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    add_executable(webdav_download_planner_test
        tests/WebDavDownloadPlannerTest.cpp
        src/services/webdav/WebDavDownloadPlanner.cpp
        src/services/webdav/WebDavDownloadPlanner.h
        src/services/webdav/WebDavClient.cpp
        src/services/webdav/WebDavClient.h
        src/utils/AppLogger.cpp
        src/utils/AppLogger.h
    )
    target_include_directories(webdav_download_planner_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(webdav_download_planner_test PRIVATE
        Qt6::Network
        Qt6::Test
        Qt6::Xml
    )

    add_test(NAME webdav_download_planner COMMAND webdav_download_planner_test)
    set_tests_properties(webdav_download_planner PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(transfer_manager_test
        tests/TransferManagerTest.cpp
        src/services/webdav/TransferManager.cpp
        src/services/webdav/TransferManager.h
        src/viewmodels/TransferTaskListModel.cpp
        src/viewmodels/TransferTaskListModel.h
        src/utils/AppLogger.cpp
        src/utils/AppLogger.h
    )
    target_include_directories(transfer_manager_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(transfer_manager_test PRIVATE
        Qt6::Network
        Qt6::Test
    )

    add_test(NAME transfer_manager COMMAND transfer_manager_test)
    set_tests_properties(transfer_manager PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(emby_client_search_test
        tests/EmbyClientSearchTest.cpp
        src/network/NetworkClient.cpp
        src/network/NetworkClient.h
        src/services/emby/EmbyClient.cpp
        src/services/emby/EmbyClient.h
        src/services/media/MediaServerClientBase.cpp
        src/services/media/MediaServerClientBase.h
        src/services/media/MediaServiceClient.h
        src/utils/JsonUtils.cpp
        src/utils/JsonUtils.h
    )
    target_include_directories(emby_client_search_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(emby_client_search_test PRIVATE
        Qt6::Network
        Qt6::Test
    )

    add_test(NAME emby_client_search COMMAND emby_client_search_test)
    set_tests_properties(emby_client_search PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(scheduled_playback_schedule_test
        tests/ScheduledPlaybackScheduleTest.cpp
        src/services/scheduler/ScheduledPlaybackSchedule.cpp
        src/services/scheduler/ScheduledPlaybackSchedule.h
    )
    target_include_directories(scheduled_playback_schedule_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(scheduled_playback_schedule_test PRIVATE
        Qt6::Test
    )

    add_test(NAME scheduled_playback_schedule COMMAND scheduled_playback_schedule_test)
    set_tests_properties(scheduled_playback_schedule PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()
