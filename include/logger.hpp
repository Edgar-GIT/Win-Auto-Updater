#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

class Logger {
public:
    using Sink = std::function<void(std::wstring_view)>;

    void setStatusSink(Sink sink);
    void setLogSink(Sink sink);
    void setProgressSink(Sink sink);

    void status(std::wstring_view message);
    void log(std::wstring_view message);
    void progress(std::wstring_view message);

private:
    std::mutex mutex_;
    Sink statusSink_;
    Sink logSink_;
    Sink progressSink_;

    void emit(const Sink& sink, std::wstring_view message);
};
