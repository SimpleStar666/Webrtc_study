add_executable(crystal_rtp_tests
    rtp_packet_test.cpp
    rtp_packetizer_test.cpp
    rtp_depacketizer_test.cpp
    jitter_buffer_test.cpp
    jitter_time_test.cpp
    twcc_header_test.cpp
)
target_link_libraries(crystal_rtp_tests PRIVATE
    crystal_media_rtp crystal_utils GTest::gtest_main
)
add_test(NAME crystal_rtp_tests COMMAND crystal_rtp_tests)

# 无锁环形缓冲测试（工程化升级 v2 Phase D 新增）
add_executable(crystal_thread_tests
    spsc_ring_test.cpp
)
target_link_libraries(crystal_thread_tests PRIVATE
    crystal_utils GTest::gtest_main
)
add_test(NAME crystal_thread_tests COMMAND crystal_thread_tests)

add_executable(crystal_rtcp_tests
    rtcp_packet_test.cpp
    retransmission_buffer_test.cpp
    nack_requester_test.cpp
    rtcp_reporter_test.cpp
    jitter_buffer_gap_test.cpp
    transport_feedback_test.cpp
)
target_link_libraries(crystal_rtcp_tests PRIVATE
    crystal_media_rtcp crystal_media_rtp crystal_utils GTest::gtest_main
)
add_test(NAME crystal_rtcp_tests COMMAND crystal_rtcp_tests)

add_executable(crystal_metrics_tests
    metrics_collector_test.cpp
)
target_link_libraries(crystal_metrics_tests PRIVATE
    crystal_media_monitor crystal_utils GTest::gtest_main
)
add_test(NAME crystal_metrics_tests COMMAND crystal_metrics_tests)

# Opus FEC/DTX 弱网对抗测试（工程化升级 v2 新增）
add_executable(crystal_opus_tests
    opus_fec_test.cpp
)
target_link_libraries(crystal_opus_tests PRIVATE
    crystal_media_audio crystal_utils GTest::gtest_main
)
add_test(NAME crystal_opus_tests COMMAND crystal_opus_tests)

# GCC 带宽估计测试（工程化升级 v2 新增）
add_executable(crystal_gcc_tests
    trendline_test.cpp
    gcc_controller_test.cpp
)
target_link_libraries(crystal_gcc_tests PRIVATE
    crystal_media_gcc crystal_utils GTest::gtest_main
)
add_test(NAME crystal_gcc_tests COMMAND crystal_gcc_tests)
