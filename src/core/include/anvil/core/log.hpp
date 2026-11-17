#pragma once

#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// The numeric values are those of VS Code's LogLevel, which extensions and the extension host use.
enum class LogLevel {
    off = 0,
    trace = 1,
    debug = 2,
    info = 3,
    warning = 4,
    error = 5,
};

std::string_view log_level_name(LogLevel level);

// Receives log messages. `source` names where a message came from, such as a subsystem or an
// extension. Implementations may be called from any thread.
class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(LogLevel level, std::string_view source, std::string_view message) = 0;
};

class NullLogger final : public Logger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

// Writes "[level] source: message" lines for messages at or above a minimum level.
class StreamLogger final : public Logger {
public:
    explicit StreamLogger(std::ostream& stream, LogLevel minimum = LogLevel::info)
        : stream_(stream), minimum_(minimum) {}

    void log(LogLevel level, std::string_view source, std::string_view message) override {
        if (level < minimum_) {
            return;
        }
        const std::lock_guard lock(mutex_);
        stream_ << '[' << log_level_name(level) << "] " << source << ": " << message << '\n';
    }

private:
    std::ostream& stream_;
    LogLevel minimum_;
    std::mutex mutex_;
};

// Keeps every message, for tests.
class RecordingLogger final : public Logger {
public:
    struct Entry {
        LogLevel level;
        std::string source;
        std::string message;
    };

    void log(LogLevel level, std::string_view source, std::string_view message) override {
        const std::lock_guard lock(mutex_);
        entries_.push_back({level, std::string(source), std::string(message)});
    }

    std::vector<Entry> entries() const {
        const std::lock_guard lock(mutex_);
        return entries_;
    }

    // True when some entry has `level`, comes from `source` (any source when empty) and contains
    // `text` in its message.
    bool contains(LogLevel level, std::string_view text, std::string_view source = {}) const {
        const std::lock_guard lock(mutex_);
        for (const Entry& entry : entries_) {
            if (entry.level == level && (source.empty() || entry.source == source) &&
                entry.message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

} // namespace anvil::core
