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
target_include_directories(crystal_media_video PRIVATE ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
target_link_directories(crystal_media_video PRIVATE ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS} ${SWSCALE_LIBRARY_DIRS})
target_link_libraries(crystal_media_video PUBLIC
    crystal_utils crystal_media_rtp
    avcodec avutil swscale
    SDL2::SDL2
)

# --- Media: Audio ---
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(OPUS REQUIRED opus)

add_library(crystal_media_audio STATIC
    media/audio/alsa_capture.cpp
    media/audio/opus_encoder.cpp
    media/audio/opus_decoder.cpp
    media/audio/sdl_audio_player.cpp
)
target_include_directories(crystal_media_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(crystal_media_audio PRIVATE ${OPUS_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
target_link_directories(crystal_media_audio PRIVATE ${OPUS_LIBRARY_DIRS} ${ALSA_LIBRARY_DIRS})
target_link_libraries(crystal_media_audio PUBLIC
    crystal_utils crystal_media_rtp
    opus asound SDL2::SDL2
)

# --- Room library ---
add_library(crystal_room STATIC
    room/room_manager.cpp
)
target_include_directories(crystal_room PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_room PUBLIC crystal_utils nlohmann_json::nlohmann_json)
