add_executable(bench_dirlisting bench_dirlisting.cpp)
target_link_libraries(bench_dirlisting PRIVATE core Qt5::Core)

add_executable(bench_directory_size bench_directory_size.cpp)
target_link_libraries(bench_directory_size PRIVATE core Qt5::Core)
