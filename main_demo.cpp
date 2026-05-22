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

static void printSeparator(const std::string& title) {
    std::cout << "\n========================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "========================================\n";
}

static int demoRtpPacket() {
    printSeparator("Demo 1: RTP Packet 构造与解析");

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

    crystal::RtpPacket pkt;
    pkt.setMarker(true);
    pkt.setPayloadType(96);
    pkt.setSequenceNumber(42);
    pkt.setTimestamp(90000);
    pkt.setSsrc(0xDEADBEEF);
    pkt.setPayload({0x65, 0x88, 0x84, 0x00, 0x40});

    std::cout << "\n[构造 RTP 包]\n";
    std::cout << "  Version:        " << (int)pkt.version() << " (固定为2)\n";
    std::cout << "  Marker:         " << (pkt.marker() ? "1 (帧结束标记)" : "0") << "\n";
    std::cout << "  Payload Type:   " << (int)pkt.payloadType() << " (96=动态分配给H264)\n";
    std::cout << "  Sequence Num:   " << pkt.sequenceNumber() << " (每发一个包+1)\n";
    std::cout << "  Timestamp:      " << pkt.timestamp() << " (90000Hz时钟, 1秒=90000)\n";
    std::cout << "  SSRC:           0x" << std::hex << pkt.ssrc() << std::dec << " (随机生成的流标识)\n";
    std::cout << "  Payload大小:    " << pkt.payload().size() << " 字节\n";
    std::cout << "  总大小:         " << pkt.totalSize() << " 字节 (12字节头+" << pkt.payload().size() << "字节负载)\n";

    auto wire = pkt.serialize();
    std::cout << "\n[序列化后的字节流 (网络传输的就是这个)]\n  ";
    for (size_t i = 0; i < wire.size(); i++) {
        printf("%02X ", wire[i]);
        if ((i + 1) % 16 == 0) std::cout << "\n  ";
    }
    std::cout << "\n";

    crystal::RtpPacket parsed;
    bool ok = parsed.parse(wire.data(), wire.size());
    std::cout << "\n[从字节流解析回来] " << (ok ? "成功 ✓" : "失败 ✗") << "\n";
    std::cout << "  Marker:       " << (parsed.marker() ? "true" : "false") << "\n";
    std::cout << "  PayloadType:  " << (int)parsed.payloadType() << "\n";
    std::cout << "  SeqNum:       " << parsed.sequenceNumber() << "\n";
    std::cout << "  Timestamp:    " << parsed.timestamp() << "\n";

    return 0;
}

static int demoFUA() {
    printSeparator("Demo 2: H.264 FU-A 分片与重组");

    std::cout << "\n[为什么需要 FU-A?]\n";
    std::cout << "  以太网 MTU = 1500 字节\n";
    std::cout << "  RTP头 = 12 字节, UDP头 = 8 字节, IP头 = 20 字节\n";
    std::cout << "  所以一个RTP包的最大负载 ≈ 1460 字节\n";
    std::cout << "  但一个 H.264 IDR 帧可能有 几十KB~几百KB\n";
    std::cout << "  → 必须拆分成多个 RTP 包传输！\n";

    std::vector<uint8_t> smallNal = {0x65, 0x88, 0x84, 0x00, 0x40};
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    std::cout << "\n--- 场景1: 小 NAL (5字节) → 单包模式 ---\n";
    auto singlePkts = pktizer.packetizeH264(smallNal, 3000);
    std::cout << "  NAL大小: " << smallNal.size() << " 字节 < MTU(1200)\n";
    std::cout << "  生成RTP包数: " << singlePkts.size() << "\n";
    std::cout << "  包0: seq=" << singlePkts[0].sequenceNumber()
              << " marker=" << (singlePkts[0].marker() ? "1" : "0")
              << " size=" << singlePkts[0].totalSize() << "\n";

    std::cout << "\n--- 场景2: 大 NAL (3000字节) → FU-A 分片模式 ---\n";
    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    crystal::RtpPacketizer pktizer2(96, 90000, 0x12345678);
    auto fuPkts = pktizer2.packetizeH264(largeNal, 5000);
    std::cout << "  NAL大小: " << largeNal.size() << " 字节 > MTU(1200)\n";
    std::cout << "  生成RTP包数: " << fuPkts.size() << "\n\n";

    std::cout << "  [FU-A 包格式]\n";
    std::cout << "  FU Indicator (1字节): forbidden_zero_bit(1) | nal_ref_idc(2) | type=28(5)\n";
    std::cout << "  FU Header    (1字节): start(1) | end(1) | reserved(1) | nal_type(5)\n\n";

    for (size_t i = 0; i < fuPkts.size(); i++) {
        const auto& p = fuPkts[i];
        uint8_t fuIndicator = p.payload()[0];
        uint8_t fuHeader = p.payload()[1];
        bool isStart = (fuHeader & 0x80) != 0;
        bool isEnd = (fuHeader & 0x40) != 0;
        uint8_t nalType = fuHeader & 0x1F;
        uint8_t nalRefIdc = (fuIndicator >> 5) & 0x03;

        std::cout << "  包" << i << ": seq=" << p.sequenceNumber()
                  << " marker=" << (p.marker() ? "1" : "0")
                  << " FU-Indicator[type=" << (int)(fuIndicator & 0x1F) << " ref_idc=" << (int)nalRefIdc << "]"
                  << " FU-Header[";
        if (isStart) std::cout << "START ";
        if (isEnd) std::cout << "END ";
        std::cout << "nal_type=" << (int)nalType << "]"
                  << " payload=" << p.payload().size() << "字节\n";
    }

    std::cout << "\n--- 重组验证 ---\n";
    crystal::RtpDepacketizer depktizer;
    std::vector<std::vector<uint8_t>> reassembled;
    for (const auto& p : fuPkts) {
        auto nals = depktizer.depacketizeH264(p);
        reassembled.insert(reassembled.end(), nals.begin(), nals.end());
    }
    bool match = (reassembled.size() == 1 && reassembled[0] == largeNal);
    std::cout << "  重组后NAL数: " << reassembled.size() << "\n";
    std::cout << "  重组后大小: " << reassembled[0].size() << " 字节\n";
    std::cout << "  与原始一致: " << (match ? "✓ 完全匹配！" : "✗ 不匹配") << "\n";

    return 0;
}

static int demoJitterBuffer() {
    printSeparator("Demo 3: Jitter Buffer 乱序重排与丢包检测");

    std::cout << "\n[为什么需要 Jitter Buffer?]\n";
    std::cout << "  网络传输中，RTP包可能:\n";
    std::cout << "  1. 乱序到达 (包102比101先到)\n";
    std::cout << "  2. 丢失 (某些包永远不会到达)\n";
    std::cout << "  3. 延迟抖动 (相邻包到达间隔不一致)\n";
    std::cout << "  Jitter Buffer 缓存一定量的包，重排后按序输出\n\n";

    crystal::JitterBuffer jb(40);

    std::cout << "--- 模拟1: 正常顺序到达 ---\n";
    crystal::RtpPacket p1, p2, p3;
    p1.setSequenceNumber(100); p1.setTimestamp(90000);
    p2.setSequenceNumber(101); p2.setTimestamp(93000);
    p3.setSequenceNumber(102); p3.setTimestamp(96000);

    jb.insert(p1);
    jb.insert(p2);
    jb.insert(p3);
    auto out = jb.consume();
    std::cout << "  输入: 100, 101, 102\n";
    std::cout << "  输出: ";
    for (const auto& p : out) std::cout << p.sequenceNumber() << " ";
    std::cout << "\n  丢包: " << jb.lostPacketCount() << "\n";

    std::cout << "\n--- 模拟2: 乱序到达 (102先于101到达) ---\n";
    crystal::JitterBuffer jb2(40);
    crystal::RtpPacket q1, q2, q3;
    q1.setSequenceNumber(200); q1.setTimestamp(180000);
    q3.setSequenceNumber(202); q3.setTimestamp(186000);
    q2.setSequenceNumber(201); q2.setTimestamp(183000);

    jb2.insert(q1);
    std::cout << "  收到 seq=200\n";
    jb2.insert(q3);
    std::cout << "  收到 seq=202 (乱序! 201还没到)\n";
    jb2.insert(q2);
    std::cout << "  收到 seq=201 (迟到了但还在缓冲区内)\n";
    auto out2 = jb2.consume();
    std::cout << "  输出: ";
    for (const auto& p : out2) std::cout << p.sequenceNumber() << " ";
    std::cout << "\n  丢包: " << jb2.lostPacketCount() << "\n";

    std::cout << "\n--- 模拟3: 丢包 (seq 301丢失) ---\n";
    crystal::JitterBuffer jb3(40);
    crystal::RtpPacket r1, r3;
    r1.setSequenceNumber(300); r1.setTimestamp(270000);
    r3.setSequenceNumber(302); r3.setTimestamp(276000);

    jb3.insert(r1);
    std::cout << "  收到 seq=300\n";
    jb3.insert(r3);
    std::cout << "  收到 seq=302 (301丢失!)\n";
    auto out3 = jb3.consume();
    std::cout << "  输出: ";
    for (const auto& p : out3) std::cout << p.sequenceNumber() << " ";
    std::cout << "\n  丢包数: " << jb3.lostPacketCount() << "\n";
    std::cout << "  丢包率: " << (jb3.lossRate() * 100) << "%\n";

    return 0;
}

static int demoH264Pipeline() {
    printSeparator("Demo 4: H.264 编码 → RTP打包 → 解包 → 解码 完整管道");

    std::cout << "\n[这是 WebRTC 视频传输的核心数据流]\n";
    std::cout << "  发送端: YUV帧 → H.264编码 → NAL Unit → RTP打包 → 发送\n";
    std::cout << "  接收端: 接收 → JitterBuffer → RTP解包 → NAL Unit → H.264解码 → YUV帧\n\n";

    crystal::H264EncoderConfig encConfig;
    encConfig.width = 64;
    encConfig.height = 64;
    encConfig.fps = 10;
    encConfig.bitrateKbps = 200;
    crystal::H264Encoder encoder(encConfig);

    if (!encoder.init()) {
        std::cout << "  [编码器初始化失败 - 可能缺少libx264，跳过此demo]\n";
        return 0;
    }

    crystal::H264Decoder decoder;
    if (!decoder.init()) {
        std::cout << "  [解码器初始化失败]\n";
        return 0;
    }

    int ySize = 64 * 64;
    int uvSize = ySize / 4;
    std::vector<uint8_t> yuvFrame(ySize + 2 * uvSize, 128);
    for (int i = 0; i < ySize; i++) {
        yuvFrame[i] = (i * 7) % 256;
    }

    std::cout << "--- 发送端 ---\n";
    std::cout << "  1. 生成测试YUV帧: " << encConfig.width << "x" << encConfig.height
              << " 大小=" << yuvFrame.size() << " 字节\n";

    std::vector<std::vector<uint8_t>> encodedNals;
    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        encodedNals.emplace_back(nalData, nalData + nalLen);
    });

    encoder.encode(yuvFrame.data(), yuvFrame.size());
    std::cout << "  2. H.264编码输出: " << encodedNals.size() << " 个 NAL Unit\n";

    crystal::RtpPacketizer pktizer(96, 90000, 0xCAFEBABE);
    std::vector<crystal::RtpPacket> allRtpPackets;
    uint32_t ts = 0;
    for (const auto& nal : encodedNals) {
        uint8_t nalType = nal[0] & 0x1F;
        const char* typeName = "Unknown";
        if (nalType == 5) typeName = "IDR";
        else if (nalType == 1) typeName = "P-Slice";
        else if (nalType == 7) typeName = "SPS";
        else if (nalType == 8) typeName = "PPS";
        std::cout << "     NAL type=" << (int)nalType << " (" << typeName
                  << ") size=" << nal.size() << " 字节\n";

        auto pkts = pktizer.packetizeH264(nal, ts);
        std::cout << "     → 打包为 " << pkts.size() << " 个RTP包\n";
        allRtpPackets.insert(allRtpPackets.end(), pkts.begin(), pkts.end());
        ts += 3000;
    }
    std::cout << "  3. RTP打包完成: 共 " << allRtpPackets.size() << " 个RTP包\n";

    std::cout << "\n--- 网络传输 (模拟) ---\n";
    std::cout << "  " << allRtpPackets.size() << " 个RTP包通过网络传输...\n";

    std::cout << "\n--- 接收端 ---\n";
    crystal::RtpDepacketizer depktizer;
    crystal::JitterBuffer jb(40);
    int decodedFrames = 0;

    decoder.onDecoded([&](const uint8_t* yuvData, int width, int height) {
        decodedFrames++;
        std::cout << "  6. 解码输出: " << width << "x" << height
                  << " YUV帧 (第" << decodedFrames << "帧)\n";
    });

    std::cout << "  4. JitterBuffer 接收并重排...\n";
    for (const auto& pkt : allRtpPackets) {
        jb.insert(pkt);
    }
    auto ordered = jb.consume();
    std::cout << "     收到 " << ordered.size() << " 个有序RTP包, 丢包=" << jb.lostPacketCount() << "\n";

    std::cout << "  5. RTP解包 → NAL Unit → H.264解码...\n";
    for (const auto& pkt : ordered) {
        auto nals = depktizer.depacketizeH264(pkt);
        for (const auto& nal : nals) {
            decoder.decode(nal.data(), nal.size());
        }
    }

    std::cout << "\n  [完整管道验证] " << (decodedFrames > 0 ? "✓ 成功！YUV帧经过编码→RTP→解包→解码完整还原" : "等待更多帧...") << "\n";

    return 0;
}

int main() {
    crystal::Logger::init("demo");
    crystal::Logger::set_level(spdlog::level::warn);

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║       CrystalRTC 学习演示程序            ║\n";
    std::cout << "║   无需摄像头/麦克风，直接运行看效果       ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";

    demoRtpPacket();
    demoFUA();
    demoJitterBuffer();
    demoH264Pipeline();

    printSeparator("总结");
    std::cout << "\n  你刚刚看到了 WebRTC 视频传输的核心流程:\n\n";
    std::cout << "  1. RTP包构造: 12字节固定头 + 负载\n";
    std::cout << "  2. FU-A分片:  大NAL拆成小RTP包，接收端重组\n";
    std::cout << "  3. JitterBuffer: 解决乱序和丢包问题\n";
    std::cout << "  4. 完整管道:  YUV→编码→RTP→解包→解码→YUV\n\n";
    std::cout << "  接下来请阅读学习指南，深入理解每个模块！\n\n";

    return 0;
}
