#pragma once

#include <filesystem>

class SelfDelete {
public:
    static void cleanTempFiles();
    static bool schedule(const std::filesystem::path& executable);
};
