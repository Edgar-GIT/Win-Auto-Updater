#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

class Logger {
public:
    using Sink = std::function<void(std::wstring_view)>;

    void setSink(Sink sink);
    void status(std::wstring_view message);

private:
    std::mutex mutex_;
    Sink sink_;
};
