// ============================================================================
// logger.cpp - 日志工具实现
// ============================================================================
//
// 本文件实现了 Logger 类，基于 spdlog 框架提供日志功能。
//
// 【实现要点】
// 1. 全局日志器指针
//    - 使用 static 局部变量 g_logger 存储日志器实例
//    - 整个进程共享同一个日志器实例
//
// 2. 惰性初始化
//    - get() 方法检查 g_logger 是否为空
//    - 如果为空则自动调用 init() 创建默认日志器
//    - 这确保了即使没有手动调用 init()，日志系统也能正常工作
//
// 3. stdout_color_mt
//    - 创建一个多线程安全的带颜色控制台日志器
//    - "_mt" 后缀表示 multi-threaded（线程安全）
//    - 对应的 "_st" 后缀表示 single-threaded（单线程，性能更好但不安全）
//
// 4. 日志格式
//    - [%Y-%m-%d %H:%M:%S.%e] - 时间戳，精确到毫秒
//    - [%n] - 日志器名称（如 "crystal"）
//    - [%^%l%$] - 日志级别，%^ 和 %$ 标记着色范围
//    - %v - 实际日志消息
//    - 示例输出：[2024-01-15 10:30:45.123] [crystal] [INFO] Server started
//
// ============================================================================

#include "utils/logger.h"

namespace crystal {

// 全局日志器实例
// 使用 static 变量确保整个进程只有一个日志器实例
static std::shared_ptr<spdlog::logger> g_logger;

// 初始化日志系统
// 参数：name - 日志器名称，默认为 "crystal"
void Logger::init(const std::string& name) {
    // 创建带颜色的多线程安全控制台日志器
    // stdout_color_mt 内部使用 stdout_sink_mt（多线程安全的标准输出 Sink）
    // 日志级别会根据终端支持自动着色（如 ERROR 红色、WARN 黄色等）
    g_logger = spdlog::stdout_color_mt(name);

    // 设置日志输出格式
    // 各字段含义：
    //   %Y-%m-%d %H:%M:%S.%e - 日期时间（毫秒精度）
    //   %n - 日志器名称
    //   %^%l%$ - 日志级别（%^ 和 %$ 之间的内容会被着色显示）
    //   %v - 用户日志消息
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

    // 设置默认日志级别为 debug
    // 开发阶段使用 debug 级别可以看到所有调试信息
    // 生产环境应设置为 info 或 warn 以减少日志量
    g_logger->set_level(spdlog::level::debug);

    // 将此日志器设置为 spdlog 的默认日志器
    // 这样 spdlog::info() 等全局函数也会使用此日志器
    spdlog::set_default_logger(g_logger);
}

// 获取日志器实例
// 实现惰性初始化：如果日志器尚未创建，自动调用 init()
std::shared_ptr<spdlog::logger> Logger::get() {
    if (!g_logger) {
        init(); // 使用默认参数 "crystal" 初始化
    }
    return g_logger;
}

// 设置日志级别
// 参数：level - 目标日志级别
// 常用级别：spdlog::level::trace/debug/info/warn/err/critical/off
void Logger::set_level(spdlog::level::level_enum level) {
    get()->set_level(level);
}

} // namespace crystal
