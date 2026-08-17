// ============================================================================
// ice_config.h - ICE 配置结构体
// ============================================================================
//
// 本文件定义了 CrystalRTC 中 ICE（Interactive Connectivity Establishment，
// 交互式连接建立）协议的配置信息。
//
// 【ICE 在 WebRTC 中的角色】
// WebRTC 需要在两个浏览器/终端之间建立点对点（P2P）连接，但现实中两端通常
// 位于不同的 NAT（网络地址转换）路由器后面，无法直接通信。ICE 协议就是解决
// NAT 穿透问题的标准方案，它通过依次尝试多种候选地址（Candidate）来找到一条
// 可用的网络路径。
//
// 【NAT 穿透的核心组件】
// 1. STUN（Session Traversal Utilities for NAT）
//    - 作用：帮助客户端发现自己经过 NAT 映射后的公网 IP 地址（即"服务器反射
//      候选地址" srflx）
//    - 工作方式：客户端向 STUN 服务器发送请求，服务器回复中包含客户端的公网
//      IP:Port，这个地址可以被对端用来连接
//    - 优点：轻量、快速；缺点：无法穿透对称型 NAT（Symmetric NAT）
//
// 2. TURN（Traversal Using Relays around NAT）
//    - 作用：当直接 P2P 连接无法建立时（如双方都在对称型 NAT 后面），提供一
//      个中继服务器转发所有媒体流量（即"中继候选地址" relay）
//    - 工作方式：客户端与 TURN 服务器建立连接，所有数据通过 TURN 服务器中转
//    - 优点：保证连通性；缺点：增加延迟和带宽成本
//
// 【ICE 候选地址类型】
// - host：本地网络接口地址（如 192.168.1.100）
// - srflx：通过 STUN 获取的服务器反射地址（公网 IP）
// - relay：通过 TURN 获取的中继地址
// ICE 按优先级依次尝试这些候选地址，优先使用 host（直连），其次 srflx，最后
// 才使用 relay（中继）。
//
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace crystal {

// ICE 配置结构体
// 封装了建立 WebRTC 连接所需的 ICE 服务器信息，包括 STUN 和 TURN 服务器列表
struct IceConfig {
    // STUN 服务器地址列表
    // 格式为 "stun:host:port"，例如 "stun:stun.l.google.com:19302"
    // STUN 服务器用于获取客户端的公网 IP 地址（服务器反射候选）
    std::vector<std::string> stunServers;

    // TURN 服务器地址列表
    // 格式为 "turn:host:port"，例如 "turn:turn.example.com:3478"
    // TURN 服务器在 P2P 直连失败时提供数据中继服务
    std::vector<std::string> turnServers;

    // TURN 服务器认证用户名
    // TURN 服务器需要认证以防止滥用，通常使用 HMAC 认证机制
    std::string turnUsername;

    // TURN 服务器认证密码
    // 与 turnUsername 配对使用，用于 TURN 的长期凭证认证（Long-Term Credential）
    std::string turnPassword;

    // 生成默认 ICE 配置
    // 返回一个包含 Google 公共 STUN 服务器的默认配置
    // 注意：Google STUN 服务器仅用于开发测试，生产环境应部署自己的 STUN/TURN
    static IceConfig defaultConfig() {
        IceConfig cfg;
        // Google 提供的免费 STUN 服务器，端口 19302 是标准的 STUN 端口
        cfg.stunServers = {"stun:stun.l.google.com:19302"};
        return cfg;
    }
};

} // namespace crystal
