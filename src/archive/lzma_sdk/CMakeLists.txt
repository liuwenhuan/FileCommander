# Vendored subset of the public-domain LZMA SDK (Igor Pavlov) -- the 7z decoder
# only (LZMA/LZMA2/PPMd/BCJ/Delta + AES/SHA256 primitives). SevenZipReader wraps
# it to preview encrypted / solid 7z archives that libarchive can't handle.
# License: public domain -- see LICENSE-lzma-sdk.txt.
file(GLOB LZMA7Z_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

add_library(lzma7z STATIC ${LZMA7Z_SOURCES})
target_include_directories(lzma7z PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Decode-only, single-threaded use; keep third-party warnings out of our build.
target_compile_definitions(lzma7z PRIVATE Z7_ST)
target_compile_options(lzma7z PRIVATE -w)
