#include "utils/logger.h"

namespace crystal {

static std::shared_ptr<spdlog::logger> g_logger;

void Logger::init(const std::string& name) {
    g_logger = spdlog::stdout_color_mt(name);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    g_logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> Logger::get() {
    if (!g_logger) {
        init();
    }
    return g_logger;
}

void Logger::set_level(spdlog::level::level_enum level) {
    get()->set_level(level);
}

} // namespace crystal
