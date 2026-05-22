// ============================================================================
// logger.h - 日志工具
// ============================================================================
//
// 本文件定义了 CrystalRTC 的日志工具类 Logger，基于 spdlog 日志框架实现。
//
// 【spdlog 日志框架简介】
// spdlog 是一个高性能的 C++ 日志库，具有以下特点：
// 1. 极快的性能：使用异步日志和零拷贝格式化
// 2. 头文件为主：大部分代码在头文件中，编译期优化
// 3. 灵活的 Sink 机制：可以同时输出到控制台、文件、网络等
// 4. 线程安全：支持多线程并发写日志
// 5. 格式化支持：使用 fmt 库进行高效的格式化输出
//
// 【spdlog 核心概念】
// 1. Logger（日志器）
//    - 每个Logger 有一个名称和一组 Sink
//    - 本项目创建名为 "crystal" 的日志器
//
// 2. Sink（输出目标）
//    - 定义日志的输出位置和格式
//    - 本项目使用 stdout_color_sink（带颜色的控制台输出）
//    - 其他常用 Sink：basic_file_sink（文件）、rotating_file_sink（滚动文件）
//
// 3. 日志级别
//    - trace: 最详细的跟踪信息，通常只在开发调试时使用
//    - debug: 调试信息，用于开发阶段的问题定位
//    - info: 一般信息，记录程序正常运行的关键事件
//    - warn: 警告信息，表示可能的问题但不影响运行
//    - error: 错误信息，表示操作失败但程序可以继续
//    - critical: 严重错误，可能导致程序崩溃
//    - off: 关闭所有日志
//
// 4. 格式化模式
//    - 本项目使用：[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v
//    - %Y-%m-%d %H:%M:%S.%e: 时间戳（精确到毫秒）
//    - %n: 日志器名称
//    - %^%l%$: 日志级别（%^ 和 %$ 之间的内容会着色）
//    - %v: 日志消息内容
//
// ============================================================================

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

namespace crystal {

// ============================================================================
// Logger - 日志工具类
// ============================================================================
//
// 封装 spdlog 的静态工具类，提供全局统一的日志接口。
//
// 设计思路：
// - 使用静态方法，无需创建实例即可使用
// - 惰性初始化：首次调用 get() 时自动初始化默认日志器
// - 提供两种日志接口：
//   1. 简单字符串版本：Logger::info("message")
//   2. 格式化版本：Logger::info("value={}", value)
// - 格式化版本使用 fmt 库的格式化语法，类型安全且高效
//
class Logger {
public:
    // 初始化日志系统
    // 参数：name - 日志器名称，默认为 "crystal"
    // 此方法创建一个带颜色的控制台日志器，并设置为默认日志器
    // 如果不手动调用，首次使用时会自动调用（使用默认参数）
    static void init(const std::string& name = "crystal");

    // 获取日志器实例
    // 返回值：spdlog::logger 的 shared_ptr
    // 如果日志器未初始化，会自动调用 init() 进行初始化
    static std::shared_ptr<spdlog::logger> get();

    // ---- 简单字符串日志接口 ----
    // 参数：msg - 日志消息字符串

    static void trace(const std::string& msg) { get()->trace(msg); }
    static void debug(const std::string& msg) { get()->debug(msg); }
    static void info(const std::string& msg) { get()->info(msg); }
    static void warn(const std::string& msg) { get()->warn(msg); }
    static void error(const std::string& msg) { get()->error(msg); }

    // ---- 格式化日志接口（模板） ----
    // 使用 fmt 库的格式化语法，支持类型安全的参数化输出
    // 用法示例：
    //   Logger::info("Peer {} joined room {}", peerId, room);
    //   Logger::debug("Received {} bytes from {}", len, addr);
    //
    // 参数：
    //   fmt - fmt 格式化字符串，使用 {} 作为占位符
    //   args - 可变参数列表，与占位符一一对应

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

    // 设置日志级别
    // 参数：level - 日志级别（spdlog::level::level_enum）
    // 只有高于或等于此级别的日志才会被输出
    // 例如设置为 info 后，trace 和 debug 级别的日志将被过滤
    static void set_level(spdlog::level::level_enum level);
};

} // namespace crystal
