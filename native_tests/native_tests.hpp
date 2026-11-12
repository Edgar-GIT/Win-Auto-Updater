#pragma once

// Entry points of the native test bench. The bench is a separate process of the
// same executable: "SingleUpdate.exe --native-tests [--autostart=<minutes>]".
// Keeping it out of the updater's process means a GPU driver reset or a memory
// test holding most of the RAM can never disturb an update in progress.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <filesystem>

namespace nt {

struct LaunchOptions {
    // > 0: start every test automatically for this many minutes; 0 = wait for the user.
    int autostartMinutes = 0;
    // Test aid ("--autostart-seconds=N" together with --autostart): overrides the
    // session length with a number of seconds.
    double autostartSecondsOverride = 0.0;
};

// Runs the bench window until it is closed. Returns the process exit code.
int runNativeTests(HINSTANCE instance, const LaunchOptions& options);

#ifdef SU_UI_SNAPSHOT
int runNativeSelfTest(const std::filesystem::path& reportFile);
int runNativeSnapshot(HINSTANCE instance, const std::filesystem::path& directory);
// Renders a few frames of the GPU scene off-screen into PNG files.
int runGpuSnapshot(const std::filesystem::path& directory);
#endif

}  // namespace nt
