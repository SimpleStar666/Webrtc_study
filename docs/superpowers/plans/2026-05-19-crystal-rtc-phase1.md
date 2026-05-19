# CrystalRTC Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a P2P audio/video call application using C++ and WebRTC, demonstrating understanding of RTP packetization, media pipelines, signaling, and NAT traversal.

**Architecture:** Client-server model with a lightweight WebSocket signaling server for SDP/ICE exchange and libdatachannel for P2P media transport. Media pipeline: V4L2 capture → H.264 encode → RTP packetize → send / receive → jitter buffer → RTP depacketize → H.264 decode → SDL2 render. Audio pipeline follows the same pattern with ALSA and Opus.

**Tech Stack:** C++17, CMake, libdatachannel, libwebsocket, FFmpeg/libavcodec, libopus, SDL2, spdlog, Google Test

---

## File Structure

```
CrystalRTC/
├── CMakeLists.txt
├── third_party/
│   └── CMakeLists.txt
├── src/
│   ├── signaling/
│   │   ├── signaling_server.h
│   │   ├── signaling_server.cpp
│   │   ├── signaling_client.h
│   │   └── signaling_client.cpp
│   ├── transport/
│   │   ├── transport_manager.h
│   │   ├── transport_manager.cpp
│   │   └── ice_config.h
│   ├── media/
│   │   ├── video/
│   │   │   ├── v4l2_capture.h
│   │   │   ├── v4l2_capture.cpp
│   │   │   ├── h264_encoder.h
│   │   │   ├── h264_encoder.cpp
│   │   │   ├── h264_decoder.h
│   │   │   ├── h264_decoder.cpp
│   │   │   ├── sdl_renderer.h
│   │   │   └── sdl_renderer.cpp
│   │   ├── audio/
│   │   │   ├── alsa_capture.h
│   │   │   ├── alsa_capture.cpp
│   │   │   ├── opus_encoder.h
│   │   │   ├── opus_encoder.cpp
│   │   │   ├── opus_decoder.h
│   │   │   ├── opus_decoder.cpp
│   │   │   ├── sdl_audio_player.h
│   │   │   └── sdl_audio_player.cpp
│   │   └── rtp/
│   │       ├── rtp_packet.h
│   │       ├── rtp_packet.cpp
│   │       ├── rtp_packetizer.h
│   │       ├── rtp_packetizer.cpp
│   │       ├── rtp_depacketizer.h
│   │       ├── rtp_depacketizer.cpp
│   │       ├── jitter_buffer.h
│   │       └── jitter_buffer.cpp
│   ├── room/
│   │   ├── room_manager.h
│   │   └── room_manager.cpp
│   └── utils/
│       ├── logger.h
│       └── logger.cpp
├── tests/
│   ├── rtp_packet_test.cpp
│   ├── rtp_packetizer_test.cpp
│   ├── rtp_depacketizer_test.cpp
│   └── jitter_buffer_test.cpp
├── configs/
│   └── default.json
├── main_signaling_server.cpp
└── main_client.cpp
```

---

## Task 1: Project Scaffolding & CMake Setup

**Files:**
- Create: `CMakeLists.txt`
- Create: `third_party/CMakeLists.txt`
- Create: `configs/default.json`

**Learning Goal:** 理解 C++ 项目的 CMake 构建体系，学会如何通过 FetchContent 管理第三方依赖。

- [ ] **Step 1: Create top-level CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(CrystalRTC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- Third-party dependencies ---
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.21.2
)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(BUILD_TESTING ON)
FetchContent_MakeAvailable(spdlog googletest libdatachannel)

# --- nlohmann/json for signaling messages ---
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Project source ---
add_subdirectory(src)

# --- Tests ---
enable_testing()
add_subdirectory(tests)

# --- Executables ---
add_executable(crystal_signaling_server main_signaling_server.cpp)
target_link_libraries(crystal_signaling_server PRIVATE crystal_signaling crystal_utils)

add_executable(crystal_client main_client.cpp)
target_link_libraries(crystal_client PRIVATE
    crystal_signaling crystal_transport crystal_media_video crystal_media_audio
    crystal_media_rtp crystal_room crystal_utils
    datachannel spdlog::spdlog nlohmann_json::nlohmann_json
    SDL2::SDL2 SDL2::SDL2main
)
```

- [ ] **Step 2: Create src/CMakeLists.txt with subdirectory structure**

```cmake
# --- Utils library ---
add_library(crystal_utils STATIC
    utils/logger.cpp
)
target_include_directories(crystal_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_utils PUBLIC spdlog::spdlog)

# --- Signaling library ---
add_library(crystal_signaling STATIC
    signaling/signaling_server.cpp
    signaling/signaling_client.cpp
)
target_include_directories(crystal_signaling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_signaling PUBLIC crystal_utils nlohmann_json::nlohmann_json)

# --- Transport library ---
add_library(crystal_transport STATIC
    transport/transport_manager.cpp
)
target_include_directories(crystal_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_transport PUBLIC crystal_utils datachannel)

# --- Media: RTP ---
add_library(crystal_media_rtp STATIC
    media/rtp/rtp_packet.cpp
    media/rtp/rtp_packetizer.cpp
    media/rtp/rtp_depacketizer.cpp
    media/rtp/jitter_buffer.cpp
)
target_include_directories(crystal_media_rtp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_rtp PUBLIC crystal_utils)

# --- Media: Video ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
find_package(SDL2 REQUIRED)

add_library(crystal_media_video STATIC
    media/video/v4l2_capture.cpp
    media/video/h264_encoder.cpp
    media/video/h264_decoder.cpp
    media/video/sdl_renderer.cpp
)
target_include_directories(crystal_media_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_video PUBLIC
    crystal_utils crystal_media_rtp
    ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES}
    SDL2::SDL2
)

# --- Media: Audio ---
pkg_check_modules(ALSA REQUIRED alsa)
find_package(Opus REQUIRED)

add_library(crystal_media_audio STATIC
    media/audio/alsa_capture.cpp
    media/audio/opus_encoder.cpp
    media/audio/opus_decoder.cpp
    media/audio/sdl_audio_player.cpp
)
target_include_directories(crystal_media_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_audio PUBLIC
    crystal_utils crystal_media_rtp
    ${ALSA_LIBRARIES} Opus::Opus SDL2::SDL2
)

# --- Room library ---
add_library(crystal_room STATIC
    room/room_manager.cpp
)
target_include_directories(crystal_room PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_room PUBLIC crystal_utils nlohmann_json::nlohmann_json)
```

- [ ] **Step 3: Create tests/CMakeLists.txt**

```cmake
add_executable(crystal_rtp_tests
    rtp_packet_test.cpp
    rtp_packetizer_test.cpp
    rtp_depacketizer_test.cpp
    jitter_buffer_test.cpp
)
target_link_libraries(crystal_rtp_tests PRIVATE
    crystal_media_rtp crystal_utils GTest::gtest_main
)
add_test(NAME crystal_rtp_tests COMMAND crystal_rtp_tests)
```

- [ ] **Step 4: Create default config**

```json
{
    "signaling": {
        "host": "0.0.0.0",
        "port": 8765
    },
    "ice": {
        "stun_servers": ["stun:stun.l.google.com:19302"],
        "turn_servers": []
    },
    "video": {
        "width": 640,
        "height": 480,
        "fps": 30,
        "bitrate_kbps": 1000,
        "codec": "h264"
    },
    "audio": {
        "sample_rate": 48000,
        "channels": 1,
        "bitrate_kbps": 64,
        "codec": "opus",
        "frame_size": 960
    }
}
```

- [ ] **Step 5: Create placeholder main files**

`main_signaling_server.cpp`:
```cpp
#include "signaling/signaling_server.h"
#include "utils/logger.h"

int main(int argc, char* argv[]) {
    crystal::Logger::init("signaling_server");
    crystal::SignalingServer server("0.0.0.0", 8765);
    server.start();
    return 0;
}
```

`main_client.cpp`:
```cpp
#include "utils/logger.h"

int main(int argc, char* argv[]) {
    crystal::Logger::init("client");
    crystal::Logger::info("CrystalRTC client starting...");
    return 0;
}
```

- [ ] **Step 6: Create empty source directories**

```bash
mkdir -p src/{signaling,transport,media/{video,audio,rtp},room,utils}
mkdir -p tests configs third_party
```

- [ ] **Step 7: Verify CMake configures successfully**

```bash
cd /workspace && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

Expected: CMake configuration completes without errors (dependencies will be fetched).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat: project scaffolding with CMake and dependency management"
```

---

## Task 2: Logger Utility

**Files:**
- Create: `src/utils/logger.h`
- Create: `src/utils/logger.cpp`

**Learning Goal:** 理解 spdlog 日志库的使用，学会封装统一的日志接口。WebRTC 项目中日志非常重要，用于调试 ICE 连接、RTP 丢包等问题。

- [ ] **Step 1: Create logger.h**

```cpp
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
```

- [ ] **Step 2: Create logger.cpp**

```cpp
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
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_utils
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add logger utility with spdlog"
```

---

## Task 3: RTP Packet Structure

**Files:**
- Create: `src/media/rtp/rtp_packet.h`
- Create: `src/media/rtp/rtp_packet.cpp`
- Create: `tests/rtp_packet_test.cpp`

**Learning Goal:** 这是 WebRTC 面试最核心的知识点之一。理解 RTP 包头格式：V/P/X/CC/M/PT/SeqNum/Timestamp/SSRC/CSRC。RFC 3550 是必读文档。

RTP 包头格式（12 字节固定头）：
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT     |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             SSRC                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- [ ] **Step 1: Write failing test for RtpPacket**

`tests/rtp_packet_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "media/rtp/rtp_packet.h"

TEST(RtpPacketTest, DefaultConstructorHasVersion2) {
    crystal::RtpPacket pkt;
    EXPECT_EQ(pkt.version(), 2);
    EXPECT_EQ(pkt.padding(), false);
    EXPECT_EQ(pkt.extension(), false);
    EXPECT_EQ(pkt.csrcCount(), 0);
    EXPECT_EQ(pkt.marker(), false);
    EXPECT_EQ(pkt.payloadType(), 0);
    EXPECT_EQ(pkt.sequenceNumber(), 0);
    EXPECT_EQ(pkt.timestamp(), 0u);
    EXPECT_EQ(pkt.ssrc(), 0u);
}

TEST(RtpPacketTest, SetAndGetFields) {
    crystal::RtpPacket pkt;
    pkt.setMarker(true);
    pkt.setPayloadType(96);
    pkt.setSequenceNumber(12345);
    pkt.setTimestamp(90000);
    pkt.setSsrc(0xDEADBEEF);

    EXPECT_TRUE(pkt.marker());
    EXPECT_EQ(pkt.payloadType(), 96);
    EXPECT_EQ(pkt.sequenceNumber(), 12345);
    EXPECT_EQ(pkt.timestamp(), 90000u);
    EXPECT_EQ(pkt.ssrc(), 0xDEADBEEFu);
}

TEST(RtpPacketTest, SerializeAndParse) {
    crystal::RtpPacket original;
    original.setMarker(true);
    original.setPayloadType(96);
    original.setSequenceNumber(100);
    original.setTimestamp(5400000);
    original.setSsrc(0x12345678);
    original.setPayload({0x00, 0x01, 0x02, 0x03});

    auto data = original.serialize();

    crystal::RtpPacket parsed;
    ASSERT_TRUE(parsed.parse(data.data(), data.size()));

    EXPECT_EQ(parsed.version(), 2);
    EXPECT_TRUE(parsed.marker());
    EXPECT_EQ(parsed.payloadType(), 96);
    EXPECT_EQ(parsed.sequenceNumber(), 100);
    EXPECT_EQ(parsed.timestamp(), 5400000u);
    EXPECT_EQ(parsed.ssrc(), 0x12345678u);
    EXPECT_EQ(parsed.payload().size(), 4u);
    EXPECT_EQ(parsed.payload()[0], 0x00);
    EXPECT_EQ(parsed.payload()[3], 0x03);
}

TEST(RtpPacketTest, HeaderSizeIs12Bytes) {
    crystal::RtpPacket pkt;
    EXPECT_EQ(pkt.headerSize(), 12u);
}

TEST(RtpPacketTest, TotalSizeIsHeaderPlusPayload) {
    crystal::RtpPacket pkt;
    pkt.setPayload({1, 2, 3, 4, 5});
    EXPECT_EQ(pkt.totalSize(), 17u);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="RtpPacketTest.*"
```

Expected: FAIL — `rtp_packet.h` does not exist yet.

- [ ] **Step 3: Create rtp_packet.h**

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace crystal {

class RtpPacket {
public:
    RtpPacket();
    ~RtpPacket() = default;

    bool parse(const uint8_t* data, size_t size);
    std::vector<uint8_t> serialize() const;

    uint8_t version() const;
    bool padding() const;
    bool extension() const;
    uint8_t csrcCount() const;
    bool marker() const;
    uint8_t payloadType() const;
    uint16_t sequenceNumber() const;
    uint32_t timestamp() const;
    uint32_t ssrc() const;
    const std::vector<uint8_t>& payload() const;

    void setMarker(bool m);
    void setPayloadType(uint8_t pt);
    void setSequenceNumber(uint16_t seq);
    void setTimestamp(uint32_t ts);
    void setSsrc(uint32_t ssrc);
    void setPayload(const std::vector<uint8_t>& data);
    void setPayload(const uint8_t* data, size_t len);

    size_t headerSize() const;
    size_t totalSize() const;

private:
    bool marker_ = false;
    uint8_t payloadType_ = 0;
    uint16_t sequenceNumber_ = 0;
    uint32_t timestamp_ = 0;
    uint32_t ssrc_ = 0;
    std::vector<uint8_t> payload_;
};

} // namespace crystal
```

- [ ] **Step 4: Create rtp_packet.cpp**

```cpp
#include "media/rtp/rtp_packet.h"
#include "utils/logger.h"
#include <cstring>
#include <stdexcept>

namespace crystal {

RtpPacket::RtpPacket() = default;

bool RtpPacket::parse(const uint8_t* data, size_t size) {
    if (size < 12) {
        Logger::error("RTP packet too short: {} bytes", size);
        return false;
    }

    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];

    if ((byte0 >> 6) != 2) {
        Logger::error("Invalid RTP version: {}", byte0 >> 6);
        return false;
    }

    uint8_t cc = byte0 & 0x0F;
    size_t header_len = 12 + cc * 4;

    if (size < header_len) {
        Logger::error("RTP packet too short for CSRC: {} < {}", size, header_len);
        return false;
    }

    marker_ = (byte1 & 0x80) != 0;
    payloadType_ = byte1 & 0x7F;
    sequenceNumber_ = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    timestamp_ = (static_cast<uint32_t>(data[4]) << 24) |
                 (static_cast<uint32_t>(data[5]) << 16) |
                 (static_cast<uint32_t>(data[6]) << 8) |
                 data[7];
    ssrc_ = (static_cast<uint32_t>(data[8]) << 24) |
            (static_cast<uint32_t>(data[9]) << 16) |
            (static_cast<uint32_t>(data[10]) << 8) |
            data[11];

    size_t payload_len = size - header_len;
    if (payload_len > 0) {
        payload_.assign(data + header_len, data + header_len + payload_len);
    } else {
        payload_.clear();
    }

    return true;
}

std::vector<uint8_t> RtpPacket::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(12 + payload_.size());

    uint8_t byte0 = (2 << 6) | 0x00;
    buf.push_back(byte0);

    uint8_t byte1 = (marker_ ? 0x80 : 0x00) | (payloadType_ & 0x7F);
    buf.push_back(byte1);

    buf.push_back(static_cast<uint8_t>(sequenceNumber_ >> 8));
    buf.push_back(static_cast<uint8_t>(sequenceNumber_ & 0xFF));

    buf.push_back(static_cast<uint8_t>(timestamp_ >> 24));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(timestamp_ & 0xFF));

    buf.push_back(static_cast<uint8_t>(ssrc_ >> 24));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ssrc_ & 0xFF));

    buf.insert(buf.end(), payload_.begin(), payload_.end());
    return buf;
}

uint8_t RtpPacket::version() const { return 2; }
bool RtpPacket::padding() const { return false; }
bool RtpPacket::extension() const { return false; }
uint8_t RtpPacket::csrcCount() const { return 0; }
bool RtpPacket::marker() const { return marker_; }
uint8_t RtpPacket::payloadType() const { return payloadType_; }
uint16_t RtpPacket::sequenceNumber() const { return sequenceNumber_; }
uint32_t RtpPacket::timestamp() const { return timestamp_; }
uint32_t RtpPacket::ssrc() const { return ssrc_; }
const std::vector<uint8_t>& RtpPacket::payload() const { return payload_; }

void RtpPacket::setMarker(bool m) { marker_ = m; }
void RtpPacket::setPayloadType(uint8_t pt) { payloadType_ = pt & 0x7F; }
void RtpPacket::setSequenceNumber(uint16_t seq) { sequenceNumber_ = seq; }
void RtpPacket::setTimestamp(uint32_t ts) { timestamp_ = ts; }
void RtpPacket::setSsrc(uint32_t ssrc) { ssrc_ = ssrc; }

void RtpPacket::setPayload(const std::vector<uint8_t>& data) {
    payload_ = data;
}

void RtpPacket::setPayload(const uint8_t* data, size_t len) {
    payload_.assign(data, data + len);
}

size_t RtpPacket::headerSize() const { return 12; }
size_t RtpPacket::totalSize() const { return 12 + payload_.size(); }

} // namespace crystal
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="RtpPacketTest.*"
```

Expected: All 5 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add RtpPacket with parse/serialize and unit tests"
```

---

## Task 4: RTP Packetizer (H.264 FU-A)

**Files:**
- Create: `src/media/rtp/rtp_packetizer.h`
- Create: `src/media/rtp/rtp_packetizer.cpp`
- Create: `tests/rtp_packetizer_test.cpp`

**Learning Goal:** 这是面试最高频的考点之一。理解 H.264 NAL Unit 如何被拆分成 RTP 包：
- **单 NAL 包模式**：NAL 小于 MTU (1200字节)，直接放入 RTP payload
- **FU-A 分片模式**：NAL 大于 MTU，拆分为多个 Fragmentation Unit，每个 FU-A 包有 1 字节 FU Indicator + 1 字节 FU Header
- **STAP-A 聚合模式**：多个小 NAL 打包到一个 RTP 包

FU-A 格式：
```
FU Indicator (1 byte):
  forbidden_zero_bit(1) | nal_ref_idc(2) | type=28(5)

FU Header (1 byte):
  start(1) | end(1) | reserved(1) | nal_type(5)
```

- [ ] **Step 1: Write failing tests**

`tests/rtp_packetizer_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "media/rtp/rtp_packetizer.h"

TEST(RtpPacketizerTest, SmallNalPackedAsSingleUnit) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    std::vector<uint8_t> smallNal = {0x65, 0x88, 0x84, 0x00, 0x40};
    auto packets = pktizer.packetizeH264(smallNal, 3000);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets[0].marker());
    EXPECT_EQ(packets[0].payloadType(), 96);
    EXPECT_EQ(packets[0].timestamp(), 3000u);
    EXPECT_EQ(packets[0].payload()[0], 0x65);
}

TEST(RtpPacketizerTest, LargeNalFragmentedWithFUA) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    auto packets = pktizer.packetizeH264(largeNal, 5000);

    EXPECT_GT(packets.size(), 1u);

    EXPECT_EQ(packets[0].payload()[0] & 0x1F, 28);
    EXPECT_TRUE(packets[0].payload()[1] & 0x80);
    EXPECT_FALSE(packets[0].payload()[1] & 0x40);
    EXPECT_FALSE(packets[0].marker());

    for (size_t i = 1; i < packets.size() - 1; i++) {
        EXPECT_EQ(packets[i].payload()[0] & 0x1F, 28);
        EXPECT_FALSE(packets[i].payload()[1] & 0x80);
        EXPECT_FALSE(packets[i].payload()[1] & 0x40);
        EXPECT_FALSE(packets[i].marker());
    }

    EXPECT_EQ(packets.back().payload()[0] & 0x1F, 28);
    EXPECT_FALSE(packets.back().payload()[1] & 0x80);
    EXPECT_TRUE(packets.back().payload()[1] & 0x40);
    EXPECT_TRUE(packets.back().marker());

    for (const auto& pkt : packets) {
        EXPECT_EQ(pkt.timestamp(), 5000u);
        EXPECT_EQ(pkt.ssrc(), 0x12345678u);
    }
}

TEST(RtpPacketizerTest, SequenceNumbersIncrement) {
    crystal::RtpPacketizer pktizer(96, 90000, 0xABCD);

    std::vector<uint8_t> nal1 = {0x65, 0x01};
    std::vector<uint8_t> nal2 = {0x65, 0x02};

    auto pkts1 = pktizer.packetizeH264(nal1, 1000);
    auto pkts2 = pktizer.packetizeH264(nal2, 2000);

    uint16_t lastSeq = pkts1.back().sequenceNumber();
    uint16_t firstSeq = pkts2.front().sequenceNumber();
    EXPECT_EQ(firstSeq, static_cast<uint16_t>(lastSeq + 1));
}

TEST(RtpPacketizerTest, AudioPacketizeSinglePacket) {
    crystal::RtpPacketizer pktizer(97, 48000, 0xBEEFCAFE);

    std::vector<uint8_t> opusFrame(80, 0xCC);
    auto packets = pktizer.packetizeOpus(opusFrame, 960);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets[0].marker());
    EXPECT_EQ(packets[0].payloadType(), 97);
    EXPECT_EQ(packets[0].timestamp(), 960u);
    EXPECT_EQ(packets[0].payload().size(), 80u);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="RtpPacketizerTest.*"
```

Expected: FAIL — `rtp_packetizer.h` does not exist yet.

- [ ] **Step 3: Create rtp_packetizer.h**

```cpp
#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <vector>

namespace crystal {

class RtpPacketizer {
public:
    RtpPacketizer(uint8_t payloadType, uint32_t clockRate, uint32_t ssrc,
                  uint16_t startSeqNum = 0, size_t maxPacketSize = 1200);

    std::vector<RtpPacket> packetizeH264(const std::vector<uint8_t>& nalUnit,
                                          uint32_t timestamp);
    std::vector<RtpPacket> packetizeOpus(const std::vector<uint8_t>& opusFrame,
                                          uint32_t timestamp);

private:
    uint8_t payloadType_;
    uint32_t clockRate_;
    uint32_t ssrc_;
    uint16_t sequenceNumber_;
    size_t maxPacketSize_;
};

} // namespace crystal
```

- [ ] **Step 4: Create rtp_packetizer.cpp**

```cpp
#include "media/rtp/rtp_packetizer.h"
#include "utils/logger.h"

namespace crystal {

RtpPacketizer::RtpPacketizer(uint8_t payloadType, uint32_t clockRate,
                             uint32_t ssrc, uint16_t startSeqNum,
                             size_t maxPacketSize)
    : payloadType_(payloadType), clockRate_(clockRate), ssrc_(ssrc),
      sequenceNumber_(startSeqNum), maxPacketSize_(maxPacketSize) {}

std::vector<RtpPacket> RtpPacketizer::packetizeH264(
    const std::vector<uint8_t>& nalUnit, uint32_t timestamp) {

    std::vector<RtpPacket> result;

    if (nalUnit.size() <= maxPacketSize_) {
        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);
        pkt.setTimestamp(timestamp);
        pkt.setSsrc(ssrc_);
        pkt.setMarker(true);
        pkt.setPayload(nalUnit);
        result.push_back(std::move(pkt));
        return result;
    }

    uint8_t nalRefIdc = (nalUnit[0] & 0x60) >> 5;
    uint8_t nalType = nalUnit[0] & 0x1F;

    const uint8_t* nalData = nalUnit.data() + 1;
    size_t nalDataLen = nalUnit.size() - 1;
    size_t maxFragLen = maxPacketSize_ - 2;

    size_t offset = 0;
    while (offset < nalDataLen) {
        size_t fragLen = std::min(maxFragLen, nalDataLen - offset);
        bool isFirst = (offset == 0);
        bool isLast = (offset + fragLen >= nalDataLen);

        std::vector<uint8_t> fuPayload;
        fuPayload.reserve(2 + fragLen);

        uint8_t fuIndicator = (nalRefIdc << 5) | 28;
        fuPayload.push_back(fuIndicator);

        uint8_t fuHeader = (nalType & 0x1F);
        if (isFirst) fuHeader |= 0x80;
        if (isLast) fuHeader |= 0x40;
        fuPayload.push_back(fuHeader);

        fuPayload.insert(fuPayload.end(), nalData + offset,
                         nalData + offset + fragLen);

        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);
        pkt.setTimestamp(timestamp);
        pkt.setSsrc(ssrc_);
        pkt.setMarker(isLast);
        pkt.setPayload(fuPayload);
        result.push_back(std::move(pkt));

        offset += fragLen;
    }

    Logger::debug("FU-A fragmented NAL (type={} size={}) into {} RTP packets",
                  static_cast<int>(nalType), nalUnit.size(), result.size());
    return result;
}

std::vector<RtpPacket> RtpPacketizer::packetizeOpus(
    const std::vector<uint8_t>& opusFrame, uint32_t timestamp) {

    RtpPacket pkt;
    pkt.setPayloadType(payloadType_);
    pkt.setSequenceNumber(sequenceNumber_++);
    pkt.setTimestamp(timestamp);
    pkt.setSsrc(ssrc_);
    pkt.setMarker(true);
    pkt.setPayload(opusFrame);

    std::vector<RtpPacket> result;
    result.push_back(std::move(pkt));
    return result;
}

} // namespace crystal
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="RtpPacketizerTest.*"
```

Expected: All 4 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add RtpPacketizer with H.264 FU-A fragmentation and Opus support"
```

---

## Task 5: RTP Depacketizer

**Files:**
- Create: `src/media/rtp/rtp_depacketizer.h`
- Create: `src/media/rtp/rtp_depacketizer.cpp`
- Create: `tests/rtp_depacketizer_test.cpp`

**Learning Goal:** 理解 RTP 解包的逆过程——将 FU-A 分片重组为完整的 NAL Unit。面试常问：如何判断一个 RTP 包是 FU-A 的起始/中间/结束包？如何重组？

- [ ] **Step 1: Write failing tests**

`tests/rtp_depacketizer_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "media/rtp/rtp_depacketizer.h"
#include "media/rtp/rtp_packetizer.h"

TEST(RtpDepacketizerTest, SingleNalReconstructed) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> nal = {0x65, 0x88, 0x84, 0x00, 0x40};
    auto packets = pktizer.packetizeH264(nal, 3000);

    for (const auto& pkt : packets) {
        auto nals = depktizer.depacketizeH264(pkt);
        ASSERT_EQ(nals.size(), 1u);
        EXPECT_EQ(nals[0], nal);
    }
}

TEST(RtpDepacketizerTest, FUAFragmentsReassembled) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    auto packets = pktizer.packetizeH264(largeNal, 5000);

    std::vector<std::vector<uint8_t>> allNals;
    for (const auto& pkt : packets) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }

    ASSERT_EQ(allNals.size(), 1u);
    EXPECT_EQ(allNals[0], largeNal);
}

TEST(RtpDepacketizerTest, MultipleNalsProduceMultipleOutputs) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> nal1 = {0x65, 0x01, 0x02};
    std::vector<uint8_t> nal2 = {0x65, 0x03, 0x04};

    auto pkts1 = pktizer.packetizeH264(nal1, 1000);
    auto pkts2 = pktizer.packetizeH264(nal2, 2000);

    std::vector<std::vector<uint8_t>> allNals;
    for (const auto& pkt : pkts1) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }
    for (const auto& pkt : pkts2) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }

    ASSERT_EQ(allNals.size(), 2u);
    EXPECT_EQ(allNals[0], nal1);
    EXPECT_EQ(allNals[1], nal2);
}

TEST(RtpDepacketizerTest, OpusFrameExtracted) {
    crystal::RtpPacketizer pktizer(97, 48000, 0xBEEF);
    crystal::RtpDepacketizer depktizer;

    std::vector<uint8_t> opusFrame(80, 0xCC);
    auto packets = pktizer.packetizeOpus(opusFrame, 960);

    ASSERT_EQ(packets.size(), 1u);
    auto frames = depktizer.depacketizeOpus(packets[0]);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0], opusFrame);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="RtpDepacketizerTest.*"
```

Expected: FAIL — `rtp_depacketizer.h` does not exist yet.

- [ ] **Step 3: Create rtp_depacketizer.h**

```cpp
#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <vector>

namespace crystal {

class RtpDepacketizer {
public:
    RtpDepacketizer();

    std::vector<std::vector<uint8_t>> depacketizeH264(const RtpPacket& pkt);
    std::vector<std::vector<uint8_t>> depacketizeOpus(const RtpPacket& pkt);

private:
    std::vector<uint8_t> fuBuffer_;
    bool fuStarted_ = false;
};

} // namespace crystal
```

- [ ] **Step 4: Create rtp_depacketizer.cpp**

```cpp
#include "media/rtp/rtp_depacketizer.h"
#include "utils/logger.h"

namespace crystal {

RtpDepacketizer::RtpDepacketizer() = default;

std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeH264(
    const RtpPacket& pkt) {

    std::vector<std::vector<uint8_t>> result;
    const auto& payload = pkt.payload();

    if (payload.empty()) return result;

    uint8_t nalType = payload[0] & 0x1F;

    if (nalType >= 1 && nalType <= 23) {
        if (fuStarted_) {
            Logger::warn("Dropping incomplete FU-A buffer on single NAL arrival");
            fuStarted_ = false;
            fuBuffer_.clear();
        }
        result.push_back(payload);
    } else if (nalType == 28) {
        if (payload.size() < 2) {
            Logger::warn("FU-A packet too short");
            return result;
        }

        uint8_t fuHeader = payload[1];
        bool startBit = (fuHeader & 0x80) != 0;
        bool endBit = (fuHeader & 0x40) != 0;
        uint8_t originalNalType = fuHeader & 0x1F;

        if (startBit) {
            if (fuStarted_) {
                Logger::warn("New FU-A start while previous incomplete, dropping old");
            }
            fuStarted_ = true;
            fuBuffer_.clear();
            uint8_t nalRefIdc = (payload[0] & 0x60);
            uint8_t reconstructedNal = nalRefIdc | originalNalType;
            fuBuffer_.push_back(reconstructedNal);
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2,
                                 payload.end());
            }
        } else if (fuStarted_) {
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2,
                                 payload.end());
            }

            if (endBit) {
                fuStarted_ = false;
                result.push_back(std::move(fuBuffer_));
                fuBuffer_.clear();
            }
        } else {
            Logger::warn("FU-A middle/end without start, dropping");
        }
    } else if (nalType == 24) {
        Logger::debug("STAP-A packet received (not yet fully supported, extracting single NAL)");
    } else {
        Logger::warn("Unsupported NAL type in RTP: {}", static_cast<int>(nalType));
    }

    return result;
}

std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeOpus(
    const RtpPacket& pkt) {
    std::vector<std::vector<uint8_t>> result;
    result.push_back(pkt.payload());
    return result;
}

} // namespace crystal
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="RtpDepacketizerTest.*"
```

Expected: All 4 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add RtpDepacketizer with FU-A reassembly and Opus support"
```

---

## Task 6: Jitter Buffer

**Files:**
- Create: `src/media/rtp/jitter_buffer.h`
- Create: `src/media/rtp/jitter_buffer.cpp`
- Create: `tests/jitter_buffer_test.cpp`

**Learning Goal:** Jitter Buffer 是 WebRTC 接收端的核心组件。理解：
- 为什么需要 Jitter Buffer？网络抖动导致包到达时间不一致
- 如何检测丢包？sequence number 不连续
- 如何计算抖动？RFC 3550 中的抖动计算公式
- 缓冲策略：静态 vs 自适应

- [ ] **Step 1: Write failing tests**

`tests/jitter_buffer_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "media/rtp/jitter_buffer.h"

TEST(JitterBufferTest, InsertAndConsumeInOrder) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2, pkt3;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(101);
    pkt2.setTimestamp(93000);
    pkt3.setSequenceNumber(102);
    pkt3.setTimestamp(96000);

    jb.insert(pkt1);
    jb.insert(pkt2);
    jb.insert(pkt3);

    auto out = jb.consume();
    ASSERT_GE(out.size(), 3u);
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 101);
    EXPECT_EQ(out[2].sequenceNumber(), 102);
}

TEST(JitterBufferTest, ReordersOutOfOrderPackets) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2, pkt3;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(102);
    pkt2.setTimestamp(96000);
    pkt3.setSequenceNumber(101);
    pkt3.setTimestamp(93000);

    jb.insert(pkt1);
    jb.insert(pkt2);
    jb.insert(pkt3);

    auto out = jb.consume();
    ASSERT_GE(out.size(), 3u);
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 101);
    EXPECT_EQ(out[2].sequenceNumber(), 102);
}

TEST(JitterBufferTest, DetectsPacketLoss) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(103);
    pkt2.setTimestamp(99000);

    jb.insert(pkt1);
    jb.insert(pkt2);

    auto out = jb.consume();
    EXPECT_GT(jb.lostPacketCount(), 0u);
}

TEST(JitterBufferTest, StatsInitializedToZero) {
    crystal::JitterBuffer jb(40);
    EXPECT_EQ(jb.lostPacketCount(), 0u);
    EXPECT_DOUBLE_EQ(jb.lossRate(), 0.0);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="JitterBufferTest.*"
```

Expected: FAIL — `jitter_buffer.h` does not exist yet.

- [ ] **Step 3: Create jitter_buffer.h**

```cpp
#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <map>
#include <vector>

namespace crystal {

class JitterBuffer {
public:
    explicit JitterBuffer(uint32_t targetDelayMs = 40);

    void insert(const RtpPacket& pkt);
    std::vector<RtpPacket> consume();

    uint64_t lostPacketCount() const;
    uint64_t receivedPacketCount() const;
    double lossRate() const;

private:
    uint32_t targetDelayMs_;
    std::map<uint16_t, RtpPacket> buffer_;
    uint16_t expectedSeq_ = 0;
    bool firstPacket_ = true;
    uint64_t lostCount_ = 0;
    uint64_t receivedCount_ = 0;
};

} // namespace crystal
```

- [ ] **Step 4: Create jitter_buffer.cpp**

```cpp
#include "media/rtp/jitter_buffer.h"
#include "utils/logger.h"

namespace crystal {

JitterBuffer::JitterBuffer(uint32_t targetDelayMs)
    : targetDelayMs_(targetDelayMs) {}

void JitterBuffer::insert(const RtpPacket& pkt) {
    receivedCount_++;

    if (firstPacket_) {
        expectedSeq_ = pkt.sequenceNumber();
        firstPacket_ = false;
        buffer_[pkt.sequenceNumber()] = pkt;
        Logger::debug("JitterBuffer: first packet seq={}", pkt.sequenceNumber());
        return;
    }

    uint16_t seq = pkt.sequenceNumber();
    int16_t diff = static_cast<int16_t>(seq - expectedSeq_);

    if (diff == 0) {
        buffer_[seq] = pkt;
    } else if (diff > 0) {
        if (diff > 1) {
            lostCount_ += static_cast<uint64_t>(diff - 1);
            Logger::debug("JitterBuffer: gap detected, expected {} got {}, {} lost",
                          expectedSeq_, seq, diff - 1);
        }
        buffer_[seq] = pkt;
    } else {
        Logger::debug("JitterBuffer: late/duplicate packet seq={}", seq);
    }
}

std::vector<RtpPacket> JitterBuffer::consume() {
    std::vector<RtpPacket> result;

    if (buffer_.empty()) return result;

    auto it = buffer_.begin();
    while (it != buffer_.end()) {
        uint16_t seq = it->first;
        int16_t diff = static_cast<int16_t>(seq - expectedSeq_);

        if (diff <= 0) {
            result.push_back(it->second);
            expectedSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else if (diff <= 3) {
            expectedSeq_ = seq;
            result.push_back(it->second);
            expectedSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else {
            ++it;
        }
    }

    return result;
}

uint64_t JitterBuffer::lostPacketCount() const { return lostCount_; }
uint64_t JitterBuffer::receivedPacketCount() const { return receivedCount_; }

double JitterBuffer::lossRate() const {
    uint64_t total = receivedCount_ + lostCount_;
    if (total == 0) return 0.0;
    return static_cast<double>(lostCount_) / static_cast<double>(total);
}

} // namespace crystal
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests --gtest_filter="JitterBufferTest.*"
```

Expected: All 4 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add JitterBuffer with reordering and loss detection"
```

---

## Task 7: Signaling Server

**Files:**
- Create: `src/signaling/signaling_server.h`
- Create: `src/signaling/signaling_server.cpp`

**Learning Goal:** 理解 WebRTC 信令流程——为什么需要信令服务器？因为 WebRTC 本身不定义信令协议，SDP 和 ICE 候选需要通过其他通道交换。信令服务器就是这个通道。

- [ ] **Step 1: Create signaling_server.h**

```cpp
#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>

struct lws;
struct lws_context;

namespace crystal {

struct SignalingMessage {
    std::string type;
    std::string sdp;
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
    std::string to;
    std::string room;
    std::string peerId;
};

class SignalingServer {
public:
    SignalingServer(const std::string& host, uint16_t port);
    ~SignalingServer();

    void start();
    void stop();

    using MessageCallback = std::function<void(const std::string& peerId,
                                               const SignalingMessage& msg)>;
    void onMessage(MessageCallback cb);

private:
    void handleMessage(const std::string& peerId, const std::string& data);
    void sendToPeer(const std::string& peerId, const std::string& data);
    void broadcastToRoom(const std::string& room, const std::string& data,
                         const std::string& excludePeerId);

    std::string host_;
    uint16_t port_;
    lws_context* context_ = nullptr;
    bool running_ = false;

    std::mutex peersMutex_;
    struct PeerInfo {
        struct lws* wsi;
        std::string room;
    };
    std::unordered_map<std::string, PeerInfo> peers_;

    MessageCallback messageCallback_;
};

} // namespace crystal
```

- [ ] **Step 2: Create signaling_server.cpp**

```cpp
#include "signaling/signaling_server.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <libwebsockets.h>
#include <thread>
#include <random>
#include <cstring>

namespace crystal {

using json = nlohmann::json;

static std::string generatePeerId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    return std::to_string(dist(gen));
}

struct PerSessionData {
    std::string peerId;
    std::vector<uint8_t> recvBuffer;
};

static SignalingServer* g_server = nullptr;

static int callbackProtocol(struct lws* wsi, enum lws_callback_reasons reason,
                            void* user, void* in, size_t len) {
    auto* psd = static_cast<PerSessionData*>(user);

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        psd = new PerSessionData();
        psd->peerId = generatePeerId();
        lws_wsi_user(wsi) = psd;
        if (g_server) {
            Logger::info("Signaling: peer connected id={}", psd->peerId);
        }
        break;
    }
    case LWS_CALLBACK_RECEIVE: {
        if (!psd || !g_server) break;
        std::string data(static_cast<const char*>(in), len);
        Logger::debug("Signaling: received from {}: {} bytes", psd->peerId, len);
        g_server->onMessage([&](const std::string& peerId,
                                const SignalingMessage& msg) {
            json j;
            j["type"] = msg.type;
            if (!msg.sdp.empty()) j["sdp"] = msg.sdp;
            if (!msg.candidate.empty()) j["candidate"] = msg.candidate;
            if (!msg.sdpMid.empty()) j["sdpMid"] = msg.sdpMid;
            if (msg.sdpMLineIndex >= 0) j["sdpMLineIndex"] = msg.sdpMLineIndex;
            if (!msg.to.empty()) j["to"] = msg.to;
            if (!msg.room.empty()) j["room"] = msg.room;
            if (!msg.peerId.empty()) j["peerId"] = msg.peerId;
        });
        break;
    }
    case LWS_CALLBACK_CLOSED: {
        if (psd) {
            Logger::info("Signaling: peer disconnected id={}", psd->peerId);
            delete psd;
            lws_wsi_user(wsi) = nullptr;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {"signaling", callbackProtocol, sizeof(PerSessionData*), 4096, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}
};

SignalingServer::SignalingServer(const std::string& host, uint16_t port)
    : host_(host), port_(port) {}

SignalingServer::~SignalingServer() {
    stop();
}

void SignalingServer::start() {
    g_server = this;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port_;
    info.iface = host_.c_str();
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    context_ = lws_create_context(&info);
    if (!context_) {
        Logger::error("Failed to create libwebsocket context");
        return;
    }

    Logger::info("Signaling server started on {}:{}", host_, port_);
    running_ = true;

    while (running_) {
        lws_service(context_, 50);
    }

    lws_context_destroy(context_);
    context_ = nullptr;
    g_server = nullptr;
}

void SignalingServer::stop() {
    running_ = false;
}

void SignalingServer::onMessage(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void SignalingServer::handleMessage(const std::string& peerId,
                                     const std::string& data) {
    try {
        json j = json::parse(data);
        SignalingMessage msg;
        msg.type = j.value("type", "");
        msg.sdp = j.value("sdp", "");
        msg.candidate = j.value("candidate", "");
        msg.sdpMid = j.value("sdpMid", "");
        msg.sdpMLineIndex = j.value("sdpMLineIndex", 0);
        msg.to = j.value("to", "");
        msg.room = j.value("room", "");
        msg.peerId = peerId;

        if (msg.type == "join") {
            std::lock_guard<std::mutex> lock(peersMutex_);
            peers_[peerId].room = msg.room;
            Logger::info("Peer {} joined room {}", peerId, msg.room);

            json notify;
            notify["type"] = "peer_joined";
            notify["peerId"] = peerId;
            broadcastToRoom(msg.room, notify.dump(), peerId);

            json welcome;
            welcome["type"] = "joined";
            welcome["peerId"] = peerId;
            sendToPeer(peerId, welcome.dump());
        } else if (msg.type == "offer" || msg.type == "answer" ||
                   msg.type == "candidate") {
            if (!msg.to.empty()) {
                json forward = j;
                forward["from"] = peerId;
                sendToPeer(msg.to, forward.dump());
            }
        } else if (msg.type == "leave") {
            std::lock_guard<std::mutex> lock(peersMutex_);
            std::string room = peers_[peerId].room;
            peers_.erase(peerId);

            json notify;
            notify["type"] = "peer_left";
            notify["peerId"] = peerId;
            broadcastToRoom(room, notify.dump(), "");
        }

        if (messageCallback_) {
            messageCallback_(peerId, msg);
        }
    } catch (const json::exception& e) {
        Logger::error("Failed to parse signaling message: {}", e.what());
    }
}

void SignalingServer::sendToPeer(const std::string& peerId,
                                  const std::string& data) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end() && it->second.wsi) {
        std::vector<uint8_t> buf(LWS_PRE + data.size());
        std::memcpy(buf.data() + LWS_PRE, data.data(), data.size());
        lws_write(it->second.wsi, buf.data() + LWS_PRE, data.size(),
                  LWS_WRITE_TEXT);
    }
}

void SignalingServer::broadcastToRoom(const std::string& room,
                                       const std::string& data,
                                       const std::string& excludePeerId) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    for (const auto& [id, info] : peers_) {
        if (id != excludePeerId && info.room == room) {
            sendToPeer(id, data);
        }
    }
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_signaling
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add WebSocket signaling server with room management"
```

---

## Task 8: Signaling Client

**Files:**
- Create: `src/signaling/signaling_client.h`
- Create: `src/signaling/signaling_client.cpp`

**Learning Goal:** 理解客户端如何与信令服务器交互——连接、加入房间、发送 SDP/ICE、接收对端消息。

- [ ] **Step 1: Create signaling_client.h**

```cpp
#pragma once

#include "signaling/signaling_server.h"
#include <string>
#include <functional>
#include <atomic>
#include <thread>

struct lws;
struct lws_context;

namespace crystal {

class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    bool connect(const std::string& url, uint16_t port);
    void disconnect();

    void joinRoom(const std::string& room);
    void sendOffer(const std::string& sdp, const std::string& to);
    void sendAnswer(const std::string& sdp, const std::string& to);
    void sendCandidate(const std::string& candidate,
                       const std::string& sdpMid,
                       int sdpMLineIndex,
                       const std::string& to);
    void leaveRoom();

    using MessageCallback = std::function<void(const SignalingMessage& msg)>;
    void onMessage(MessageCallback cb);

    const std::string& peerId() const { return peerId_; }

private:
    void serviceThread();
    void handleMessage(const std::string& data);

    lws_context* context_ = nullptr;
    lws* wsi_ = nullptr;
    std::atomic<bool> connected_{false};
    std::thread serviceThread_;
    std::string peerId_;
    std::string currentRoom_;
    MessageCallback messageCallback_;

    std::mutex sendMutex_;
    std::vector<std::string> pendingSends_;
};

} // namespace crystal
```

- [ ] **Step 2: Create signaling_client.cpp**

```cpp
#include "signaling/signaling_client.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <libwebsockets.h>
#include <cstring>

namespace crystal {

using json = nlohmann::json;

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    disconnect();
}

bool SignalingClient::connect(const std::string& url, uint16_t port) {
    struct lws_context_creation_info ctxInfo;
    memset(&ctxInfo, 0, sizeof(ctxInfo));
    ctxInfo.port = CONTEXT_PORT_NO_LISTEN;
    ctxInfo.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context_ = lws_create_context(&ctxInfo);
    if (!context_) {
        Logger::error("Failed to create WebSocket client context");
        return false;
    }

    struct lws_client_connect_info ccInfo;
    memset(&ccInfo, 0, sizeof(ccInfo));
    ccInfo.context = context_;
    ccInfo.address = url.c_str();
    ccInfo.port = port;
    ccInfo.path = "/";
    ccInfo.host = url.c_str();
    ccInfo.origin = url.c_str();
    ccInfo.protocol = "signaling";

    wsi_ = lws_client_connect_via_info(&ccInfo);
    if (!wsi_) {
        Logger::error("Failed to connect to signaling server");
        lws_context_destroy(context_);
        context_ = nullptr;
        return false;
    }

    connected_ = true;
    serviceThread_ = std::thread(&SignalingClient::serviceThread, this);
    Logger::info("Connected to signaling server at {}:{}", url, port);
    return true;
}

void SignalingClient::disconnect() {
    connected_ = false;
    if (serviceThread_.joinable()) {
        serviceThread_.join();
    }
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    wsi_ = nullptr;
}

void SignalingClient::serviceThread() {
    while (connected_) {
        lws_service(context_, 50);
    }
}

void SignalingClient::joinRoom(const std::string& room) {
    currentRoom_ = room;
    json j;
    j["type"] = "join";
    j["room"] = room;
    handleMessage(j.dump());
}

void SignalingClient::sendOffer(const std::string& sdp,
                                 const std::string& to) {
    json j;
    j["type"] = "offer";
    j["sdp"] = sdp;
    j["to"] = to;
    handleMessage(j.dump());
}

void SignalingClient::sendAnswer(const std::string& sdp,
                                  const std::string& to) {
    json j;
    j["type"] = "answer";
    j["sdp"] = sdp;
    j["to"] = to;
    handleMessage(j.dump());
}

void SignalingClient::sendCandidate(const std::string& candidate,
                                     const std::string& sdpMid,
                                     int sdpMLineIndex,
                                     const std::string& to) {
    json j;
    j["type"] = "candidate";
    j["candidate"] = candidate;
    j["sdpMid"] = sdpMid;
    j["sdpMLineIndex"] = sdpMLineIndex;
    j["to"] = to;
    handleMessage(j.dump());
}

void SignalingClient::leaveRoom() {
    json j;
    j["type"] = "leave";
    j["room"] = currentRoom_;
    handleMessage(j.dump());
    currentRoom_.clear();
}

void SignalingClient::onMessage(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void SignalingClient::handleMessage(const std::string& data) {
    try {
        json j = json::parse(data);
        SignalingMessage msg;
        msg.type = j.value("type", "");
        msg.sdp = j.value("sdp", "");
        msg.candidate = j.value("candidate", "");
        msg.sdpMid = j.value("sdpMid", "");
        msg.sdpMLineIndex = j.value("sdpMLineIndex", 0);
        msg.to = j.value("to", "");
        msg.room = j.value("room", "");
        msg.peerId = j.value("peerId", "");
        if (msg.peerId.empty()) msg.peerId = j.value("from", "");

        if (msg.type == "joined") {
            peerId_ = msg.peerId;
            Logger::info("Joined signaling server, peerId={}", peerId_);
        }

        if (messageCallback_) {
            messageCallback_(msg);
        }
    } catch (const json::exception& e) {
        Logger::error("Failed to parse signaling message: {}", e.what());
    }
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_signaling
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add signaling client with WebSocket connection"
```

---

## Task 9: Transport Layer (libdatachannel Wrapper)

**Files:**
- Create: `src/transport/ice_config.h`
- Create: `src/transport/transport_manager.h`
- Create: `src/transport/transport_manager.cpp`

**Learning Goal:** 理解 WebRTC 的核心传输流程：
1. ICE 候选收集（Host/SRFLX/Relay）
2. DTLS 握手建立安全通道
3. SRTP 密钥从 DTLS 中导出
4. libdatachannel 如何封装这些流程

- [ ] **Step 1: Create ice_config.h**

```cpp
#pragma once

#include <string>
#include <vector>

namespace crystal {

struct IceConfig {
    std::vector<std::string> stunServers;
    std::vector<std::string> turnServers;
    std::string turnUsername;
    std::string turnPassword;

    static IceConfig defaultConfig() {
        IceConfig cfg;
        cfg.stunServers = {"stun:stun.l.google.com:19302"};
        return cfg;
    }
};

} // namespace crystal
```

- [ ] **Step 2: Create transport_manager.h**

```cpp
#pragma once

#include "transport/ice_config.h"
#include <rtc/rtc.hpp>
#include <string>
#include <functional>
#include <memory>
#include <mutex>

namespace crystal {

class PeerConnection {
public:
    using IceCandidateCallback = std::function<void(const std::string& candidate,
                                                     const std::string& sdpMid,
                                                     int sdpMLineIndex)>;
    using TrackCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using StateCallback = std::function<void(rtc::PeerConnection::State state)>;

    explicit PeerConnection(const IceConfig& config);
    ~PeerConnection();

    std::string createOffer();
    std::string createAnswer();
    void setRemoteDescription(const std::string& sdp, const std::string& type);
    void addIceCandidate(const std::string& candidate,
                         const std::string& sdpMid, int sdpMLineIndex);

    void sendMedia(const uint8_t* data, size_t len);
    void sendMedia(const std::vector<uint8_t>& data);

    void onIceCandidate(IceCandidateCallback cb);
    void onTrack(TrackCallback cb);
    void onStateChange(StateCallback cb);

    rtc::PeerConnection::State state() const;
    bool isConnected() const;

private:
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> track_;
    IceConfig config_;
    IceCandidateCallback iceCandidateCb_;
    TrackCallback trackCb_;
    StateCallback stateCb_;
};

class TransportManager {
public:
    explicit TransportManager(const IceConfig& config = IceConfig::defaultConfig());

    std::shared_ptr<PeerConnection> createPeerConnection();
    void destroyPeerConnection(const std::string& id);

private:
    IceConfig config_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerConnection>> connections_;
};

} // namespace crystal
```

- [ ] **Step 3: Create transport_manager.cpp**

```cpp
#include "transport/transport_manager.h"
#include "utils/logger.h"
#include <rtc/rtc.hpp>
#include <random>
#include <sstream>

namespace crystal {

PeerConnection::PeerConnection(const IceConfig& config) : config_(config) {
    rtc::InitLogger(rtc::LogLevel::Warning);

    rtc::Configuration rtcConfig;
    for (const auto& stun : config_.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }
    for (const auto& turn : config_.turnServers) {
        rtc::IceServer turnServer(turn, config_.turnUsername,
                                   config_.turnPassword);
        rtcConfig.iceServers.push_back(turnServer);
    }

    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

    pc_->onLocalDescription([this](const rtc::Description& desc) {
        Logger::debug("Local description created: type={}",
                       std::string(desc.type()));
    });

    pc_->onLocalCandidate([this](const rtc::Candidate& cand) {
        Logger::debug("Local ICE candidate: {}",
                       std::string(cand.candidate()));
        if (iceCandidateCb_) {
            iceCandidateCb_(std::string(cand.candidate()),
                           std::string(cand.mid()), 0);
        }
    });

    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        Logger::info("PeerConnection state: {}", static_cast<int>(state));
        if (stateCb_) stateCb_(state);
    });

    pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        Logger::debug("ICE gathering state: {}", static_cast<int>(state));
    });
}

PeerConnection::~PeerConnection() {
    if (pc_) {
        pc_->close();
    }
}

std::string PeerConnection::createOffer() {
    auto track = pc_->addTrack(rtc::Description::Video("video",
                                 rtc::Description::Direction::SendRecv));
    if (track) {
        track_ = track;
        track_->onMessage([this](const rtc::binary& data) {
            if (trackCb_) {
                trackCb_(std::vector<uint8_t>(data.begin(), data.end()));
            }
        });
    }

    auto desc = pc_->localDescription();
    if (desc) {
        return std::string(*desc);
    }

    rtc::Description offer = pc_->localDescription().value_or(
        rtc::Description("offer", rtc::Description::Type::Offer));
    return std::string(offer);
}

std::string PeerConnection::createAnswer() {
    auto desc = pc_->localDescription();
    if (desc) {
        return std::string(*desc);
    }
    return "";
}

void PeerConnection::setRemoteDescription(const std::string& sdp,
                                           const std::string& type) {
    rtc::Description desc(sdp, type == "offer" ? rtc::Description::Type::Offer
                                                : rtc::Description::Type::Answer);
    pc_->setRemoteDescription(desc);

    if (!track_) {
        pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
            track_ = track;
            track_->onMessage([this](const rtc::binary& data) {
                if (trackCb_) {
                    trackCb_(std::vector<uint8_t>(data.begin(), data.end()));
                }
            });
        });
    }
}

void PeerConnection::addIceCandidate(const std::string& candidate,
                                      const std::string& sdpMid,
                                      int sdpMLineIndex) {
    rtc::Candidate cand(candidate, sdpMid);
    pc_->addRemoteCandidate(cand);
}

void PeerConnection::sendMedia(const uint8_t* data, size_t len) {
    if (track_ && track_->isOpen()) {
        track_->send(reinterpret_cast<const std::byte*>(data), len);
    }
}

void PeerConnection::sendMedia(const std::vector<uint8_t>& data) {
    sendMedia(data.data(), data.size());
}

void PeerConnection::onIceCandidate(IceCandidateCallback cb) {
    iceCandidateCb_ = std::move(cb);
}

void PeerConnection::onTrack(TrackCallback cb) {
    trackCb_ = std::move(cb);
}

void PeerConnection::onStateChange(StateCallback cb) {
    stateCb_ = std::move(cb);
}

rtc::PeerConnection::State PeerConnection::state() const {
    return pc_->state();
}

bool PeerConnection::isConnected() const {
    return pc_->state() == rtc::PeerConnection::State::Connected;
}

TransportManager::TransportManager(const IceConfig& config)
    : config_(config) {}

std::shared_ptr<PeerConnection> TransportManager::createPeerConnection() {
    std::lock_guard<std::mutex> lock(mutex_);
    static int counter = 0;
    std::string id = "pc_" + std::to_string(counter++);
    auto pc = std::make_shared<PeerConnection>(config_);
    connections_[id] = pc;
    Logger::info("Created PeerConnection: {}", id);
    return pc;
}

void TransportManager::destroyPeerConnection(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(id);
    Logger::info("Destroyed PeerConnection: {}", id);
}

} // namespace crystal
```

- [ ] **Step 4: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_transport
```

Expected: Builds successfully.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: add transport layer with libdatachannel PeerConnection wrapper"
```

---

## Task 10: H.264 Encoder

**Files:**
- Create: `src/media/video/h264_encoder.h`
- Create: `src/media/video/h264_encoder.cpp`

**Learning Goal:** 理解视频编码的核心概念：
- **NAL Unit 类型**：SPS(7)/PPS(8)/IDR(5)/P-Slice(1)
- **编码参数**：码率控制(CBR/VBR)、GOP 大小、B帧
- **FFmpeg 编码 API**：avcodec_send_frame / avcodec_receive_packet

- [ ] **Step 1: Create h264_encoder.h**

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace crystal {

struct H264EncoderConfig {
    int width = 640;
    int height = 480;
    int fps = 30;
    int bitrateKbps = 1000;
    int gopSize = 30;
};

class H264Encoder {
public:
    using EncodedCallback = std::function<void(const uint8_t* nalData,
                                                size_t nalLen)>;

    explicit H264Encoder(const H264EncoderConfig& config);
    ~H264Encoder();

    bool init();
    void encode(const uint8_t* yuvData, size_t len);
    void onEncoded(EncodedCallback cb);

private:
    void processPacket(AVPacket* pkt);

    H264EncoderConfig config_;
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    int64_t pts_ = 0;
    EncodedCallback encodedCb_;
};

} // namespace crystal
```

- [ ] **Step 2: Create h264_encoder.cpp**

```cpp
#include "media/video/h264_encoder.h"
#include "utils/logger.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

namespace crystal {

H264Encoder::H264Encoder(const H264EncoderConfig& config)
    : config_(config) {}

H264Encoder::~H264Encoder() {
    if (frame_) av_frame_free(&frame_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

bool H264Encoder::init() {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        Logger::error("H264 encoder not found");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        Logger::error("Failed to allocate H264 encoder context");
        return false;
    }

    codecCtx_->bit_rate = config_.bitrateKbps * 1000;
    codecCtx_->width = config_.width;
    codecCtx_->height = config_.height;
    codecCtx_->time_base = {1, config_.fps};
    codecCtx_->framerate = {config_.fps, 1};
    codecCtx_->gop_size = config_.gopSize;
    codecCtx_->max_b_frames = 0;
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(codecCtx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        Logger::error("Failed to open H264 encoder: {}", ret);
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = config_.width;
    frame_->height = config_.height;
    av_frame_get_buffer(frame_, 0);

    Logger::info("H264 encoder initialized: {}x{} @ {}kbps",
                 config_.width, config_.height, config_.bitrateKbps);
    return true;
}

void H264Encoder::encode(const uint8_t* yuvData, size_t len) {
    int ySize = config_.width * config_.height;
    int uvSize = ySize / 4;
    int expectedLen = ySize + 2 * uvSize;

    if (static_cast<int>(len) < expectedLen) {
        Logger::warn("YUV data too short: {} < {}", len, expectedLen);
        return;
    }

    frame_->data[0] = const_cast<uint8_t*>(yuvData);
    frame_->data[1] = const_cast<uint8_t*>(yuvData + ySize);
    frame_->data[2] = const_cast<uint8_t*>(yuvData + ySize + uvSize);
    frame_->linesize[0] = config_.width;
    frame_->linesize[1] = config_.width / 2;
    frame_->linesize[2] = config_.width / 2;
    frame_->pts = pts_++;

    int ret = avcodec_send_frame(codecCtx_, frame_);
    if (ret < 0) {
        Logger::warn("Failed to send frame to encoder: {}", ret);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        processPacket(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void H264Encoder::processPacket(AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    int size = pkt->size;
    int offset = 0;

    while (offset < size) {
        if (offset + 4 > size) break;

        int nalSize = (data[offset] << 24) | (data[offset + 1] << 16) |
                      (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;

        if (offset + nalSize > size) break;

        if (encodedCb_) {
            encodedCb_(data + offset, nalSize);
        }
        offset += nalSize;
    }
}

void H264Encoder::onEncoded(EncodedCallback cb) {
    encodedCb_ = std::move(cb);
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_media_video
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add H.264 encoder with FFmpeg libx264"
```

---

## Task 11: H.264 Decoder

**Files:**
- Create: `src/media/video/h264_decoder.h`
- Create: `src/media/video/h264_decoder.cpp`

**Learning Goal:** 理解视频解码流程——解码器需要先收到 SPS/PPS 才能初始化，然后才能解码 IDR 帧和 P 帧。

- [ ] **Step 1: Create h264_decoder.h**

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace crystal {

class H264Decoder {
public:
    using DecodedCallback = std::function<void(const uint8_t* yuvData,
                                                int width, int height)>;

    H264Decoder();
    ~H264Decoder();

    bool init();
    void decode(const uint8_t* nalData, size_t nalLen);
    void onDecoded(DecodedCallback cb);

private:
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool initialized_ = false;
    DecodedCallback decodedCb_;
};

} // namespace crystal
```

- [ ] **Step 2: Create h264_decoder.cpp**

```cpp
#include "media/video/h264_decoder.h"
#include "utils/logger.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <cstring>

namespace crystal {

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() {
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

bool H264Decoder::init() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        Logger::error("H264 decoder not found");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        Logger::error("Failed to allocate H264 decoder context");
        return false;
    }

    codecCtx_->flags2 |= AV_CODEC_FLAG2_FAST;

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        Logger::error("Failed to open H264 decoder: {}", ret);
        return false;
    }

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    initialized_ = true;

    Logger::info("H264 decoder initialized");
    return true;
}

void H264Decoder::decode(const uint8_t* nalData, size_t nalLen) {
    if (!initialized_) return;

    std::vector<uint8_t> annexB;
    annexB.reserve(4 + nalLen);
    annexB.push_back(0x00);
    annexB.push_back(0x00);
    annexB.push_back(0x00);
    annexB.push_back(0x01);
    annexB.insert(annexB.end(), nalData, nalData + nalLen);

    pkt_->data = annexB.data();
    pkt_->size = static_cast<int>(annexB.size());

    int ret = avcodec_send_packet(codecCtx_, pkt_);
    if (ret < 0) {
        Logger::debug("Failed to send packet to decoder: {}", ret);
        return;
    }

    while (avcodec_receive_frame(codecCtx_, frame_) == 0) {
        if (decodedCb_) {
            decodedCb_(frame_->data[0], frame_->width, frame_->height);
        }
    }
}

void H264Decoder::onDecoded(DecodedCallback cb) {
    decodedCb_ = std::move(cb);
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_media_video
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add H.264 decoder with FFmpeg"
```

---

## Task 12: V4L2 Video Capture

**Files:**
- Create: `src/media/video/v4l2_capture.h`
- Create: `src/media/video/v4l2_capture.cpp`

**Learning Goal:** 理解 Linux 下如何直接操作摄像头设备。V4L2 是 Video for Linux 2 的缩写，是 Linux 内核提供的视频采集 API。面试中展示你理解底层设备操作。

- [ ] **Step 1: Create v4l2_capture.h**

```cpp
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

namespace crystal {

struct V4L2Config {
    std::string device = "/dev/video0";
    int width = 640;
    int height = 480;
    int fps = 30;
};

class V4L2Capture {
public:
    using FrameCallback = std::function<void(const uint8_t* yuvData,
                                             size_t len)>;

    explicit V4L2Capture(const V4L2Config& config);
    ~V4L2Capture();

    bool open();
    void close();
    void startCapture();
    void stopCapture();

    void onFrame(FrameCallback cb);

private:
    void captureLoop();

    V4L2Config config_;
    int fd_ = -1;
    std::atomic<bool> capturing_{false};
    std::thread captureThread_;
    FrameCallback frameCb_;
    std::vector<uint8_t> buffer_;
};

} // namespace crystal
```

- [ ] **Step 2: Create v4l2_capture.cpp**

```cpp
#include "media/video/v4l2_capture.h"
#include "utils/logger.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

namespace crystal {

struct Buffer {
    void* start;
    size_t length;
};

static constexpr int BUFFER_COUNT = 4;

V4L2Capture::V4L2Capture(const V4L2Config& config) : config_(config) {}

V4L2Capture::~V4L2Capture() {
    close();
}

bool V4L2Capture::open() {
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        Logger::error("Failed to open video device {}: {}",
                      config_.device, strerror(errno));
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config_.width;
    fmt.fmt.pix.height = config_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        Logger::warn("YUV420 not supported, trying MJPEG");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            Logger::error("Failed to set video format: {}", strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    struct v4l2_requestbuffers req = {};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        Logger::error("Failed to request buffers: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    Logger::info("V4L2 capture opened: {}x{} on {}",
                 config_.width, config_.height, config_.device);
    return true;
}

void V4L2Capture::close() {
    stopCapture();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void V4L2Capture::startCapture() {
    if (fd_ < 0) return;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMON, &type);

    capturing_ = true;
    captureThread_ = std::thread(&V4L2Capture::captureLoop, this);
    Logger::info("V4L2 capture started");
}

void V4L2Capture::stopCapture() {
    capturing_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    if (fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
}

void V4L2Capture::captureLoop() {
    buffer_.resize(config_.width * config_.height * 3 / 2);

    while (capturing_) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {
                Logger::warn("V4L2 DQBUF error: {}", strerror(errno));
            }
            usleep(1000);
            continue;
        }

        if (frameCb_ && buf.bytesused > 0) {
            size_t expectedSize = config_.width * config_.height * 3 / 2;
            if (buf.bytesused >= static_cast<unsigned int>(expectedSize)) {
                frameCb_(buffer_.data(), expectedSize);
            }
        }

        ioctl(fd_, VIDIOC_QBUF, &buf);
    }
}

void V4L2Capture::onFrame(FrameCallback cb) {
    frameCb_ = std::move(cb);
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_media_video
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add V4L2 video capture"
```

---

## Task 13: SDL2 Video Renderer

**Files:**
- Create: `src/media/video/sdl_renderer.h`
- Create: `src/media/video/sdl_renderer.cpp`

**Learning Goal:** 理解视频渲染流程——YUV 到 RGB 转换、双缓冲、垂直同步。

- [ ] **Step 1: Create sdl_renderer.h**

```cpp
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace crystal {

class SDLRenderer {
public:
    SDLRenderer(int width, int height, const std::string& title = "CrystalRTC");
    ~SDLRenderer();

    bool init();
    void render(const uint8_t* yuvData, int width, int height);
    void pollEvents();
    bool shouldQuit() const;

private:
    int width_;
    int height_;
    std::string title_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    bool quit_ = false;
};

} // namespace crystal
```

- [ ] **Step 2: Create sdl_renderer.cpp**

```cpp
#include "media/video/sdl_renderer.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>

namespace crystal {

SDLRenderer::SDLRenderer(int width, int height, const std::string& title)
    : width_(width), height_(height), title_(title) {}

SDLRenderer::~SDLRenderer() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool SDLRenderer::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        Logger::error("SDL init failed: {}", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(title_.c_str(),
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                width_, height_,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        Logger::error("SDL window creation failed: {}", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        Logger::error("SDL renderer creation failed: {}", SDL_GetError());
        return false;
    }

    texture_ = SDL_CreateTexture(renderer_,
                                  SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  width_, height_);
    if (!texture_) {
        Logger::error("SDL texture creation failed: {}", SDL_GetError());
        return false;
    }

    Logger::info("SDL renderer initialized: {}x{}", width_, height_);
    return true;
}

void SDLRenderer::render(const uint8_t* yuvData, int width, int height) {
    if (!texture_ || !renderer_) return;

    SDL_UpdateYUVTexture(texture_, nullptr,
                          yuvData, width,
                          yuvData + width * height, width / 2,
                          yuvData + width * height * 5 / 4, width / 2);

    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

void SDLRenderer::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            quit_ = true;
        }
    }
}

bool SDLRenderer::shouldQuit() const {
    return quit_;
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_media_video
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add SDL2 video renderer"
```

---

## Task 14: Opus Encoder & Decoder

**Files:**
- Create: `src/media/audio/opus_encoder.h`
- Create: `src/media/audio/opus_encoder.cpp`
- Create: `src/media/audio/opus_decoder.h`
- Create: `src/media/audio/opus_decoder.cpp`

**Learning Goal:** 理解 Opus 编解码器——WebRTC 标准音频编解码器。关键参数：采样率 48kHz、帧大小 20ms(960 samples)、码率 6-510 kbps。

- [ ] **Step 1: Create opus_encoder.h**

```cpp
#pragma once

#include <opus/opus.h>
#include <vector>
#include <cstdint>
#include <functional>

namespace crystal {

struct OpusEncoderConfig {
    int sampleRate = 48000;
    int channels = 1;
    int bitrateKbps = 64;
    int frameSize = 960;
};

class OpusEncoder {
public:
    explicit OpusEncoder(const OpusEncoderConfig& config);
    ~OpusEncoder();

    bool init();
    std::vector<uint8_t> encode(const int16_t* pcmData, int frameSize);
    int frameSize() const { return config_.frameSize; }

private:
    OpusEncoderConfig config_;
    ::OpusEncoder* encoder_ = nullptr;
};

} // namespace crystal
```

- [ ] **Step 2: Create opus_encoder.cpp**

```cpp
#include "media/audio/opus_encoder.h"
#include "utils/logger.h"

namespace crystal {

OpusEncoder::OpusEncoder(const OpusEncoderConfig& config)
    : config_(config) {}

OpusEncoder::~OpusEncoder() {
    if (encoder_) opus_encoder_destroy(encoder_);
}

bool OpusEncoder::init() {
    int error;
    encoder_ = opus_encoder_create(config_.sampleRate, config_.channels,
                                    OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !encoder_) {
        Logger::error("Failed to create Opus encoder: {}", opus_strerror(error));
        return false;
    }

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrateKbps * 1000));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));

    Logger::info("Opus encoder initialized: {}Hz {}ch @ {}kbps",
                 config_.sampleRate, config_.channels, config_.bitrateKbps);
    return true;
}

std::vector<uint8_t> OpusEncoder::encode(const int16_t* pcmData, int frameSize) {
    if (!encoder_) return {};

    std::vector<uint8_t> output(4000);
    int len = opus_encode(encoder_, pcmData, frameSize,
                          output.data(), static_cast<opus_int32>(output.size()));
    if (len < 0) {
        Logger::warn("Opus encode failed: {}", opus_strerror(len));
        return {};
    }

    output.resize(len);
    return output;
}

} // namespace crystal
```

- [ ] **Step 3: Create opus_decoder.h**

```cpp
#pragma once

#include <opus/opus.h>
#include <vector>
#include <cstdint>

namespace crystal {

class OpusDecoder {
public:
    OpusDecoder(int sampleRate = 48000, int channels = 1);
    ~OpusDecoder();

    bool init();
    std::vector<int16_t> decode(const uint8_t* opusData, size_t len,
                                 int frameSize = 960);

private:
    int sampleRate_;
    int channels_;
    ::OpusDecoder* decoder_ = nullptr;
};

} // namespace crystal
```

- [ ] **Step 4: Create opus_decoder.cpp**

```cpp
#include "media/audio/opus_decoder.h"
#include "utils/logger.h"

namespace crystal {

OpusDecoder::OpusDecoder(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels) {}

OpusDecoder::~OpusDecoder() {
    if (decoder_) opus_decoder_destroy(decoder_);
}

bool OpusDecoder::init() {
    int error;
    decoder_ = opus_decoder_create(sampleRate_, channels_, &error);
    if (error != OPUS_OK || !decoder_) {
        Logger::error("Failed to create Opus decoder: {}", opus_strerror(error));
        return false;
    }

    Logger::info("Opus decoder initialized: {}Hz {}ch", sampleRate_, channels_);
    return true;
}

std::vector<int16_t> OpusDecoder::decode(const uint8_t* opusData, size_t len,
                                           int frameSize) {
    if (!decoder_) return {};

    std::vector<int16_t> output(frameSize * channels_);
    int samples = opus_decode(decoder_, opusData, static_cast<opus_int32>(len),
                               output.data(), frameSize, 0);
    if (samples < 0) {
        Logger::warn("Opus decode failed: {}", opus_strerror(samples));
        return {};
    }

    output.resize(samples * channels_);
    return output;
}

} // namespace crystal
```

- [ ] **Step 5: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_media_audio
```

Expected: Builds successfully.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add Opus encoder and decoder"
```

---

## Task 15: ALSA Audio Capture & SDL2 Audio Player

**Files:**
- Create: `src/media/audio/alsa_capture.h`
- Create: `src/media/audio/alsa_capture.cpp`
- Create: `src/media/audio/sdl_audio_player.h`
- Create: `src/media/audio/sdl_audio_player.cpp`

**Learning Goal:** 理解 Linux 音频采集和播放流程。ALSA 是 Linux 标准音频 API，SDL2 提供跨平台音频播放。

- [ ] **Step 1: Create alsa_capture.h**

```cpp
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

struct _snd_pcm;

namespace crystal {

struct AlsaConfig {
    std::string device = "default";
    int sampleRate = 48000;
    int channels = 1;
    int frameSize = 960;
};

class AlsaCapture {
public:
    using AudioCallback = std::function<void(const int16_t* data, size_t samples)>;

    explicit AlsaCapture(const AlsaConfig& config);
    ~AlsaCapture();

    bool open();
    void close();
    void startCapture();
    void stopCapture();

    void onAudio(AudioCallback cb);

private:
    void captureLoop();

    AlsaConfig config_;
    _snd_pcm* pcm_ = nullptr;
    std::atomic<bool> capturing_{false};
    std::thread captureThread_;
    AudioCallback audioCb_;
};

} // namespace crystal
```

- [ ] **Step 2: Create alsa_capture.cpp**

```cpp
#include "media/audio/alsa_capture.h"
#include "utils/logger.h"
#include <alsa/asoundlib.h>

namespace crystal {

AlsaCapture::AlsaCapture(const AlsaConfig& config) : config_(config) {}

AlsaCapture::~AlsaCapture() {
    close();
}

bool AlsaCapture::open() {
    int err = snd_pcm_open(&pcm_, config_.device.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        Logger::error("ALSA open failed: {}", snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_, params);

    snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, params, config_.channels);

    unsigned int rate = config_.sampleRate;
    snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, nullptr);

    snd_pcm_uframes_t frames = config_.frameSize;
    snd_pcm_hw_params_set_period_size_near(pcm_, params, &frames, nullptr);

    err = snd_pcm_hw_params(pcm_, params);
    if (err < 0) {
        Logger::error("ALSA hw_params failed: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    Logger::info("ALSA capture opened: {}Hz {}ch on {}",
                 config_.sampleRate, config_.channels, config_.device);
    return true;
}

void AlsaCapture::close() {
    stopCapture();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

void AlsaCapture::startCapture() {
    if (!pcm_) return;
    capturing_ = true;
    captureThread_ = std::thread(&AlsaCapture::captureLoop, this);
    Logger::info("ALSA capture started");
}

void AlsaCapture::stopCapture() {
    capturing_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
}

void AlsaCapture::captureLoop() {
    std::vector<int16_t> buffer(config_.frameSize * config_.channels);

    while (capturing_) {
        int frames = snd_pcm_readi(pcm_, buffer.data(), config_.frameSize);
        if (frames < 0) {
            frames = snd_pcm_recover(pcm_, frames, 0);
            if (frames < 0) {
                Logger::warn("ALSA read failed: {}", snd_strerror(frames));
                break;
            }
            continue;
        }

        if (audioCb_) {
            audioCb_(buffer.data(), static_cast<size_t>(frames));
        }
    }
}

void AlsaCapture::onAudio(AudioCallback cb) {
    audioCb_ = std::move(cb);
}

} // namespace crystal
```

- [ ] **Step 3: Create sdl_audio_player.h**

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <queue>

namespace crystal {

class SDLAudioPlayer {
public:
    SDLAudioPlayer(int sampleRate = 48000, int channels = 1);
    ~SDLAudioPlayer();

    bool init();
    void play(const int16_t* data, size_t samples);
    void stop();

private:
    static void audioCallback(void* userdata, uint8_t* stream, int len);
    void fillBuffer(uint8_t* stream, int len);

    int sampleRate_;
    int channels_;
    std::mutex mutex_;
    std::queue<int16_t> buffer_;
};

} // namespace crystal
```

- [ ] **Step 4: Create sdl_audio_player.cpp**

```cpp
#include "media/audio/sdl_audio_player.h"
#include "utils/logger.h"
#include <SDL2/SDL.h>
#include <cstring>

namespace crystal {

SDLAudioPlayer::SDLAudioPlayer(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels) {}

SDLAudioPlayer::~SDLAudioPlayer() {
    stop();
}

bool SDLAudioPlayer::init() {
    SDL_AudioSpec spec;
    spec.freq = sampleRate_;
    spec.format = AUDIO_S16LSB;
    spec.channels = channels_;
    spec.samples = 960;
    spec.callback = audioCallback;
    spec.userdata = this;

    if (SDL_OpenAudio(&spec, nullptr) < 0) {
        Logger::error("SDL audio open failed: {}", SDL_GetError());
        return false;
    }

    SDL_PauseAudio(0);
    Logger::info("SDL audio player initialized: {}Hz {}ch", sampleRate_, channels_);
    return true;
}

void SDLAudioPlayer::play(const int16_t* data, size_t samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < samples * channels_; i++) {
        buffer_.push(data[i]);
    }
}

void SDLAudioPlayer::stop() {
    SDL_CloseAudio();
}

void SDLAudioPlayer::audioCallback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<SDLAudioPlayer*>(userdata);
    self->fillBuffer(stream, len);
}

void SDLAudioPlayer::fillBuffer(uint8_t* stream, int len) {
    std::lock_guard<std::mutex> lock(mutex_);
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    int samples = len / 2;

    for (int i = 0; i < samples; i++) {
        if (!buffer_.empty()) {
            out[i] = buffer_.front();
            buffer_.pop();
        } else {
            out[i] = 0;
        }
    }
}

} // namespace crystal
```

- [ ] **Step 5: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_media_audio
```

Expected: Builds successfully.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add ALSA audio capture and SDL2 audio player"
```

---

## Task 16: Room Manager

**Files:**
- Create: `src/room/room_manager.h`
- Create: `src/room/room_manager.cpp`

**Learning Goal:** 理解房间管理模型——WebRTC 会议系统中的核心概念：房间、Peer、发布/订阅关系。

- [ ] **Step 1: Create room_manager.h**

```cpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace crystal {

struct PeerInfo {
    std::string id;
    std::string room;
    bool hasVideo = false;
    bool hasAudio = false;
};

class RoomManager {
public:
    std::string joinRoom(const std::string& room, const std::string& peerId);
    void leaveRoom(const std::string& peerId);
    std::vector<PeerInfo> getPeersInRoom(const std::string& room);
    PeerInfo getPeerInfo(const std::string& peerId);

    using PeerEventCallback = std::function<void(const std::string& room,
                                                  const std::string& peerId,
                                                  const std::string& event)>;
    void onPeerEvent(PeerEventCallback cb);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, PeerInfo> peers_;
    std::unordered_map<std::string, std::vector<std::string>> rooms_;
    PeerEventCallback peerEventCb_;
};

} // namespace crystal
```

- [ ] **Step 2: Create room_manager.cpp**

```cpp
#include "room/room_manager.h"
#include "utils/logger.h"

namespace crystal {

std::string RoomManager::joinRoom(const std::string& room,
                                   const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    PeerInfo info;
    info.id = peerId;
    info.room = room;
    peers_[peerId] = info;
    rooms_[room].push_back(peerId);

    Logger::info("Peer {} joined room {}", peerId, room);

    if (peerEventCb_) {
        peerEventCb_(room, peerId, "joined");
    }

    return room;
}

void RoomManager::leaveRoom(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peerId);
    if (it == peers_.end()) return;

    std::string room = it->second.room;
    peers_.erase(it);

    auto& peerList = rooms_[room];
    peerList.erase(std::remove(peerList.begin(), peerList.end(), peerId),
                   peerList.end());

    if (peerList.empty()) {
        rooms_.erase(room);
    }

    Logger::info("Peer {} left room {}", peerId, room);

    if (peerEventCb_) {
        peerEventCb_(room, peerId, "left");
    }
}

std::vector<PeerInfo> RoomManager::getPeersInRoom(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PeerInfo> result;
    auto it = rooms_.find(room);
    if (it != rooms_.end()) {
        for (const auto& peerId : it->second) {
            auto pit = peers_.find(peerId);
            if (pit != peers_.end()) {
                result.push_back(pit->second);
            }
        }
    }
    return result;
}

PeerInfo RoomManager::getPeerInfo(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end()) return it->second;
    return {};
}

void RoomManager::onPeerEvent(PeerEventCallback cb) {
    peerEventCb_ = std::move(cb);
}

} // namespace crystal
```

- [ ] **Step 3: Verify compilation**

```bash
cd /workspace/build && cmake --build . --target crystal_room
```

Expected: Builds successfully.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add room manager with peer tracking"
```

---

## Task 17: Main Client Application

**Files:**
- Modify: `main_client.cpp`

**Learning Goal:** 将所有模块串联起来，理解完整的 P2P 通话流程：信令交换 → ICE 连接 → 媒体传输。

- [ ] **Step 1: Update main_client.cpp**

```cpp
#include "utils/logger.h"
#include "signaling/signaling_client.h"
#include "transport/transport_manager.h"
#include "media/video/v4l2_capture.h"
#include "media/video/h264_encoder.h"
#include "media/video/h264_decoder.h"
#include "media/video/sdl_renderer.h"
#include "media/audio/alsa_capture.h"
#include "media/audio/opus_encoder.h"
#include "media/audio/opus_decoder.h"
#include "media/audio/sdl_audio_player.h"
#include "media/rtp/rtp_packetizer.h"
#include "media/rtp/rtp_depacketizer.h"
#include "media/rtp/jitter_buffer.h"
#include "room/room_manager.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    crystal::Logger::init("client");

    std::string signalingHost = "127.0.0.1";
    uint16_t signalingPort = 8765;
    std::string room = "default";

    if (argc >= 2) room = argv[1];
    if (argc >= 3) signalingHost = argv[2];
    if (argc >= 4) signalingPort = static_cast<uint16_t>(std::stoi(argv[3]));

    crystal::Logger::info("CrystalRTC Client starting...");
    crystal::Logger::info("Room: {}, Server: {}:{}", room, signalingHost, signalingPort);

    crystal::IceConfig iceConfig = crystal::IceConfig::defaultConfig();
    crystal::TransportManager transportMgr(iceConfig);

    auto pc = transportMgr.createPeerConnection();

    crystal::RtpPacketizer videoPacketizer(96, 90000, 0x12345678);
    crystal::RtpPacketizer audioPacketizer(97, 48000, 0x87654321);
    crystal::RtpDepacketizer depacketizer;
    crystal::JitterBuffer videoJitterBuf(40);
    crystal::JitterBuffer audioJitterBuf(40);

    crystal::H264EncoderConfig encConfig;
    crystal::H264Encoder encoder(encConfig);
    crystal::H264Decoder decoder;

    crystal::V4L2Config v4l2Config;
    crystal::V4L2Capture capture(v4l2Config);

    crystal::SDLRenderer renderer(encConfig.width, encConfig.height, "CrystalRTC - Local");

    crystal::OpusEncoderConfig opusEncConfig;
    crystal::OpusEncoder opusEncoder(opusEncConfig);
    crystal::OpusDecoder opusDecoder;

    crystal::AlsaConfig alsaConfig;
    crystal::AlsaCapture alsaCapture(alsaConfig);
    crystal::SDLAudioPlayer audioPlayer;

    crystal::SignalingClient signalingClient;

    pc->onTrack([&](const std::vector<uint8_t>& data) {
        crystal::RtpPacket pkt;
        if (!pkt.parse(data.data(), data.size())) return;

        if (pkt.payloadType() == 96) {
            videoJitterBuf.insert(pkt);
            auto packets = videoJitterBuf.consume();
            for (const auto& p : packets) {
                auto nals = depacketizer.depacketizeH264(p);
                for (const auto& nal : nals) {
                    decoder.decode(nal.data(), nal.size());
                }
            }
        } else if (pkt.payloadType() == 97) {
            audioJitterBuf.insert(pkt);
            auto packets = audioJitterBuf.consume();
            for (const auto& p : packets) {
                auto frames = depacketizer.depacketizeOpus(p);
                for (const auto& frame : frames) {
                    auto pcm = opusDecoder.decode(frame.data(), frame.size());
                    if (!pcm.empty()) {
                        audioPlayer.play(pcm.data(), pcm.size());
                    }
                }
            }
        }
    });

    pc->onIceCandidate([&](const std::string& candidate,
                            const std::string& sdpMid, int sdpMLineIndex) {
        signalingClient.sendCandidate(candidate, sdpMid, sdpMLineIndex, "");
    });

    decoder.onDecoded([&](const uint8_t* yuvData, int width, int height) {
        renderer.render(yuvData, width, height);
    });

    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        std::vector<uint8_t> nal(nalData, nalData + nalLen);
        auto packets = videoPacketizer.packetizeH264(nal, 0);
        for (const auto& pkt : packets) {
            auto data = pkt.serialize();
            pc->sendMedia(data);
        }
    });

    capture.onFrame([&](const uint8_t* yuvData, size_t len) {
        encoder.encode(yuvData, len);
    });

    alsaCapture.onAudio([&](const int16_t* data, size_t samples) {
        auto opusFrame = opusEncoder.encode(data, opusEncoder.frameSize());
        if (!opusFrame.empty()) {
            auto packets = audioPacketizer.packetizeOpus(opusFrame, 0);
            for (const auto& pkt : packets) {
                auto data = pkt.serialize();
                pc->sendMedia(data);
            }
        }
    });

    signalingClient.onMessage([&](const crystal::SignalingMessage& msg) {
        if (msg.type == "peer_joined") {
            crystal::Logger::info("Peer joined: {}, sending offer", msg.peerId);
            auto sdp = pc->createOffer();
            signalingClient.sendOffer(sdp, msg.peerId);
        } else if (msg.type == "offer") {
            crystal::Logger::info("Received offer from {}", msg.peerId);
            pc->setRemoteDescription(msg.sdp, "offer");
            auto answer = pc->createAnswer();
            signalingClient.sendAnswer(answer, msg.peerId);
        } else if (msg.type == "answer") {
            crystal::Logger::info("Received answer from {}", msg.peerId);
            pc->setRemoteDescription(msg.sdp, "answer");
        } else if (msg.type == "candidate") {
            crystal::Logger::debug("Received ICE candidate from {}", msg.peerId);
            pc->addIceCandidate(msg.candidate, msg.sdpMid, msg.sdpMLineIndex);
        }
    });

    if (!encoder.init()) {
        crystal::Logger::error("Failed to init H264 encoder");
        return 1;
    }
    if (!decoder.init()) {
        crystal::Logger::error("Failed to init H264 decoder");
        return 1;
    }
    if (!renderer.init()) {
        crystal::Logger::error("Failed to init SDL renderer");
        return 1;
    }
    if (!opusEncoder.init()) {
        crystal::Logger::error("Failed to init Opus encoder");
        return 1;
    }
    if (!opusDecoder.init()) {
        crystal::Logger::error("Failed to init Opus decoder");
        return 1;
    }
    if (!audioPlayer.init()) {
        crystal::Logger::error("Failed to init audio player");
        return 1;
    }

    if (!signalingClient.connect(signalingHost, signalingPort)) {
        crystal::Logger::error("Failed to connect to signaling server");
        return 1;
    }

    signalingClient.joinRoom(room);

    if (!capture.open()) {
        crystal::Logger::warn("Video capture not available, continuing without video");
    } else {
        capture.startCapture();
    }

    if (!alsaCapture.open()) {
        crystal::Logger::warn("Audio capture not available, continuing without audio");
    } else {
        alsaCapture.startCapture();
    }

    crystal::Logger::info("CrystalRTC client running. Press Ctrl+C to quit.");

    while (g_running && !renderer.shouldQuit()) {
        renderer.pollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    crystal::Logger::info("Shutting down...");
    capture.stopCapture();
    alsaCapture.stopCapture();
    signalingClient.leaveRoom();
    signalingClient.disconnect();

    return 0;
}
```

- [ ] **Step 2: Verify full build**

```bash
cd /workspace/build && cmake --build .
```

Expected: Full project builds successfully.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "feat: wire up main client with full P2P pipeline"
```

---

## Task 18: Run All Tests & Final Verification

**Files:** None new

**Learning Goal:** 确保所有单元测试通过，验证 RTP 打包/解包/抖动缓冲的正确性。

- [ ] **Step 1: Build and run all tests**

```bash
cd /workspace/build && cmake --build . --target crystal_rtp_tests
./tests/crystal_rtp_tests
```

Expected: All tests pass.

- [ ] **Step 2: Verify full project build**

```bash
cd /workspace/build && cmake --build .
```

Expected: All targets build without errors.

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "chore: verify all tests pass and project builds cleanly"
```
