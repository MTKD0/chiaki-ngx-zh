cmake_minimum_required(VERSION 3.16)
project(chiaki_enhancement_videolab LANGUAGES CXX)
# Standalone on purpose: a compile error here must never break the GUI, worker or CI builds.
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLACEBO REQUIRED IMPORTED_TARGET libplacebo>=6.338)

add_executable(enhancement-video-lab main.cpp realtime.cpp pipeline_backend.cpp
    backend.h metrics.h raw_video.h vulkan_device.h ../pipeline.cpp ../pipeline.h)
target_compile_features(enhancement-video-lab PRIVATE cxx_std_17)
target_link_libraries(enhancement-video-lab PRIVATE Qt6::Core Qt6::Gui Qt6::Network PkgConfig::PLACEBO)

add_executable(videolab-metrics-test metrics_test.cpp metrics.h)
target_compile_features(videolab-metrics-test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME videolab-metrics COMMAND videolab-metrics-test)

# End to end without a GPU: a synthetic 60 fps clip, halved to 30 fps, scored against the
# CPU cross-fade generator. Needs an ffmpeg with libx264 on PATH; skipped otherwise.
find_program(VIDEOLAB_FFMPEG ffmpeg)
find_program(VIDEOLAB_FFPROBE ffprobe)
if(VIDEOLAB_FFMPEG AND VIDEOLAB_FFPROBE)
    add_test(NAME videolab-dry-run COMMAND ${CMAKE_COMMAND}
        -DLAB=$<TARGET_FILE:enhancement-video-lab> -DFFMPEG=${VIDEOLAB_FFMPEG} -DFFPROBE=${VIDEOLAB_FFPROBE}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/dry-run -P ${CMAKE_CURRENT_SOURCE_DIR}/dry_run_test.cmake)
    set_tests_properties(videolab-dry-run PROPERTIES TIMEOUT 180)
endif()
