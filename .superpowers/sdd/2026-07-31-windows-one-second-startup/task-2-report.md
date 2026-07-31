# Task 2 Report: Lazy Embedded Quick View

## Status

Implementation complete and focused behavior tests pass. The required Windows Release startup benchmark executed all seven launches, but did not meet the configured startup budget.

## Changed Files

- `src/ui/MainWindow.h`
- `src/ui/MainWindow.cpp`
- `tests/ui/test_MainWindowPreviewSwap.cpp`

`MainWindow::ensureQuickView()` now owns first construction of the embedded preview, its preview signals, media warm timer, and initial content typography. Ctrl+Q and package preview smoke explicitly ensure it. Null-aware warm-timer and active-preview paths preserve teardown and theme-safe behavior while no preview has been created.

## RED

The first normal build command failed before compiling the test because the shell did not have the MSVC standard-library environment:

```powershell
cmake --build build\windows-mf-debug --target ui_tests --config Debug
```

```text
fatal error C1083: cannot open include file: 'cstddef'
```

With the Visual Studio Developer environment, the failing test built and failed for the expected eager-construction reason:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && set "GTEST_FILTER=MainWindowPreviewSwapTest.FirstQuickViewUseCreatesOneInstanceWithCurrentTypography" && ctest --test-dir build\windows-mf-debug -R ui_tests --output-on-failure'
```

```text
Value of: window.findChildren<QuickView *>().isEmpty()
  Actual: false
Expected: true
[  FAILED  ] MainWindowPreviewSwapTest.FirstQuickViewUseCreatesOneInstanceWithCurrentTypography
```

## GREEN

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build\windows-mf-debug --target ui_tests --config Debug'
```

```text
[8/8] Linking CXX executable tests\ui\ui_tests.exe
```

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && set "GTEST_FILTER=QuickViewLazyPages.*:MainWindowPreviewSwapTest.*:MainWindowStartupTest.*" && ctest --test-dir build\windows-mf-debug -R ui_tests --output-on-failure'
```

```text
1/1 Test #2: ui_tests ... Passed 17.88 sec
100% tests passed, 0 tests failed out of 1
```

The Release executable was rebuilt with:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build\windows-msvc-release --target FileCommander --config Release'
```

## Seven-Launch Windows Release Benchmark

```powershell
$env:PATH = 'C:\Users\deepin\AppData\Local\FileCommanderSDK\Qt\5.15.2\msvc2019_64\bin;C:\Users\deepin\AppData\Local\FileCommanderSDK\vcpkg\installed\x64-windows\bin;C:\Users\deepin\AppData\Local\FileCommanderSDK\poppler-qt5\bin;' + $env:PATH
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\measure-windows-startup.ps1 -ExecutablePath .\build\windows-msvc-release\FileCommander.exe
```

| Run | visibleMs | panelsLoadedMs | interactiveMs |
|---:|---:|---:|---:|
| 1 | 1414 | 1453 | 1642 |
| 2 | 1489 | 1527 | 1645 |
| 3 | 1449 | 1504 | 1694 |
| 4 | 1415 | 1453 | 1578 |
| 5 | 1438 | 1488 | 1621 |
| 6 | 1414 | 1461 | 1587 |
| 7 | 1462 | 1506 | 1640 |

```text
interactiveMs median=1640 p90=1694 max=1694
Windows startup budget exceeded: median=1640 p90=1694 max=1694
```

## Commit

Implementation commit: `b072a341bbdeebd890e22b321571d608d28a62cb` (`perf: create embedded preview on first use`).

## Self-Review And Concerns

- `ensureQuickView()` is idempotent, applies the current settings before first preview use, and retains one owned object across swaps.
- The original download-cancel, stream-failure, media-warmed, and media-failed connections moved with creation; the package smoke path explicitly ensures the preview.
- `startupReady` and `--startup-probe` behavior were not modified.
- Theme refresh already checks `m_quickView`; active-preview and media-timer use sites now also tolerate a null preview/timer.
- Concern: the seven-run Release startup measurement exceeds the `1000/1200/1500 ms` median/p90/max budget. This task removes the embedded `QuickView` from construction, but the measured full startup remains above the plan threshold and needs follow-up outside Task 2 scope.

## Fix Round 1

### Settings Isolation

The typography test previously wrote the shared persistent `Settings` values `Arial` and `15` without restoring them. `ScopedTypographySettingsRestore` is a test-only RAII guard that records the family and size and restores both in its destructor, including on `ASSERT_*` early return or exception stack unwinding.

RED command:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build\windows-mf-debug --target ui_tests --config Debug && set "GTEST_FILTER=MainWindowPreviewSwapTest.TypographySettingsDoNotLeakPastTestScope" && ctest --test-dir build\windows-mf-debug -R ui_tests --output-on-failure'
```

RED output:

```text
after.globalFontFamily(): Task2QuickViewTypographySentinel
originalFamily: Arial
after.listFontSize(): 14
originalSize: 15
[  FAILED  ] MainWindowPreviewSwapTest.TypographySettingsDoNotLeakPastTestScope
```

GREEN command:

```powershell
cmd.exe /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build\windows-mf-debug --target ui_tests --config Debug && set "GTEST_FILTER=MainWindowPreviewSwapTest.FirstQuickViewUseCreatesOneInstanceWithCurrentTypography:MainWindowPreviewSwapTest.TypographySettingsDoNotLeakPastTestScope" && ctest --test-dir build\windows-mf-debug -R ui_tests --output-on-failure'
```

```text
1/1 Test #2: ui_tests ... Passed 0.43 sec
100% tests passed, 0 tests failed out of 1
```

The full focused suite was then rerun successfully:

```text
GTEST_FILTER=QuickViewLazyPages.*:MainWindowPreviewSwapTest.*:MainWindowStartupTest.*
1/1 Test #2: ui_tests ... Passed 16.81 sec
100% tests passed, 0 tests failed out of 1
```

### Base/Head Startup Comparison

The `1000/1200/1500 ms` median/p90/max budget is the Task 6 final acceptance threshold, not a standalone Task 2 pass/fail condition. No threshold or production scope was changed in this fix.

Base `8b91a63` and Head `b072a34` were each built in a fresh detached temporary worktree with the same Release CMake configuration: MSVC 19.44, Ninja, Qt 5.15.2, vcpkg x64-windows, Poppler Qt5, and `FILECOMMANDER_MEDIA_BACKEND=auto`. Each executable was measured with the same `scripts/measure-windows-startup.ps1` command and the same prefixed Qt/vcpkg/Poppler runtime `PATH`. Both temporary worktrees were removed after measurement.

| Run | Base visible | Head visible | Delta | Base loaded | Head loaded | Delta | Base interactive | Head interactive | Delta |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1095 | 1101 | +6 | 1137 | 1144 | +7 | 1288 | 1279 | -9 |
| 2 | 1089 | 1064 | -25 | 1131 | 1108 | -23 | 1283 | 1231 | -52 |
| 3 | 1053 | 1075 | +22 | 1093 | 1115 | +22 | 1232 | 1238 | +6 |
| 4 | 1047 | 1068 | +21 | 1093 | 1112 | +19 | 1239 | 1233 | -6 |
| 5 | 1079 | 1035 | -44 | 1123 | 1087 | -36 | 1263 | 1219 | -44 |
| 6 | 1073 | 1063 | -10 | 1120 | 1108 | -12 | 1259 | 1237 | -22 |
| 7 | 1026 | 1054 | +28 | 1078 | 1098 | +20 | 1215 | 1223 | +8 |

`Delta = Head - Base`, in milliseconds. The comparable `interactiveMs` median improved from `1259 ms` to `1233 ms` (`-26 ms`); p90/max improved from `1288 ms` to `1279 ms` (`-9 ms`). The result is an observed Task 2 improvement, while both samples remain above the final Task 6 budget.

### Fix Commit

`1253c645fead1f98b8ea08aa06d68a33cc31cda8` (`test: isolate QuickView typography settings`).
