#pragma once

// Entry points of the native tests. They are a separate process of the same
// executable:
//
//   SingleUpdate.exe --native-tests --auto     every test at once, by itself
//   SingleUpdate.exe --native-tests --manual   one window per test, by hand
//
// Keeping them out of the updater's process means a GPU driver reset or a
// memory test holding most of the RAM can never disturb an update in progress.

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
    // true: automatic session (dashboard); false: the separate test windows.
    bool automatic = false;
    // "--keep-resolution": leave the display mode alone (by default the
    // graphics test switches the screen to its highest resolution).
    bool keepDisplayMode = false;
    // "--test-seconds=N": length of a test on the test clock (0 = five minutes).
    double testSeconds = 0.0;
};

// Runs the native tests until their windows are closed. Returns the exit code.
int runNativeTests(HINSTANCE instance, const LaunchOptions& options);

#ifdef SU_UI_SNAPSHOT
int runNativeSelfTest(const std::filesystem::path& reportFile);
int runNativeSnapshot(HINSTANCE instance, const std::filesystem::path& directory);
// Renders a few frames of the GPU scene off-screen into PNG files.
int runGpuSnapshot(const std::filesystem::path& directory);
#endif

}  // namespace nt
