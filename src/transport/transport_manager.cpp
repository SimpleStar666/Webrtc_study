// ============================================================================
// transport_manager.cpp - 传输管理器实现
// ============================================================================
//
// 本文件实现了 PeerConnection 和 TransportManager 类，是 CrystalRTC 传输层
// 的核心实现代码。
//
// 【关键实现细节】
// 1. rtc::binary 与 std::vector<uint8_t> 的转换
//    libdatachannel 中 rtc::binary 定义为 std::vector<std::byte>，
//    而 C++ 中 std::byte 与 uint8_t 是不同类型，不能直接互换。
//    因此提供了 binaryToVec() 和 vecToBinary() 两个辅助函数进行转换。
//
// 2. libdatachannel 回调机制
//    rtc::PeerConnection 使用回调函数通知应用层各种事件：
//    - onLocalDescription: 本地 SDP 描述生成完成
//    - onLocalCandidate: 本地收集到新的 ICE 候选
//    - onStateChange: 连接状态变化
//    - onGatheringStateChange: ICE 候选收集状态变化
//    - onTrack: 远端添加了新的媒体轨道
//
// 3. SDP Offer/Answer 模型
//    WebRTC 使用 SDP（Session Description Protocol）进行能力协商：
//    - Offer 由呼叫方创建，包含其支持的媒体格式和传输参数
//    - Answer 由被叫方创建，在 Offer 基础上选择双方共同支持的参数
//    SDP 中包含了媒体编解码器列表、ICE 候选、DTLS 指纹等信息
//
// ============================================================================

#include "transport/transport_manager.h"
#include "utils/logger.h"
#include <rtc/rtc.hpp>
#include <random>
#include <sstream>

namespace crystal {

// 将 rtc::binary（std::vector<std::byte>）转换为 std::vector<uint8_t>
// 这是必要的，因为 libdatachannel 的 rtc::binary 底层是 std::byte 类型，
// 而 std::byte 不能隐式转换为 uint8_t，需要逐元素 static_cast
// 参数：bin - libdatachannel 的二进制数据
// 返回值：以 uint8_t 为元素的 vector
static std::vector<uint8_t> binaryToVec(const rtc::binary& bin) {
    std::vector<uint8_t> result(bin.size());
    for (size_t i = 0; i < bin.size(); i++) {
        // std::byte 到 uint8_t 需要显式转换
        result[i] = static_cast<uint8_t>(bin[i]);
    }
    return result;
}

// 将 uint8_t 数组转换为 rtc::binary（std::vector<std::byte>）
// 用于将应用层的媒体数据转换为 libdatachannel 可接受的格式
// 参数：
//   data - 指向 uint8_t 数据的指针
//   len  - 数据长度
// 返回值：libdatachannel 的 rtc::binary 对象
static rtc::binary vecToBinary(const uint8_t* data, size_t len) {
    rtc::binary result(len);
    for (size_t i = 0; i < len; i++) {
        // uint8_t 到 std::byte 需要显式转换
        result[i] = static_cast<std::byte>(data[i]);
    }
    return result;
}

// ============================================================================
// PeerConnection 实现
// ============================================================================

// 构造函数：初始化 libdatachannel 的 PeerConnection
// 核心流程：
//   1. 初始化 libdatachannel 日志级别
//   2. 配置 ICE 服务器（STUN/TURN）
//   3. 创建 rtc::PeerConnection 实例
//   4. 注册各种回调函数
PeerConnection::PeerConnection(const IceConfig& config) : config_(config) {
    // 设置 libdatachannel 的日志级别为 Warning，减少不必要的日志输出
    // 可选级别：Verbose/Debug/Info/Warning/Error/Fatal/None
    rtc::InitLogger(rtc::LogLevel::Warning);

    // 构造 libdatachannel 的配置对象
    rtc::Configuration rtcConfig;

    // 添加 STUN 服务器到 ICE 服务器列表
    // STUN 服务器用于 NAT 穿透，帮助客户端发现公网 IP 地址
    for (const auto& stun : config_.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }

    // 添加 TURN 服务器到 ICE 服务器列表
    // TURN 服务器在 P2P 直连失败时提供数据中继
    // 参数：服务器地址、端口、用户名、密码
    // 端口 3478 是 TURN 的默认端口
    for (const auto& turn : config_.turnServers) {
        rtcConfig.iceServers.emplace_back(turn, 3478,
                                           config_.turnUsername,
                                           config_.turnPassword);
    }

    // 创建 PeerConnection 实例
    // 此时 ICE Agent 开始工作，准备收集本地候选地址
    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

    // 注册本地 SDP 描述生成回调
    // 当 libdatachannel 内部生成 SDP 描述时触发（通常在 setLocalDescription 后）
    pc_->onLocalDescription([this](const rtc::Description& desc) {
        Logger::debug("Local description created: type={}",
                       desc.typeString());
    });

    // 注册本地 ICE 候选生成回调
    // 当 ICE Agent 发现新的本地候选地址时触发
    // 这是 WebRTC ICE 收集过程的核心：每发现一个候选地址就通知应用层
    // 应用层需要通过信令服务器将这些候选发送给对端
    pc_->onLocalCandidate([this](const rtc::Candidate& cand) {
        Logger::debug("Local ICE candidate: {}",
                       std::string(cand.candidate()));
        // 如果应用层注册了 ICE 候选回调，则转发候选信息
        if (iceCandidateCb_) {
            iceCandidateCb_(std::string(cand.candidate()),
                           std::string(cand.mid()), 0);
        }
    });

    // 注册连接状态变化回调
    // 状态变化流程：New -> Connecting -> Connected / Disconnected / Failed
    // Connected 表示 ICE 连接已建立且 DTLS 握手已完成
    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        Logger::info("PeerConnection state: {}", static_cast<int>(state));
        if (stateCb_) stateCb_(state);
    });

    // 注册 ICE 收集状态变化回调
    // GatheringState: New -> InProgress -> Complete
    // Complete 表示所有候选地址已收集完毕（包括 host/srflx/relay）
    pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        Logger::debug("ICE gathering state: {}", static_cast<int>(state));
    });
}

// 析构函数：关闭 PeerConnection
PeerConnection::~PeerConnection() {
    if (pc_) {
        // 关闭连接，释放 ICE/DTLS/SRTP 相关资源
        pc_->close();
    }
}

// 创建 SDP Offer
// 呼叫方调用此方法发起 WebRTC 连接
// 流程：
//   1. 创建一个视频媒体轨道（Track），方向为 SendRecv
//   2. 将 Track 添加到 PeerConnection
//   3. 设置 Track 的消息回调以接收远端媒体数据
//   4. 返回本地 SDP 描述（包含媒体格式、ICE 候选、DTLS 指纹等）
std::string PeerConnection::createOffer() {
    // 创建视频媒体描述
    // 参数1: "video" - 媒体轨道的 MID（Media ID）
    // 参数2: Direction::SendRecv - 双向模式，可发送也可接收
    // 其他可选方向：SendOnly/RecvOnly/Inactive
    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);

    // 将媒体轨道添加到 PeerConnection
    // addTrack 会触发 SDP 协商和 ICE 候选收集
    auto track = pc_->addTrack(video);
    if (track) {
        track_ = track;

        // 设置 Track 的消息回调
        // 当通过此 Track 接收到远端媒体数据时触发
        // 参数1: 消息回调（接收 rtc::binary 即 std::vector<std::byte>）
        // 参数2: nullptr 表示不设置字符串消息回调（媒体数据都是二进制的）
        track_->onMessage([this](const rtc::binary& data) {
            if (trackCb_) {
                // 将 rtc::binary 转换为 std::vector<uint8_t> 后回调
                trackCb_(binaryToVec(data));
            }
        }, nullptr);
    }

    // 获取本地 SDP 描述
    // SDP 中包含：
    // - 媒体格式信息（编解码器列表、payload type 等）
    // - ICE 候选地址（ufrag/pwd/candidates）
    // - DTLS 指纹（用于验证 DTLS 握手对方身份）
    auto desc = pc_->localDescription();
    if (desc) {
        return std::string(*desc);
    }

    return "";
}

// 创建 SDP Answer
// 被叫方在收到 Offer 并调用 setRemoteDescription 后调用
// 返回本地 SDP 描述，其中包含被叫方选择的媒体参数和 ICE 信息
std::string PeerConnection::createAnswer() {
    auto desc = pc_->localDescription();
    if (desc) {
        return std::string(*desc);
    }
    return "";
}

// 设置远端 SDP 描述
// 这是 SDP Offer/Answer 交换的关键步骤：
// - 呼叫方收到 Answer 后调用此方法设置被叫方的 SDP
// - 被叫方收到 Offer 后也调用此方法设置呼叫方的 SDP
// 设置远端描述后，libdatachannel 会：
//   1. 解析远端的 ICE 参数（ufrag/pwd）
//   2. 开始 ICE 连通性检查
//   3. 启动 DTLS 握手（协商 SRTP 加密密钥）
void PeerConnection::setRemoteDescription(const std::string& sdp,
                                           const std::string& type) {
    // 根据 type 字符串构造 rtc::Description 对象
    // type 为 "answer" 时构造 Answer 类型，否则构造 Offer 类型
    rtc::Description desc(sdp, type == "answer" ? rtc::Description::Type::Answer
                                                : rtc::Description::Type::Offer);
    // 设置远端 SDP 描述
    pc_->setRemoteDescription(desc);

    // 如果本地还没有 Track（说明是被叫方），注册 onTrack 回调
    // 当远端的媒体轨道到达时，创建本地对应的 Track 来接收数据
    if (!track_) {
        pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
            track_ = track;

            // 设置远端 Track 的消息回调
            // 接收远端发送的媒体数据（如视频帧）
            track_->onMessage([this](const rtc::binary& data) {
                if (trackCb_) {
                    trackCb_(binaryToVec(data));
                }
            }, nullptr);
        });
    }
}

// 添加远端 ICE 候选
// 对端通过信令服务器发送 ICE Candidate 后，调用此方法将其添加到本地
// ICE Agent 会将此候选纳入连通性检查列表，尝试与本地候选配对
void PeerConnection::addIceCandidate(const std::string& candidate,
                                      const std::string& sdpMid,
                                      int sdpMLineIndex) {
    // 构造 rtc::Candidate 对象
    // 参数：candidate 字符串、所属的媒体流 MID
    rtc::Candidate cand(candidate, sdpMid);
    // 将远端候选添加到 PeerConnection
    pc_->addRemoteCandidate(cand);
}

// 发送媒体数据（原始指针版本）
// 通过已建立的 Track 发送媒体数据
// 数据会经过 SRTP 加密后通过 RTP 协议传输
void PeerConnection::sendMedia(const uint8_t* data, size_t len) {
    // 检查 Track 是否存在且已打开
    if (track_ && track_->isOpen()) {
        // 将 uint8_t 数据转换为 rtc::binary 格式
        auto bin = vecToBinary(data, len);
        // 通过 Track 发送数据
        track_->send(bin);
    }
}

// 发送媒体数据（vector 版本）
// 委托给原始指针版本实现
void PeerConnection::sendMedia(const std::vector<uint8_t>& data) {
    sendMedia(data.data(), data.size());
}

// 设置 ICE 候选回调
void PeerConnection::onIceCandidate(IceCandidateCallback cb) {
    iceCandidateCb_ = std::move(cb);
}

// 设置媒体数据接收回调
void PeerConnection::onTrack(TrackCallback cb) {
    trackCb_ = std::move(cb);
}

// 设置连接状态变化回调
void PeerConnection::onStateChange(StateCallback cb) {
    stateCb_ = std::move(cb);
}

// 获取当前连接状态
rtc::PeerConnection::State PeerConnection::state() const {
    return pc_->state();
}

// 检查是否已连接
// State::Connected 表示 ICE 连接已建立且 DTLS 握手已完成
bool PeerConnection::isConnected() const {
    return pc_->state() == rtc::PeerConnection::State::Connected;
}

// ============================================================================
// TransportManager 实现
// ============================================================================

// 构造函数
TransportManager::TransportManager(const IceConfig& config)
    : config_(config) {}

// 创建新的 PeerConnection
// 线程安全：使用 lock_guard 保护 connections_ 的并发访问
std::shared_ptr<PeerConnection> TransportManager::createPeerConnection() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 生成唯一 ID，使用静态计数器确保 ID 不重复
    static int counter = 0;
    std::string id = "pc_" + std::to_string(counter++);

    // 创建新的 PeerConnection，使用共享的 ICE 配置
    auto pc = std::make_shared<PeerConnection>(config_);

    // 存入映射表
    connections_[id] = pc;
    Logger::info("Created PeerConnection: {}", id);
    return pc;
}

// 销毁指定 ID 的 PeerConnection
// 线程安全：使用 lock_guard 保护 connections_ 的并发访问
void TransportManager::destroyPeerConnection(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 从映射表中移除，shared_ptr 引用计数减 1
    // 如果没有其他持有者，PeerConnection 将被自动销毁
    connections_.erase(id);
    Logger::info("Destroyed PeerConnection: {}", id);
}

} // namespace crystal
