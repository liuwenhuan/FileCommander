if(TARGET LibArchive::LibArchive)
    add_executable(archive_tests
        test_ArchiveHandler.cpp
        test_ArchiveProvider.cpp
        test_ArchiveLocality.cpp
        test_LegacyZipNames.cpp
    )

    target_link_libraries(archive_tests PRIVATE
        archive
        GTest::gtest
        GTest::gtest_main
        Qt5::Core
        Qt5::Gui
    )

    add_test(NAME archive_tests COMMAND archive_tests)
    filecommander_set_test_runtime(archive_tests)
endif()
