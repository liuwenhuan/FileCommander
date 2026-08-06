#pragma once

// Prints where a test crashed, because otherwise nothing does.
//
// A crash kills the process between two gtest lines: the log stops mid-test and
// the exit code is all that is left. A suite that crashes for one run in forty
// then reads as an infrastructure hiccup rather than a defect, which is how a
// real one survives.
//
// gtest's own SEH handler on Windows reports "SEH exception with code
// 0xc0000005 thrown in the test body. Stack trace:" and then prints nothing at
// all -- the header without the trace, which is worse than silence because it
// looks like it told you something.
//
// Both platforms are covered here on purpose. The Linux half was written first,
// for a segfault in ui_tests; the Windows half exists because the next crash
// happened to be on Windows, in core_tests, and left no evidence.
//
// Call installCrashReporter() from main() before RUN_ALL_TESTS().

#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
// DbgHelp must follow windows.h.
#include <dbghelp.h>
#elif defined(__GLIBC__)
#include <execinfo.h>
#endif

namespace fc {

namespace detail {

inline void printWho(const char *what) {
    std::cerr << std::endl << "[  crash   ] " << what;
    if (const auto *info = ::testing::UnitTest::GetInstance()->current_test_info())
        std::cerr << " during " << info->test_suite_name() << '.' << info->name();
    std::cerr << std::endl;
    std::cerr.flush();
}

} // namespace detail

#if defined(_WIN32)

// A vectored handler rather than SetUnhandledExceptionFilter: gtest installs an
// SEH filter of its own around each test body, which swallows the exception
// before an unhandled-exception filter would ever see it. Vectored handlers run
// FIRST, ahead of any frame-based handler, so this still gets to speak.
inline LONG CALLBACK crashReporter(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Only the ones that mean "this process is broken". C++ exceptions
    // (0xE06D7363) and debugger breakpoints travel through here too, and
    // reporting those would bury the real thing in noise.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    char what[64];
    std::snprintf(what, sizeof(what), "exception 0x%08lX", static_cast<unsigned long>(code));
    detail::printWho(what);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
        std::cerr << "[  crash   ] " << (kind == 0 ? "read from " : kind == 1 ? "write to "
                                                                             : "execute at ")
                  << reinterpret_cast<void *>(info->ExceptionRecord->ExceptionInformation[1])
                  << std::endl;
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    void *frames[62];
    const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
    // SYMBOL_INFO carries the name inline past the end of the struct, so the
    // buffer has to be big enough for both.
    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;

    for (USHORT i = 0; i < count; ++i) {
        const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
        std::cerr << "[  crash   ] " << frames[i] << "  ";
        if (SymFromAddr(process, address, nullptr, symbol))
            std::cerr << symbol->Name;
        else
            std::cerr << "<no symbol>";
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD displacement = 0;
        if (SymGetLineFromAddr64(process, address, &displacement, &line))
            std::cerr << "  " << line.FileName << ':' << line.LineNumber;
        std::cerr << std::endl;
    }
    std::cerr.flush();
    return EXCEPTION_CONTINUE_SEARCH; // let gtest still record the failure
}

inline void installCrashReporter() {
    AddVectoredExceptionHandler(1 /* call first */, crashReporter);
}

#elif defined(__GLIBC__)

extern "C" inline void crashReporter(int signum) {
    detail::printWho(signum == SIGSEGV ? "SIGSEGV" : signum == SIGABRT ? "SIGABRT" : "signal");
    void *frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fileno(stderr));
    // Re-raise through the default action so the exit status still reports the
    // signal: a handler that returned would turn a crash into a silent pass.
    std::signal(signum, SIG_DFL);
    std::raise(signum);
}

inline void installCrashReporter() {
    std::signal(SIGSEGV, crashReporter);
    std::signal(SIGABRT, crashReporter);
}

#else

inline void installCrashReporter() {}

#endif

} // namespace fc
