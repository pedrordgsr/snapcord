qt_add_executable(snapcord WIN32 MACOSX_BUNDLE
    main.cpp
    AppController.cpp AppController.h
    AttachmentTray.cpp AttachmentTray.h
    Avatar.cpp Avatar.h
    ChannelSidebar.cpp ChannelSidebar.h
    ChatView.cpp ChatView.h
    ConnectionInfoPopup.cpp ConnectionInfoPopup.h
    Demo.cpp Demo.h
    EmojiPicker.cpp EmojiPicker.h
    FriendsView.cpp FriendsView.h
    ImageCache.cpp ImageCache.h
    DiscordVideo.cpp DiscordVideo.h
    ImageViewer.cpp ImageViewer.h
    IncomingCallWindow.cpp IncomingCallWindow.h
    Language.cpp Language.h
    LoginWindow.cpp LoginWindow.h
    MainWindow.cpp MainWindow.h
    MemberListView.cpp MemberListView.h
    MentionPopup.cpp MentionPopup.h
    MessageView.cpp MessageView.h
    Motion.cpp Motion.h
    Notifier.cpp Notifier.h
    RichPresence.cpp RichPresence.h
    ProfileCard.cpp ProfileCard.h
    ProfileEditor.cpp ProfileEditor.h
    ProfilePopup.cpp ProfilePopup.h
    ServerDialogs.cpp ServerDialogs.h
    ServerRail.cpp ServerRail.h
    SettingsDialog.cpp SettingsDialog.h
    Theme.cpp Theme.h
    UpdateDialog.cpp UpdateDialog.h
    UserPanel.cpp UserPanel.h
    VoiceChannelView.cpp VoiceChannelView.h
    VoiceController.cpp VoiceController.h
    VoicePanel.cpp VoicePanel.h
)

set(resource_dir "${PROJECT_SOURCE_DIR}/resources")
qt_add_resources(snapcord "app_resources"
    PREFIX "/"
    BASE "${resource_dir}"
    FILES
        "${resource_dir}/theme/app.qss"
        "${resource_dir}/icons/snapcord.svg"
        "${resource_dir}/icons/home.svg"
        "${resource_dir}/icons/plus.svg"
        "${resource_dir}/icons/hash.svg"
        "${resource_dir}/icons/speaker.svg"
        "${resource_dir}/icons/mic.svg"
        "${resource_dir}/icons/mic-off.svg"
        "${resource_dir}/icons/headphones.svg"
        "${resource_dir}/icons/headphones-off.svg"
        "${resource_dir}/icons/settings.svg"
        "${resource_dir}/icons/disconnect.svg"
        "${resource_dir}/icons/call.svg"
        "${resource_dir}/icons/speaker-active.svg"
        "${resource_dir}/icons/at.svg"
        "${resource_dir}/icons/arrow-right.svg"
        "${resource_dir}/icons/file.svg"
        "${resource_dir}/icons/members.svg"
)

# Separate process: Qt Multimedia (and the FFmpeg/Quick libraries it pulls in) live only here.
# When the window closes, the process exits and that memory is returned to the system.
qt_add_executable(snapcord_video WIN32
    video-main.cpp
    VideoViewer.cpp VideoViewer.h
    Language.cpp Language.h
)
target_link_libraries(snapcord_video PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets)
target_compile_definitions(snapcord_video PRIVATE SNAPCORD_VERSION="${PROJECT_VERSION}")
set_target_properties(snapcord_video PROPERTIES OUTPUT_NAME "snapcord-video")
if(APPLE)
    # Sit next to Snapcord inside the bundle so the main app can launch it.
    set_target_properties(snapcord_video PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:snapcord>")
endif()

# Interface translations. English is the source language; each .ts file adds another language.
qt_add_translations(
    TARGETS snapcord snapcord_video
    TS_FILES "${PROJECT_SOURCE_DIR}/translations/snapcord_pt_BR.ts"
    SOURCE_TARGETS snapcord snapcord_video snapcord_core snapcord_voice snapcord_platform
    RESOURCE_PREFIX "/i18n"
)

target_link_libraries(snapcord PRIVATE
    Qt6::Widgets Qt6::Svg
    snapcord_core snapcord_voice snapcord_platform
    unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
)
target_compile_definitions(snapcord PRIVATE SNAPCORD_VERSION="${PROJECT_VERSION}")

set(packaging_dir "${PROJECT_SOURCE_DIR}/packaging")
set_target_properties(snapcord PROPERTIES OUTPUT_NAME "Snapcord")

if(WIN32)
    # Executable icon and version information.
    set(SNAPCORD_ICO "${packaging_dir}/icons/snapcord.ico")
    configure_file("${packaging_dir}/windows/snapcord.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/snapcord.rc" @ONLY)
    target_sources(snapcord PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/snapcord.rc")
elseif(APPLE)
    # Bundle metadata (including the microphone permission prompt) and icon.
    set(icns "${packaging_dir}/icons/snapcord.icns")
    set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(snapcord PRIVATE "${icns}")
    configure_file("${packaging_dir}/macos/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(snapcord PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
endif()

# Installation layout, used by the Linux packages (AppImage, Flatpak).
include(GNUInstallDirs)
install(TARGETS snapcord snapcord_video
    BUNDLE DESTINATION .
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(UNIX AND NOT APPLE)
    set(app_id "io.github.pedrordgsr.Snapcord")
    install(FILES "${packaging_dir}/linux/${app_id}.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${packaging_dir}/linux/${app_id}.metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    foreach(size 16 32 48 64 128 256 512)
        install(FILES "${packaging_dir}/icons/snapcord-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME "${app_id}.png")
    endforeach()
endif()

# On Windows, copy the Qt DLLs next to the executable so it can be opened with a double click.
if(WIN32)
    get_target_property(qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(qt_bin_dir "${qmake_executable}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${qt_bin_dir}")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET snapcord POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime
                "$<TARGET_FILE:snapcord>"
            VERBATIM)
        add_custom_command(TARGET snapcord_video POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime
                "$<TARGET_FILE:snapcord_video>"
            VERBATIM)
    endif()
endif()
