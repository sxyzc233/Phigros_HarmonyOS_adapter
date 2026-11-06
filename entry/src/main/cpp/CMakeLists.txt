# Native probe module for the Android-on-HarmonyOS translation experiment.
# It only loads libraries, emulates a JNI surface and reports; it contains no game code.
cmake_minimum_required(VERSION 3.5.0)
project(transprobe)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
)

add_library(ReaSLES SHARED android_sles_audio.cpp)
target_link_libraries(ReaSLES PUBLIC libohaudio.so libhilog_ndk.z.so)
add_library(transprobe SHARED probe.cpp jni_emu.cpp asset_zip.cpp surface.cpp pthread_abi.cpp il2cpp_data.cpp il2cpp_lookup_hook.cpp jni_args_selftest.cpp egl_diagnostics.cpp native_touch.cpp fmod_audio_host.cpp android_network.cpp android_sysconf.cpp engine_state.cpp tap_sdk_bridge.cpp java_preferences.cpp managed_exception_trace.cpp player_platform.cpp video_surface.cpp)

target_link_libraries(transprobe PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libdl.so
    libz.so
    libace_ndk.z.so
    libEGL.so
    libnative_window.so
    libohaudio.so
    libnative_image.so
)

# Real video bridge replaces the early no-media probe stub. Keep originals archived.
add_library(mediandk SHARED android_media.cpp)
target_link_libraries(mediandk PUBLIC libnative_media_avsource.so libnative_media_avdemuxer.so libnative_media_codecbase.so libnative_media_core.so libnative_media_vdec.so libnative_media_acodec.so libhilog_ndk.z.so)
add_library(android SHARED android_framework_shim.c)
target_link_libraries(android PUBLIC libhilog_ndk.z.so libdl.so)

# Build the compatibility log library from source; no prebuilt Android binaries.
add_library(log SHARED log_shim.c)
target_link_libraries(log PUBLIC libhilog_ndk.z.so)
