if(TARGET LibArchive::LibArchive)
    add_executable(archive_tests
        test_ArchiveHandler.cpp
    )

    target_link_libraries(archive_tests PRIVATE
        archive
        gtest
        gtest_main
        Qt5::Core
    )

    add_test(NAME archive_tests COMMAND archive_tests)
endif()
