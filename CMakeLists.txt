cmake_minimum_required(VERSION 3.16)
project(CrystalRTC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

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

find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(ALSA REQUIRED alsa)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

add_executable(crystal_demo main_demo.cpp)
target_link_libraries(crystal_demo PRIVATE
    -Wl,--whole-archive
    crystal_media_video crystal_media_rtp crystal_utils
    -Wl,--no-whole-archive
    spdlog::spdlog
    avcodec swresample swscale avutil
)

add_executable(crystal_signaling_server main_signaling_server.cpp)
target_link_libraries(crystal_signaling_server PRIVATE crystal_signaling crystal_utils websockets)

add_executable(crystal_client main_client.cpp)
set_target_properties(crystal_client PROPERTIES LINK_DEPENDS_NO_SHARED ON)
target_link_libraries(crystal_client PRIVATE
    -Wl,--whole-archive
    crystal_signaling crystal_transport crystal_media_video crystal_media_audio
    crystal_media_rtp crystal_room crystal_utils
    -Wl,--no-whole-archive
    datachannel websockets spdlog::spdlog nlohmann_json::nlohmann_json
    SDL2::SDL2
    avcodec swresample swscale avutil
    opus asound
)
