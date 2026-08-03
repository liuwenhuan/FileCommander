set(PLATFORM_SOURCES
    PlatformResult.h
    privilege/PrivilegedFileOperation.h
    privilege/PrivilegedFileOperation.cpp
    privilege/PrivilegeBroker.h
    CredentialStore.h
    PathSemantics.h
    PathSemantics.cpp
    TrashService.h
    FeatureCapabilities.h
    FeatureCapabilities.cpp
    FolderArguments.h
    FolderArguments.cpp
    InstanceCoordinator.h
    InstanceCoordinator.cpp
    TerminalLauncher.h
    TerminalLauncher.cpp
)
if(WIN32)
    list(APPEND PLATFORM_SOURCES
        windows/WindowsTrashService.cpp
        windows/WindowsCredentialStore.cpp
        windows/WindowsPrivilegeBroker.cpp
    )
else()
    list(APPEND PLATFORM_SOURCES
        linux/LinuxTrashService.cpp
        linux/LibsecretCredentialStore.cpp
        linux/LinuxPrivilegeBroker.cpp
    )
endif()

add_library(platform STATIC ${PLATFORM_SOURCES})
target_include_directories(platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform PUBLIC Qt5::Core Qt5::Network)
if(WIN32)
    target_link_libraries(platform PRIVATE shell32 ole32 Advapi32)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET GLOBAL libsecret-1)
    target_link_libraries(platform PRIVATE PkgConfig::LIBSECRET)
endif()
