find_package(Qt6 REQUIRED COMPONENTS Test)

if(WIN32)
    get_target_property(qt_test_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(qt_test_bin_dir "${qt_test_qmake}" DIRECTORY)
    get_filename_component(qt_test_root "${qt_test_bin_dir}" DIRECTORY)
endif()

function(snapcord_add_test name source)
    qt_add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN})
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
    add_test(NAME ${name} COMMAND ${name})
    if(WIN32)
        # windeployqt relocates Qt beside the app, but does not deploy QtTest or the offscreen plugin.
        set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${qt_test_bin_dir};QT_PLUGIN_PATH=path_list_prepend:${qt_test_root}/plugins")
    endif()
endfunction()

snapcord_add_test(snapcord_tests VoicePipelineTest.cpp snapcord_core snapcord_voice ZLIB::ZLIB Opus::opus)
snapcord_add_test(snapcord_markdown_tests MarkdownTest.cpp snapcord_core)
snapcord_add_test(snapcord_member_list_tests MemberListTest.cpp snapcord_core)
snapcord_add_test(snapcord_game_detector_tests GameDetectorTest.cpp snapcord_core)
snapcord_add_test(snapcord_client_properties_tests ClientPropertiesTest.cpp snapcord_core)
snapcord_add_test(snapcord_composer_tests ComposerTest.cpp snapcord_core)
snapcord_add_test(snapcord_guild_folders_tests GuildFoldersTest.cpp snapcord_core)
snapcord_add_test(snapcord_update_checker_tests UpdateCheckerTest.cpp snapcord_core)

# Exercise the real composer and channel switching with an offline session.
set(chat_draft_sources ChatDraftTest.cpp)
foreach(component AttachmentTray Avatar ChatView DiscordVideo EmojiPicker ImageCache ImageViewer MemberListView
                  MentionPopup MessageView Motion Theme VoiceController)
    list(APPEND chat_draft_sources "${PROJECT_SOURCE_DIR}/src/app/${component}.cpp"
                                  "${PROJECT_SOURCE_DIR}/src/app/${component}.h")
endforeach()
snapcord_add_test(snapcord_chat_draft_tests "${chat_draft_sources}"
                 Qt6::Widgets Qt6::Svg snapcord_core snapcord_voice snapcord_platform)
set_tests_properties(snapcord_chat_draft_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
