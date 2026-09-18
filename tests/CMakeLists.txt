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

add_executable(crystal_rtcp_tests
    rtcp_packet_test.cpp
    retransmission_buffer_test.cpp
    nack_requester_test.cpp
    rtcp_reporter_test.cpp
    jitter_buffer_gap_test.cpp
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
