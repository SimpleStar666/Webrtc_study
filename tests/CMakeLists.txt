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
