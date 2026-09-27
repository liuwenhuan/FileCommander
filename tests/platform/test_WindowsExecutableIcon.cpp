#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>

TEST(WindowsExecutableIconTest, HasNativeIconResource) {
    const auto path = std::filesystem::u8path(FILECOMMANDER_TEST_EXECUTABLE_PATH);
    const HMODULE executable = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    ASSERT_NE(executable, nullptr);

    const HRSRC icon = FindResourceW(executable, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14));
    EXPECT_NE(icon, nullptr);
    if (icon) {
        EXPECT_GT(SizeofResource(executable, icon), 0U);
    }

    FreeLibrary(executable);
}
