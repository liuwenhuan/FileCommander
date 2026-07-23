if(TARGET LibArchive::LibArchive)
    # Vendored public-domain LZMA SDK (7z decoder) for encrypted/solid 7z preview.
    add_subdirectory(lzma_sdk)

    add_library(archive STATIC
        ArchiveNode.h
        ArchiveHandler.h
        ArchiveHandler.cpp
        PackageInfo.h
        PackageInfo.cpp
        SevenZipReader.h
        SevenZipReader.cpp
        SquashfsReader.h
        SquashfsReader.cpp
        ExternalArchiveTool.h
        ExternalArchiveTool.cpp
        ArchiveModel.h
        ArchiveModel.cpp
        ArchiveLayout.h
        ArchiveLayout.cpp
        ArchiveProvider.h
        ArchiveProvider.cpp
        ArchiveBrowserDialog.h
        ArchiveBrowserDialog.cpp
        CompressDialog.h
        CompressDialog.cpp
    )

    target_include_directories(archive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(archive PUBLIC
        core
        widgets
        Qt5::Widgets
        Qt5::Core
        LibArchive::LibArchive
    )
    # lzma7z (vendored 7z decoder) is an internal implementation detail of
    # SevenZipReader; nettle provides AES-256/SHA-256 for the encryption glue.
    target_link_libraries(archive PRIVATE lzma7z nettle)
else()
    message(WARNING "libarchive not found -- archive support disabled")
    add_library(archive INTERFACE)
endif()
