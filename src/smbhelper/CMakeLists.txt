# The SMB read helper. Deliberately links libsmbclient and nothing else -- no
# Qt, no ttc libraries -- so a crash inside libsmbclient can only take down this
# process, and so the parent shares no address space with it.
#
# pkg_check_modules is repeated here rather than reused from src/core: it sets
# its variables in the calling directory's scope, which does not reach sibling
# subdirectories.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SMBCLIENT REQUIRED smbclient)

include(GNUInstallDirs)

add_executable(ttc-smb-helper main.cpp)

target_include_directories(ttc-smb-helper PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/network   # SmbHelperProtocol.h (Qt-free by design)
    ${SMBCLIENT_INCLUDE_DIRS}
)

target_link_libraries(ttc-smb-helper PRIVATE ${SMBCLIENT_LIBRARIES})

# Land beside the ttc binary in the build tree, because SmbHelperClient looks
# for the helper next to the running executable first -- so a development build
# uses its own helper rather than whatever is installed system-wide.
# AUTOMOC is on project-wide; this target has no Qt at all, so turn it off
# rather than run moc over a translation unit that can never need it.
set_target_properties(ttc-smb-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    AUTOMOC OFF)

install(TARGETS ttc-smb-helper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
