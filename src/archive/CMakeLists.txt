if(TARGET LibArchive::LibArchive)
    add_library(archive STATIC
        ArchiveNode.h
        ArchiveHandler.h
        ArchiveHandler.cpp
        ArchiveModel.h
        ArchiveModel.cpp
        ArchiveBrowserDialog.h
        ArchiveBrowserDialog.cpp
    )

    target_include_directories(archive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(archive PUBLIC
        Qt5::Widgets
        Qt5::Core
        LibArchive::LibArchive
    )
else()
    message(WARNING "libarchive not found -- archive support disabled")
    add_library(archive INTERFACE)
endif()
