set(PLATFORM_SOURCES
    PlatformResult.h
    PathSemantics.h
    PathSemantics.cpp
    TrashService.h
    FeatureCapabilities.h
    FeatureCapabilities.cpp
)
if(WIN32)
    list(APPEND PLATFORM_SOURCES windows/WindowsTrashService.cpp)
else()
    list(APPEND PLATFORM_SOURCES linux/LinuxTrashService.cpp)
endif()

add_library(platform STATIC ${PLATFORM_SOURCES})
target_include_directories(platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform PUBLIC Qt5::Core)
if(WIN32)
    target_link_libraries(platform PRIVATE shell32 ole32)
endif()
