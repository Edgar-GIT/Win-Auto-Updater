#include "logger.hpp"

void Logger::setStatusSink(Sink sink) {
    std::lock_guard lock(mutex_);
    statusSink_ = std::move(sink);
}

void Logger::setLogSink(Sink sink) {
    std::lock_guard lock(mutex_);
    logSink_ = std::move(sink);
}

void Logger::setProgressSink(Sink sink) {
    std::lock_guard lock(mutex_);
    progressSink_ = std::move(sink);
}

void Logger::emit(const Sink& sink, std::wstring_view message) {
    if (sink) {
        sink(message);
    }
}

void Logger::status(std::wstring_view message) {
    Sink sink;
    {
        std::lock_guard lock(mutex_);
        sink = statusSink_;
    }
    emit(sink, message);
}

void Logger::log(std::wstring_view message) {
    Sink sink;
    {
        std::lock_guard lock(mutex_);
        sink = logSink_;
    }
    emit(sink, message);
}

void Logger::progress(std::wstring_view message) {
    Sink sink;
    {
        std::lock_guard lock(mutex_);
        sink = progressSink_;
    }
    emit(sink, message);
}
