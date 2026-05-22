#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

namespace crystal {

class Logger {
public:
    static void init(const std::string& name = "crystal");
    static std::shared_ptr<spdlog::logger> get();

    static void trace(const std::string& msg) { get()->trace(msg); }
    static void debug(const std::string& msg) { get()->debug(msg); }
    static void info(const std::string& msg) { get()->info(msg); }
    static void warn(const std::string& msg) { get()->warn(msg); }
    static void error(const std::string& msg) { get()->error(msg); }

    template<typename... Args>
    static void trace(fmt::format_string<Args...> fmt, Args&&... args) {
        get()->trace(fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void debug(fmt::format_string<Args...> fmt, Args&&... args) {
        get()->debug(fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void info(fmt::format_string<Args...> fmt, Args&&... args) {
        get()->info(fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void warn(fmt::format_string<Args...> fmt, Args&&... args) {
        get()->warn(fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void error(fmt::format_string<Args...> fmt, Args&&... args) {
        get()->error(fmt, std::forward<Args>(args)...);
    }

    static void set_level(spdlog::level::level_enum level);
};

} // namespace crystal
