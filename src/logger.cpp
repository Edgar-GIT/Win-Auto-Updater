#include "logger.hpp"

void Logger::setSink(Sink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::status(std::wstring_view message) {
    Sink sink;
    {
        std::lock_guard lock(mutex_);
        sink = sink_;
    }
    if (sink) {
        sink(message);
    }
}
