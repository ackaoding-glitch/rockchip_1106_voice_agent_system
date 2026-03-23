cmake_minimum_required(VERSION 3.10)

project(zh_client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(zh_client
    src/main.c
    src/app.c
    src/audio_uplink.c
    src/audio_capture_alsa.c
    src/audio_capture_rockit.c
    src/audio_device.c
    src/bithion_core_compat.c
    src/ble_provision.c
    src/wifi_bootstrap.c
    src/music_player.c
    src/prompt_tone.c
    src/udp_tts.c
    src/face_recognition.c
    src/config.c
    src/utils.c
    src/audio_playback_rockit.c
    src/audio_playback_alsa.c
)

add_executable(zh_ble_gatt_server
    ble_sdk/btgatt-server.c
    ble_sdk/uuid-helper.c
)

add_executable(zh_face_test
    tests/face_test.c
    src/utils.c
    src/config.c
)

set(BITHION_CORE_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/core-sdk CACHE PATH "bithion-core SDK root")
set(BITHION_CORE_SDK_LIB "" CACHE PATH "bithion-core SDK lib dir")
if (NOT BITHION_CORE_SDK_LIB)
  set(BITHION_CORE_SDK_LIB ${BITHION_CORE_SDK_ROOT}/lib)
  set(BITHION_CORE_SDK_LIB ${BITHION_CORE_SDK_LIB} CACHE PATH "bithion-core SDK lib dir" FORCE)
endif()
set(BITHION_CORE_INCLUDE_DIR ${BITHION_CORE_SDK_ROOT}/include)
find_library(BITHION_CORE_LIBRARY
  NAMES bithion-core libbithion-core
  PATHS ${BITHION_CORE_SDK_LIB}
  NO_DEFAULT_PATH
  REQUIRED
)
add_library(bithion-core::bithion-core SHARED IMPORTED)
set_target_properties(bithion-core::bithion-core PROPERTIES
  IMPORTED_LOCATION ${BITHION_CORE_LIBRARY}
  INTERFACE_INCLUDE_DIRECTORIES ${BITHION_CORE_INCLUDE_DIR}
)

# opus 预编译库路径
set(OPUS_PREBUILT_DIR ${CMAKE_SOURCE_DIR}/third_party/opus_prebuilt)
# alsa-rk
set(ALSA_PREBUILT_DIR ${CMAKE_SOURCE_DIR}/third_party/alsa_prebuilt_rk)
# rkmedia/rockit SDK
set(RK_SDK_ROOT /mnt/sdk/origin_sdk/rv110x_ipc-release-20250513 CACHE PATH "Rockchip SDK root")
if (EXISTS "${RK_SDK_ROOT}/media/out/lib")
  set(RK_MEDIA_ROOT ${RK_SDK_ROOT}/media/out)
elseif (EXISTS "${RK_SDK_ROOT}/output/out/media_out/lib")
  set(RK_MEDIA_ROOT ${RK_SDK_ROOT}/output/out/media_out)
else()
  set(RK_MEDIA_ROOT ${RK_SDK_ROOT}/media/out)
endif()
set(RK_MEDIA_INCLUDE ${RK_MEDIA_ROOT}/include)
set(RK_ROCKIT_INCLUDE ${RK_SDK_ROOT}/media/rockit/out/include)
set(RK_ROCKIT_MPI_INCLUDE ${RK_SDK_ROOT}/media/rockit/rockit/mpi/sdk/include)
set(RK_MEDIA_LIB ${RK_MEDIA_ROOT}/lib)
set(RK_OEM_LIB ${RK_MEDIA_ROOT}/oem/usr/lib)
set(RK_SDK_INCLUDE_DIRS ${RK_MEDIA_INCLUDE})
if (EXISTS "${RK_ROCKIT_INCLUDE}")
  list(APPEND RK_SDK_INCLUDE_DIRS ${RK_ROCKIT_INCLUDE})
endif()
if (EXISTS "${RK_ROCKIT_MPI_INCLUDE}")
  list(APPEND RK_SDK_INCLUDE_DIRS ${RK_ROCKIT_MPI_INCLUDE})
endif()

if (EXISTS ${RK_MEDIA_LIB}/librga.so)
  set(RK_LIBRGA ${RK_MEDIA_LIB}/librga.so)
elseif (EXISTS ${RK_SDK_ROOT}/media/rga/out/lib/librga.so)
  set(RK_LIBRGA ${RK_SDK_ROOT}/media/rga/out/lib/librga.so)
else()
  set(RK_LIBRGA ${RK_MEDIA_LIB}/librga.so)
endif()

# 读取 config.h 中的后端选择，避免通过 cmake option 控制
file(STRINGS ${CMAKE_SOURCE_DIR}/include/config.h ZH_AUDIO_BACKEND_LINE
     REGEX "^#define[ \t]+ZH_AUDIO_BACKEND_ROCKIT[ \t]+[01]")
string(REGEX REPLACE "^#define[ \t]+ZH_AUDIO_BACKEND_ROCKIT[ \t]+([01]).*$" "\\1"
       ZH_AUDIO_BACKEND_ROCKIT_VALUE "${ZH_AUDIO_BACKEND_LINE}")
if (NOT ZH_AUDIO_BACKEND_ROCKIT_VALUE)
  set(ZH_AUDIO_BACKEND_ROCKIT_VALUE "1")
endif()

if (ZH_AUDIO_BACKEND_ROCKIT_VALUE STREQUAL "1")
  target_link_directories(zh_client PRIVATE ${RK_MEDIA_LIB} ${RK_OEM_LIB})
  target_link_libraries(zh_client PRIVATE
    ${RK_MEDIA_LIB}/librockit_full.so
    ${RK_MEDIA_LIB}/librockchip_mpp.so
    ${RK_MEDIA_LIB}/libdrm.so
    ${RK_LIBRGA}
    ${RK_MEDIA_LIB}/libaec_bf_process.so
    ${RK_MEDIA_LIB}/librkaudio_common.so
  )
  target_link_options(zh_client PRIVATE -Wl,-rpath,/oem/usr/lib)
endif()

target_link_options(zh_client PRIVATE
  "-Wl,-rpath-link,${BITHION_CORE_SDK_LIB}"
)

target_link_libraries(zh_client PRIVATE
  bithion-core::bithion-core
  m
  pthread
  dl
  ${OPUS_PREBUILT_DIR}/lib/libopus.a
  ${ALSA_PREBUILT_DIR}/lib/libasound.so
)

set(RK_BLE_TEST_ROOT ${RK_SDK_ROOT}/sysdrv/packages/ble_test)
target_link_libraries(zh_ble_gatt_server PRIVATE
  ${RK_BLE_TEST_ROOT}/libs/libbluetooth-internal.a
  ${RK_BLE_TEST_ROOT}/libs/libshared-mainloop.a
  pthread
  rt
  dl
  m
)

target_link_options(zh_face_test PRIVATE
  "-Wl,-rpath-link,${BITHION_CORE_SDK_LIB}"
)

target_link_libraries(zh_face_test PRIVATE
  bithion-core::bithion-core
  m
  pthread
)

target_include_directories(zh_client PRIVATE
    ${CMAKE_SOURCE_DIR}
    include
    ${BITHION_CORE_INCLUDE_DIR}
    third_party/minimp3
    ${OPUS_PREBUILT_DIR}/include
    ${ALSA_PREBUILT_DIR}/include
    ${RK_SDK_INCLUDE_DIRS}
)

target_include_directories(zh_ble_gatt_server PRIVATE
    include
    ${BITHION_CORE_INCLUDE_DIR}
    ${RK_BLE_TEST_ROOT}/src/include
    ${RK_BLE_TEST_ROOT}/src/include/lib
)

target_include_directories(zh_face_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    include
    ${BITHION_CORE_INCLUDE_DIR}
    ${OPUS_PREBUILT_DIR}/include
    ${ALSA_PREBUILT_DIR}/include
)
