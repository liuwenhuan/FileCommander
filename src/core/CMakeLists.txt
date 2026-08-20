set(CORE_SOURCES
    diagnostics/RuntimeCounters.h
    diagnostics/RuntimeCounters.cpp
    filesystem/FileInfo.h
    filesystem/FileInfo.cpp
    filesystem/FileSystemModel.h
    filesystem/FileSystemModel.cpp
    filesystem/DirectorySizeTask.h
    filesystem/DirectorySizeTask.cpp
    filesystem/FileProvider.h
    filesystem/ProviderPath.h
    filesystem/ProviderPath.cpp
    filesystem/LocalFileProvider.h
    filesystem/LocalFileProvider.cpp
    filesystem/ComputerCatalog.h
    filesystem/ComputerCatalog.cpp
    filesystem/ComputerProvider.h
    filesystem/ComputerProvider.cpp
    filesystem/IconCache.h
    filesystem/IconCache.cpp
    theme/Phosphor.h
    theme/Phosphor.cpp
    # Lives here, not in src/ui, because both the QuickView preview pane (ui)
    # and the F4 editor (viewer) must agree on what a file's encoding is, and
    # viewer sits BELOW ui in the layer order -- it cannot include from it.
    # The class itself is QtCore-only and has no UI dependency.
    text/TextEncodingDetector.h
    text/TextEncodingDetector.cpp
    filesystem/ExternalPaths.h
    filesystem/ExternalPaths.cpp
    operations/FileOpTypes.h
    operations/OperationError.h
    operations/OperationError.cpp
    operations/FileOperations.h
    operations/FileOperations.cpp
    operations/OperationQueue.h
    operations/OperationQueue.cpp
    config/Settings.h
    config/Settings.cpp
    config/PrivatePath.h
    config/PrivatePath.cpp
    config/SessionManager.h
    config/SessionManager.cpp
    sync/DirectorySync.h
    sync/DirectorySync.cpp
    sync/SyncScanner.h
    sync/SyncScanner.cpp
    compare/TextDiff.h
    compare/TextDiff.cpp
    media/Id3Reader.h
    media/Id3Reader.cpp
    notepad/NotepadStore.h
    notepad/NotepadStore.cpp
    network/NetworkSession.h
    network/NetworkSession.cpp
    update/UpdateChecker.h
    update/UpdateChecker.cpp
)

if(FILECOMMANDER_ENABLE_NETWORK)
    list(APPEND CORE_SOURCES
        network/ConnectionPool.h
        network/SftpProvider.h
        network/SftpProvider.cpp
        network/CurlFtpProvider.h
        network/CurlFtpProvider.cpp
        network/CurlWebDavProvider.h
        network/CurlWebDavProvider.cpp
        network/TransferErrorMapping.h
        network/TransferErrorMapping.cpp
        network/ConnectionStore.h
        network/ConnectionStore.cpp
        account/AccountClient.h
        account/AccountClient.cpp
        account/DeviceAgent.h
        account/DeviceAgent.cpp
        account/FileShareServer.h
        account/FileShareServer.cpp
        account/ShareIdentity.h
        account/ShareIdentity.cpp
        account/RelayTunnel.h
        account/RelayTunnel.cpp
    )
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    list(APPEND CORE_SOURCES
        network/GvfsMounter.h
        network/GvfsMounter.cpp
        network/SmbClientGate.h
        network/SmbProvider.h
        network/SmbProvider.cpp
        network/SmbHelperProtocol.h
        network/SmbHelperClient.h
        network/SmbHelperClient.cpp
        devices/RemovableDeviceMonitor.h
        devices/RemovableDeviceMonitor.cpp
    )
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION OR (WIN32 AND FILECOMMANDER_ENABLE_NETWORK))
    list(APPEND CORE_SOURCES
        network/SmbHostBrowser.h
        network/SmbHostBrowser.cpp
        network/WsdDiscovery.h
        network/WsdDiscovery.cpp
        network/MdnsDiscovery.h
        network/LanScan.h
    )
    if(WIN32)
        list(APPEND CORE_SOURCES
            network/MdnsDiscovery_windows.cpp
            network/LanScan_windows.cpp
        )
    else()
        list(APPEND CORE_SOURCES
            network/MdnsDiscovery.cpp
            network/LanScan.cpp
        )
    endif()
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION OR WIN32)
    list(APPEND CORE_SOURCES
    )
endif()

if(WIN32)
    list(APPEND CORE_SOURCES
        devices/RemovableDeviceMonitor.h
        devices/RemovableDeviceMonitor_windows.cpp
        devices/WindowsRemovableStorage.h
        devices/WindowsRemovableStorage.cpp
    )
    if(FILECOMMANDER_ENABLE_NETWORK)
        list(APPEND CORE_SOURCES
            network/WindowsSmbSession.h
            network/WindowsSmbSession.cpp
            network/WindowsSmbProvider.h
            network/WindowsSmbProvider.cpp
        )
    endif()
endif()

add_library(core STATIC ${CORE_SOURCES})

if(FILECOMMANDER_ENABLE_NETWORK)
    if(WIN32)
        find_package(libssh2 CONFIG REQUIRED)
        find_package(CURL CONFIG REQUIRED)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
        pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    endif()
    # ShareIdentity only: Qt5 cannot generate a keypair, so the device's
    # self-signed share certificate is made with the OpenSSL C API.
    find_package(OpenSSL REQUIRED)
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SMBCLIENT REQUIRED IMPORTED_TARGET smbclient)
endif()

target_include_directories(core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/filesystem
    ${CMAKE_CURRENT_SOURCE_DIR}/operations
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/sync
    ${CMAKE_CURRENT_SOURCE_DIR}/compare
    ${CMAKE_CURRENT_SOURCE_DIR}/media
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/devices
    ${CMAKE_CURRENT_SOURCE_DIR}/notepad
    ${CMAKE_CURRENT_SOURCE_DIR}/update
    ${CMAKE_CURRENT_SOURCE_DIR}/account
    ${CMAKE_CURRENT_SOURCE_DIR}/text
)

target_link_libraries(core PUBLIC
    platform
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Concurrent
    Qt5::Network
)

if(WIN32)
    target_link_libraries(core PUBLIC Advapi32 Shell32)
endif()

if(FILECOMMANDER_ENABLE_NETWORK)
    target_link_libraries(core PUBLIC Qt5::WebSockets)
    target_link_libraries(core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(core PUBLIC libssh2::libssh2 CURL::libcurl ws2_32 Mpr Netapi32)
    else()
        target_link_libraries(core PUBLIC PkgConfig::LIBSSH2 PkgConfig::CURL)
    endif()
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    target_link_libraries(core PUBLIC
        Qt5::DBus
        PkgConfig::SMBCLIENT
    )
endif()
