// ============================================================================
// main_demo.cpp - CrystalRTC 无硬件依赖的学习演示程序
// ============================================================================
//
// 【程序用途】
//   本程序是 CrystalRTC 项目的教学演示入口，无需摄像头、麦克风等硬件设备，
//   直接运行即可观察 WebRTC 视频传输核心流程的每一步。
//
// 【运行方式】
//   ./demo
//   无需任何命令行参数，程序会依次执行四个独立的演示。
//
// 【涉及的模块】
//   - RTP Packet (rtp_packet.h)       : RTP 包的构造、序列化与解析
//   - RTP Packetizer (rtp_packetizer.h): H.264 NAL Unit → RTP 包打包（含 FU-A 分片）
//   - RTP Depacketizer (rtp_depacketizer.h): RTP 包 → H.264 NAL Unit 解包（含 FU-A 重组）
//   - Jitter Buffer (jitter_buffer.h) : RTP 包乱序重排与丢包检测
//   - H.264 Encoder (h264_encoder.h)  : YUV 帧 → H.264 NAL Unit 编码
//   - H.264 Decoder (h264_decoder.h)  : H.264 NAL Unit → YUV 帧解码
//
// 【四个演示的各自目的】
//   Demo 1 - RTP 包构造与解析：
//     展示 RTP 包头各字段的含义，以及如何将 RTP 包序列化为字节流、
//     再从字节流解析回来。这是理解 WebRTC 媒体传输的基础。
//
//   Demo 2 - H.264 FU-A 分片与重组：
//     展示为什么需要 FU-A（因为单个 NAL 可能超过 MTU），
//     以及如何将大 NAL 分片为多个 RTP 包、接收端再重组为完整 NAL。
//
//   Demo 3 - Jitter Buffer 乱序重排与丢包检测：
//     展示网络传输中 RTP 包可能乱序、丢失，Jitter Buffer 如何
//     缓存并重排包，以及如何检测和统计丢包。
//
//   Demo 4 - 完整编码→RTP→解码流水线：
//     将前三个演示串联起来，展示 YUV 帧 → H.264 编码 → RTP 打包 →
//     Jitter Buffer → RTP 解包 → H.264 解码 → YUV 帧的完整数据流。
//
// 【模块协作关系图】
//
//   发送端:  YUV帧 → [H264Encoder] → NAL → [RtpPacketizer] → RTP包 → 网络
//   接收端:  网络 → RTP包 → [JitterBuffer] → 有序RTP包 → [RtpDepacketizer] → NAL → [H264Decoder] → YUV帧
//
// ============================================================================

#include "utils/logger.h"
#include "media/rtp/rtp_packet.h"
#include "media/rtp/rtp_packetizer.h"
#include "media/rtp/rtp_depacketizer.h"
#include "media/rtp/jitter_buffer.h"
#include "media/video/h264_encoder.h"
#include "media/video/h264_decoder.h"
#include <iostream>
#include <vector>
#include <chrono>

// 打印分隔线，用于在控制台上区分不同演示的输出
static void printSeparator(const std::string& title) {
    std::cout << "\n========================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "========================================\n";
}

// ============================================================================
// Demo 1: RTP 包构造与解析
// ============================================================================
// 目的：展示 RTP 包头的结构（RFC 3550），以及序列化/反序列化过程。
//       RTP 是 WebRTC 中所有媒体数据的传输协议，理解其包头格式是基础。
// ============================================================================
static int demoRtpPacket() {
    printSeparator("Demo 1: RTP Packet 构造与解析");

    // 打印 RTP 包头格式，帮助理解各字段位置（参考 RFC 3550 Section 5.1）
    std::cout << "\n[RTP 包头格式 - RFC 3550]\n";
    std::cout << " 0                   1                   2                   3\n";
    std::cout << " 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1\n";
    std::cout << "+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+\n";
    std::cout << "|V=2|P|X|  CC   |M|     PT     |       sequence number         |\n";
    std::cout << "+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+\n";
    std::cout << "|                           timestamp                           |\n";
    std::cout << "+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+\n";
    std::cout << "|                             SSRC                              |\n";
    std::cout << "+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+\n";

    // 构造一个 RTP 包，设置各头部字段
    crystal::RtpPacket pkt;
    pkt.setMarker(true);          // M=1，标记帧结束（对视频来说表示当前帧的最后一个包）
    pkt.setPayloadType(96);       // PT=96，动态负载类型，通常分配给 H.264 视频
    pkt.setSequenceNumber(42);    // 序列号，每发送一个 RTP 包递增 1，用于排序和丢包检测
    pkt.setTimestamp(90000);      // 时间戳，使用 90000Hz 时钟（视频常用），1秒=90000个单位
    pkt.setSsrc(0xDEADBEEF);     // SSRC，随机生成的同步源标识符，区分不同的媒体流
    pkt.setPayload({0x65, 0x88, 0x84, 0x00, 0x40});  // 负载数据（模拟一个 H.264 NAL 片段）

    // 打印构造的 RTP 包各字段值
    std::cout << "\n[构造 RTP 包]\n";
    std::cout << "  Version:        " << (int)pkt.version() << " (固定为2)\n";
    std::cout << "  Marker:         " << (pkt.marker() ? "1 (帧结束标记)" : "0") << "\n";
    std::cout << "  Payload Type:   " << (int)pkt.payloadType() << " (96=动态分配给H264)\n";
    std::cout << "  Sequence Num:   " << pkt.sequenceNumber() << " (每发一个包+1)\n";
    std::cout << "  Timestamp:      " << pkt.timestamp() << " (90000Hz时钟, 1秒=90000)\n";
    std::cout << "  SSRC:           0x" << std::hex << pkt.ssrc() << std::dec << " (随机生成的流标识)\n";
    std::cout << "  Payload大小:    " << pkt.payload().size() << " 字节\n";
    std::cout << "  总大小:         " << pkt.totalSize() << " 字节 (12字节头+" << pkt.payload().size() << "字节负载)\n";

    // 序列化：将 RTP 包转为字节流，这就是实际通过网络传输的数据
    auto wire = pkt.serialize();
    std::cout << "\n[序列化后的字节流 (网络传输的就是这个)]\n  ";
    for (size_t i = 0; i < wire.size(); i++) {
        printf("%02X ", wire[i]);
        if ((i + 1) % 16 == 0) std::cout << "\n  ";
    }
    std::cout << "\n";

    // 解析：从字节流还原 RTP 包，模拟接收端的处理
    crystal::RtpPacket parsed;
    bool ok = parsed.parse(wire.data(), wire.size());
    std::cout << "\n[从字节流解析回来] " << (ok ? "成功 ✓" : "失败 ✗") << "\n";
    std::cout << "  Marker:       " << (parsed.marker() ? "true" : "false") << "\n";
    std::cout << "  PayloadType:  " << (int)parsed.payloadType() << "\n";
    std::cout << "  SeqNum:       " << parsed.sequenceNumber() << "\n";
    std::cout << "  Timestamp:    " << parsed.timestamp() << "\n";

    return 0;
}

// ============================================================================
// Demo 2: H.264 FU-A 分片与重组
// ============================================================================
// 目的：展示当 H.264 NAL Unit 大小超过 MTU 时，如何使用 FU-A 机制将其
//       分片为多个 RTP 包传输，接收端再重组为完整 NAL。
//       这是 WebRTC 视频 RTP 承载（RFC 6184）的核心机制。
// ============================================================================
static int demoFUA() {
    printSeparator("Demo 2: H.264 FU-A 分片与重组");

    // 解释为什么需要 FU-A：以太网 MTU 限制
    std::cout << "\n[为什么需要 FU-A?]\n";
    std::cout << "  以太网 MTU = 1500 字节\n";
    std::cout << "  RTP头 = 12 字节, UDP头 = 8 字节, IP头 = 20 字节\n";
    std::cout << "  所以一个RTP包的最大负载 ≈ 1460 字节\n";
    std::cout << "  但一个 H.264 IDR 帧可能有 几十KB~几百KB\n";
    std::cout << "  → 必须拆分成多个 RTP 包传输！\n";

    // 场景1：小 NAL（5字节），无需分片，直接放入单个 RTP 包
    std::vector<uint8_t> smallNal = {0x65, 0x88, 0x84, 0x00, 0x40};
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);  // PT=96, 时钟=90000Hz, SSRC=0x12345678

    std::cout << "\n--- 场景1: 小 NAL (5字节) → 单包模式 ---\n";
    auto singlePkts = pktizer.packetizeH264(smallNal, 3000);  // timestamp=3000
    std::cout << "  NAL大小: " << smallNal.size() << " 字节 < MTU(1200)\n";
    std::cout << "  生成RTP包数: " << singlePkts.size() << "\n";
    // 小 NAL 只生成 1 个 RTP 包，marker=1 表示帧结束
    std::cout << "  包0: seq=" << singlePkts[0].sequenceNumber()
              << " marker=" << (singlePkts[0].marker() ? "1" : "0")
              << " size=" << singlePkts[0].totalSize() << "\n";

    // 场景2：大 NAL（3000字节），超过 MTU，需要 FU-A 分片
    std::cout << "\n--- 场景2: 大 NAL (3000字节) → FU-A 分片模式 ---\n";
    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;  // NAL 头：type=5 (IDR slice), nal_ref_idc=3

    // 创建新的打包器，确保序列号从 0 开始
    crystal::RtpPacketizer pktizer2(96, 90000, 0x12345678);
    auto fuPkts = pktizer2.packetizeH264(largeNal, 5000);  // timestamp=5000
    std::cout << "  NAL大小: " << largeNal.size() << " 字节 > MTU(1200)\n";
    std::cout << "  生成RTP包数: " << fuPkts.size() << "\n\n";

    // 打印 FU-A 包格式说明
    std::cout << "  [FU-A 包格式]\n";
    std::cout << "  FU Indicator (1字节): forbidden_zero_bit(1) | nal_ref_idc(2) | type=28(5)\n";
    std::cout << "  FU Header    (1字节): start(1) | end(1) | reserved(1) | nal_type(5)\n\n";

    // 遍历每个 FU-A 分片，解析其 FU Indicator 和 FU Header
    for (size_t i = 0; i < fuPkts.size(); i++) {
        const auto& p = fuPkts[i];
        uint8_t fuIndicator = p.payload()[0];  // FU Indicator：type=28 表示 FU-A
        uint8_t fuHeader = p.payload()[1];      // FU Header：包含 start/end 标记和原始 NAL type
        bool isStart = (fuHeader & 0x80) != 0;  // 最高位=1 表示这是第一个分片
        bool isEnd = (fuHeader & 0x40) != 0;    // 次高位=1 表示这是最后一个分片
        uint8_t nalType = fuHeader & 0x1F;       // 低5位是原始 NAL Unit 的类型
        uint8_t nalRefIdc = (fuIndicator >> 5) & 0x03;  // 从 FU Indicator 中提取 nal_ref_idc

        std::cout << "  包" << i << ": seq=" << p.sequenceNumber()
                  << " marker=" << (p.marker() ? "1" : "0")
                  << " FU-Indicator[type=" << (int)(fuIndicator & 0x1F) << " ref_idc=" << (int)nalRefIdc << "]"
                  << " FU-Header[";
        if (isStart) std::cout << "START ";
        if (isEnd) std::cout << "END ";
        std::cout << "nal_type=" << (int)nalType << "]"
                  << " payload=" << p.payload().size() << "字节\n";
    }

    // 重组验证：用 RtpDepacketizer 将 FU-A 分片重组回原始 NAL
    std::cout << "\n--- 重组验证 ---\n";
    crystal::RtpDepacketizer depktizer;
    std::vector<std::vector<uint8_t>> reassembled;
    for (const auto& p : fuPkts) {
        // 逐个 RTP 包送入解包器，当收到完整 NAL 时返回
        auto nals = depktizer.depacketizeH264(p);
        reassembled.insert(reassembled.end(), nals.begin(), nals.end());
    }
    // 验证重组后的 NAL 是否与原始 NAL 完全一致
    bool match = (reassembled.size() == 1 && reassembled[0] == largeNal);
    std::cout << "  重组后NAL数: " << reassembled.size() << "\n";
    std::cout << "  重组后大小: " << reassembled[0].size() << " 字节\n";
    std::cout << "  与原始一致: " << (match ? "✓ 完全匹配！" : "✗ 不匹配") << "\n";

    return 0;
}

// ============================================================================
// Demo 3: Jitter Buffer 乱序重排与丢包检测
// ============================================================================
// 目的：展示 Jitter Buffer 如何解决网络传输中的三个问题：
//       1. 乱序到达（后发的包先到）
//       2. 丢包（某些包永远不会到达）
//       3. 延迟抖动（相邻包到达间隔不一致）
//       Jitter Buffer 通过缓存一定量的包，等待后重排输出，保证解码器收到有序数据。
// ============================================================================
static int demoJitterBuffer() {
    printSeparator("Demo 3: Jitter Buffer 乱序重排与丢包检测");

    // 解释 Jitter Buffer 的必要性
    std::cout << "\n[为什么需要 Jitter Buffer?]\n";
    std::cout << "  网络传输中，RTP包可能:\n";
    std::cout << "  1. 乱序到达 (包102比101先到)\n";
    std::cout << "  2. 丢失 (某些包永远不会到达)\n";
    std::cout << "  3. 延迟抖动 (相邻包到达间隔不一致)\n";
    std::cout << "  Jitter Buffer 缓存一定量的包，重排后按序输出\n\n";

    // 模拟1：正常顺序到达，Jitter Buffer 直接按序输出
    std::cout << "--- 模拟1: 正常顺序到达 ---\n";
    crystal::JitterBuffer jb(40);  // 缓冲区大小=40个包
    crystal::RtpPacket p1, p2, p3;
    p1.setSequenceNumber(100); p1.setTimestamp(90000);   // seq=100, 时间戳=1秒
    p2.setSequenceNumber(101); p2.setTimestamp(93000);   // seq=101, 时间戳=1.033秒
    p3.setSequenceNumber(102); p3.setTimestamp(96000);   // seq=102, 时间戳=1.067秒

    // 按序插入 Jitter Buffer
    jb.insert(p1);
    jb.insert(p2);
    jb.insert(p3);
    // consume() 返回按序列号排序的 RTP 包列表
    auto out = jb.consume();
    std::cout << "  输入: 100, 101, 102\n";
    std::cout << "  输出: ";
    for (const auto& p : out) std::cout << p.sequenceNumber() << " ";
    std::cout << "\n  丢包: " << jb.lostPacketCount() << "\n";

    // 模拟2：乱序到达（seq=202 先于 201 到达），Jitter Buffer 能正确重排
    std::cout << "\n--- 模拟2: 乱序到达 (102先于101到达) ---\n";
    crystal::JitterBuffer jb2(40);
    crystal::RtpPacket q1, q2, q3;
    q1.setSequenceNumber(200); q1.setTimestamp(180000);
    q3.setSequenceNumber(202); q3.setTimestamp(186000);
    q2.setSequenceNumber(201); q2.setTimestamp(183000);

    jb2.insert(q1);
    std::cout << "  收到 seq=200\n";
    // seq=202 先到，但 seq=201 还没到，Jitter Buffer 会缓存 202 等待 201
    jb2.insert(q3);
    std::cout << "  收到 seq=202 (乱序! 201还没到)\n";
    // seq=201 迟到但仍在缓冲窗口内，Jitter Buffer 可以正确重排
    jb2.insert(q2);
    std::cout << "  收到 seq=201 (迟到了但还在缓冲区内)\n";
    auto out2 = jb2.consume();
    std::cout << "  输出: ";
    for (const auto& p : out2) std::cout << p.sequenceNumber() << " ";
    std::cout << "\n  丢包: " << jb2.lostPacketCount() << "\n";

    // 模拟3：丢包（seq=301 永远不会到达），Jitter Buffer 检测到丢包
    std::cout << "\n--- 模拟3: 丢包 (seq 301丢失) ---\n";
    crystal::JitterBuffer jb3(40);
    crystal::RtpPacket r1, r3;
    r1.setSequenceNumber(300); r1.setTimestamp(270000);
    r3.setSequenceNumber(302); r3.setTimestamp(276000);

    jb3.insert(r1);
    std::cout << "  收到 seq=300\n";
    // 收到 seq=302 但 seq=301 从未到达，Jitter Buffer 判定 seq=301 丢失
    jb3.insert(r3);
    std::cout << "  收到 seq=302 (301丢失!)\n";
    auto out3 = jb3.consume();
    std::cout << "  输出: ";
    for (const auto& p : out3) std::cout << p.sequenceNumber() << " ";
    std::cout << "\n  丢包数: " << jb3.lostPacketCount() << "\n";
    std::cout << "  丢包率: " << (jb3.lossRate() * 100) << "%\n";

    return 0;
}

// ============================================================================
// Demo 4: H.264 编码 → RTP打包 → 解包 → 解码 完整管道
// ============================================================================
// 目的：将前三个演示串联，展示 WebRTC 视频传输的完整数据流：
//       发送端：YUV帧 → H.264编码 → NAL → RTP打包 → 发送
//       接收端：接收 → JitterBuffer → RTP解包 → NAL → H.264解码 → YUV帧
//       这是 WebRTC 视频传输的核心流水线。
// ============================================================================
static int demoH264Pipeline() {
    printSeparator("Demo 4: H.264 编码 → RTP打包 → 解包 → 解码 完整管道");

    // 打印完整数据流路径
    std::cout << "\n[这是 WebRTC 视频传输的核心数据流]\n";
    std::cout << "  发送端: YUV帧 → H.264编码 → NAL Unit → RTP打包 → 发送\n";
    std::cout << "  接收端: 接收 → JitterBuffer → RTP解包 → NAL Unit → H.264解码 → YUV帧\n\n";

    // ---- 发送端：初始化编码器 ----
    crystal::H264EncoderConfig encConfig;
    encConfig.width = 64;           // 使用小分辨率以加速演示
    encConfig.height = 64;
    encConfig.fps = 10;
    encConfig.bitrateKbps = 200;
    crystal::H264Encoder encoder(encConfig);

    if (!encoder.init()) {
        std::cout << "  [编码器初始化失败 - 可能缺少libx264，跳过此demo]\n";
        return 0;
    }

    // ---- 接收端：初始化解码器 ----
    crystal::H264Decoder decoder;
    if (!decoder.init()) {
        std::cout << "  [解码器初始化失败]\n";
        return 0;
    }

    // 生成测试用 YUV420P 帧（64x64，Y/U/V 各填充不同值）
    int ySize = 64 * 64;            // Y 分量大小 = width * height
    int uvSize = ySize / 4;         // U/V 分量大小 = width * height / 4（4:2:0 子采样）
    std::vector<uint8_t> yuvFrame(ySize + 2 * uvSize, 128);  // U/V 填充 128（灰色）
    for (int i = 0; i < ySize; i++) {
        yuvFrame[i] = (i * 7) % 256;  // Y 分量填充渐变值，产生可见的图案
    }

    std::cout << "--- 发送端 ---\n";
    std::cout << "  1. 生成测试YUV帧: " << encConfig.width << "x" << encConfig.height
              << " 大小=" << yuvFrame.size() << " 字节\n";

    // 注册编码回调：编码器每输出一个 NAL Unit 就会调用此回调
    std::vector<std::vector<uint8_t>> encodedNals;
    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        encodedNals.emplace_back(nalData, nalData + nalLen);
    });

    // 执行编码：YUV帧 → H.264 NAL Units
    encoder.encode(yuvFrame.data(), yuvFrame.size());
    std::cout << "  2. H.264编码输出: " << encodedNals.size() << " 个 NAL Unit\n";

    // 将每个 NAL Unit 打包为 RTP 包
    crystal::RtpPacketizer pktizer(96, 90000, 0xCAFEBABE);  // PT=96, 时钟=90000Hz, SSRC=0xCAFEBABE
    std::vector<crystal::RtpPacket> allRtpPackets;
    uint32_t ts = 0;
    for (const auto& nal : encodedNals) {
        // 解析 NAL 类型，帮助理解编码器输出了什么
        uint8_t nalType = nal[0] & 0x1F;
        const char* typeName = "Unknown";
        if (nalType == 5) typeName = "IDR";       // IDR 帧：关键帧，可独立解码
        else if (nalType == 1) typeName = "P-Slice";  // P 帧：参考前面的帧解码
        else if (nalType == 7) typeName = "SPS";   // SPS：序列参数集，描述视频参数
        else if (nalType == 8) typeName = "PPS";   // PPS：图像参数集，描述编码参数
        std::cout << "     NAL type=" << (int)nalType << " (" << typeName
                  << ") size=" << nal.size() << " 字节\n";

        // 将 NAL 打包为 RTP 包（大 NAL 会自动使用 FU-A 分片）
        auto pkts = pktizer.packetizeH264(nal, ts);
        std::cout << "     → 打包为 " << pkts.size() << " 个RTP包\n";
        allRtpPackets.insert(allRtpPackets.end(), pkts.begin(), pkts.end());
        ts += 3000;  // 每个 NAL 的时间戳递增 3000（对应 33ms，即 ~30fps）
    }
    std::cout << "  3. RTP打包完成: 共 " << allRtpPackets.size() << " 个RTP包\n";

    // ---- 网络传输模拟 ----
    std::cout << "\n--- 网络传输 (模拟) ---\n";
    std::cout << "  " << allRtpPackets.size() << " 个RTP包通过网络传输...\n";

    // ---- 接收端：初始化解包器、Jitter Buffer、解码器 ----
    std::cout << "\n--- 接收端 ---\n";
    crystal::RtpDepacketizer depktizer;
    crystal::JitterBuffer jb(40);
    int decodedFrames = 0;

    // 注册解码回调：解码器每输出一帧 YUV 就会调用此回调
    decoder.onDecoded([&](const uint8_t* yuvData, int width, int height) {
        decodedFrames++;
        std::cout << "  6. 解码输出: " << width << "x" << height
                  << " YUV帧 (第" << decodedFrames << "帧)\n";
    });

    // 步骤4：将所有 RTP 包送入 Jitter Buffer 进行重排
    std::cout << "  4. JitterBuffer 接收并重排...\n";
    for (const auto& pkt : allRtpPackets) {
        jb.insert(pkt);
    }
    auto ordered = jb.consume();  // 获取按序排列的 RTP 包
    std::cout << "     收到 " << ordered.size() << " 个有序RTP包, 丢包=" << jb.lostPacketCount() << "\n";

    // 步骤5-6：RTP 解包 → NAL → H.264 解码
    std::cout << "  5. RTP解包 → NAL Unit → H.264解码...\n";
    for (const auto& pkt : ordered) {
        // 从 RTP 包中提取 NAL Unit（如果是 FU-A 分片，会自动重组）
        auto nals = depktizer.depacketizeH264(pkt);
        for (const auto& nal : nals) {
            // 将 NAL 送入解码器
            decoder.decode(nal.data(), nal.size());
        }
    }

    // 验证完整管道是否成功
    std::cout << "\n  [完整管道验证] " << (decodedFrames > 0 ? "✓ 成功！YUV帧经过编码→RTP→解包→解码完整还原" : "等待更多帧...") << "\n";

    return 0;
}

// ============================================================================
// 主函数：依次执行四个演示
// ============================================================================
int main() {
    // 初始化日志系统，设置日志级别为 warn 以减少干扰输出
    crystal::Logger::init("demo");
    crystal::Logger::set_level(spdlog::level::warn);

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║       CrystalRTC 学习演示程序            ║\n";
    std::cout << "║   无需摄像头/麦克风，直接运行看效果       ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";

    // 依次执行四个演示，从简单到复杂
    demoRtpPacket();       // Demo 1: RTP 包构造与解析
    demoFUA();             // Demo 2: FU-A 分片与重组
    demoJitterBuffer();    // Demo 3: Jitter Buffer 乱序重排与丢包检测
    demoH264Pipeline();    // Demo 4: 完整编码→RTP→解码流水线

    // 打印总结
    printSeparator("总结");
    std::cout << "\n  你刚刚看到了 WebRTC 视频传输的核心流程:\n\n";
    std::cout << "  1. RTP包构造: 12字节固定头 + 负载\n";
    std::cout << "  2. FU-A分片:  大NAL拆成小RTP包，接收端重组\n";
    std::cout << "  3. JitterBuffer: 解决乱序和丢包问题\n";
    std::cout << "  4. 完整管道:  YUV→编码→RTP→解包→解码→YUV\n\n";
    std::cout << "  接下来请阅读学习指南，深入理解每个模块！\n\n";

    return 0;
}
