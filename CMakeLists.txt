cmake_minimum_required(VERSION 3.22)
project(veyamux-linux VERSION 1.7 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)

# ---- Shaders: Veyra's original HLSL (shaders/veyra) + Linux-only passes (shaders), compiled to SPIR-V with DXC ----
set(VEYAMUX_DXC_ROOT "" CACHE PATH "Directory with the DXC Linux release (bin/dxc, lib/libdxcompiler.so)")
find_program(VEYAMUX_DXC dxc HINTS "${VEYAMUX_DXC_ROOT}/bin" ENV DXC_ROOT)
if(NOT VEYAMUX_DXC)
  message(FATAL_ERROR "dxc not found. Run scripts/fetch-dxc.sh and pass -DVEYAMUX_DXC_ROOT=<dir>")
endif()
get_filename_component(_dxc_bin "${VEYAMUX_DXC}" DIRECTORY)
get_filename_component(_dxc_root "${_dxc_bin}" DIRECTORY)
set(ORIG_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/veyra")
file(GLOB ORIG_INCLUDES "${ORIG_SHADERS}/*.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli")

set(SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
set(SPV_OUTPUTS "")
function(veyamux_spv out_name src_file)
  set(out "${SPV_DIR}/${out_name}.spv")
  add_custom_command(OUTPUT "${out}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SPV_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${_dxc_root}/lib"
            "${VEYAMUX_DXC}" -spirv -fspv-target-env=vulkan1.1 -T cs_6_2 -E main -O3
            -fvk-use-dx-layout -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0 -fvk-s-shift 48 0
            -I "${ORIG_SHADERS}" ${ARGN} "${src_file}" -Fo "${out}"
    DEPENDS "${src_file}" ${ORIG_INCLUDES}
    COMMENT "dxc ${out_name}.spv" VERBATIM)
  list(APPEND SPV_OUTPUTS "${out}")
  set(SPV_OUTPUTS "${SPV_OUTPUTS}" PARENT_SCOPE)
endfunction()

foreach(s RgbToLinear AaLuma ScaleBlit)
  veyamux_spv(${s} "${ORIG_SHADERS}/${s}.hlsl")
endforeach()
foreach(q 3 4 5)
  veyamux_spv(AaFxaa${q} "${ORIG_SHADERS}/AaFxaa.hlsl" -DFXAA_PRESET=${q})
endforeach()
# SMAA 1x (iryoku/smaa, MIT, via Veyra / PushFPS) and the colour effects texture / clarity / dehaze (Veyra)
foreach(s AaSmaaEdges AaSmaaBlend ColorDetailReduce ColorDetailBlur ColorDetailApply)
  veyamux_spv(${s} "${ORIG_SHADERS}/${s}.hlsl")
endforeach()
foreach(q MEDIUM HIGH ULTRA)
  veyamux_spv(AaSmaaWeights${q} "${ORIG_SHADERS}/AaSmaaWeights.hlsl" -DSMAA_PRESET_${q})
endforeach()
foreach(s LxEnhance LxYuv LxLuma LxLkLevel LxMvFill LxDownscale LxMvSelect LxNlm LxTemporalNr LxP010 LxHdrRgb LxNv12 LxPqOut LxStatic LxDetail LxCrtGeom LxPixelGrid LxDeflicker LxFit LxHudUpdate LxHudCompose LxCompare)
  veyamux_spv(${s} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s}.hlsl")
endforeach()
# AMD FSR 1 (EASU + RCAS) from the FidelityFX SDK headers
veyamux_spv(LxFsr1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/LxFsr1.hlsl" -I "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx/include/FidelityFX/gpu")
# Snapdragon Game Super Resolution 1 (BSD-3-Clause)
veyamux_spv(LxSgsr "${CMAKE_CURRENT_SOURCE_DIR}/shaders/LxSgsr.hlsl" -I "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sgsr")
# NVIDIA Image Scaling (MIT)
veyamux_spv(LxNis "${CMAKE_CURRENT_SOURCE_DIR}/shaders/LxNis.hlsl" -I "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nis")
add_custom_target(veyamux_shaders ALL DEPENDS ${SPV_OUTPUTS})


# ---- AMD FidelityFX SDK 1.1.4 framework + FSR 3.1.6 effect code from SDK 2.4.0 (MIT): FSR 3.1.6 upscaling + frame generation, Vulkan backend ----
# FidelityFX_SC.exe is Windows-only; ffx_sc_linux.py + ffx_reflect generate the same permutation headers here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(VEYAMUX_GLSLANG glslangValidator REQUIRED)
set(FFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx")
set(FFX_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx_tools")
set(FFX_GEN_DIR "${CMAKE_BINARY_DIR}/ffx_shaders")
add_executable(ffx_reflect "${FFX_TOOLS}/ffx_reflect.c" "${FFX_TOOLS}/SPIRV-Reflect/spirv_reflect.c")
target_include_directories(ffx_reflect PRIVATE "${FFX_TOOLS}/SPIRV-Reflect")
set_target_properties(ffx_reflect PROPERTIES LINKER_LANGUAGE C)
file(GLOB_RECURSE FFX_SHADER_SOURCES "${FFX_DIR}/src/backends/vk/shaders/*.glsl" "${FFX_DIR}/include/FidelityFX/gpu/*.h")
add_custom_command(OUTPUT "${FFX_GEN_DIR}/stamp"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${FFX_GEN_DIR}"
  COMMAND "${FFX_TOOLS}/gen_ffx_shaders.sh" "${FFX_DIR}" "${FFX_GEN_DIR}" "$<TARGET_FILE:ffx_reflect>" "${VEYAMUX_GLSLANG}"
  COMMAND ${CMAKE_COMMAND} -E touch "${FFX_GEN_DIR}/stamp"
  DEPENDS ffx_reflect ${FFX_SHADER_SOURCES} "${FFX_TOOLS}/gen_ffx_shaders.sh" "${FFX_TOOLS}/ffx_sc_linux.py"
  COMMENT "FidelityFX: generating Vulkan shader permutations" VERBATIM)
add_custom_target(ffx_shaders DEPENDS "${FFX_GEN_DIR}/stamp")
add_library(veyamux_ffx STATIC
  ${FFX_DIR}/src/shared/ffx_assert.cpp ${FFX_DIR}/src/shared/ffx_breadcrumbs_list.cpp
  ${FFX_DIR}/src/shared/ffx_message.cpp ${FFX_DIR}/src/shared/ffx_object_management.cpp
  ${FFX_DIR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
  ${FFX_DIR}/src/components/frameinterpolation/ffx_frameinterpolation.cpp
  ${FFX_DIR}/src/components/opticalflow/ffx_opticalflow.cpp
  ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
  ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
  ${FFX_DIR}/src/backends/shared/ffx_shader_blobs.cpp
  ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
  ${FFX_DIR}/src/backends/vk/ffx_vk.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx_linux/ffx_linux_stubs.cpp)
add_dependencies(veyamux_ffx ffx_shaders)
target_compile_definitions(veyamux_ffx PRIVATE FFX_FSR3UPSCALER FFX_FI FFX_OF FFX_GCC)
target_compile_options(veyamux_ffx PRIVATE -w -include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx_linux/ffx_linux_compat.h")
target_include_directories(veyamux_ffx PRIVATE "${FFX_DIR}/src/components" "${FFX_DIR}/src/shared" "${FFX_DIR}/src/backends/shared" "${FFX_GEN_DIR}")
target_include_directories(veyamux_ffx PUBLIC "${FFX_DIR}/include" "${FFX_DIR}/include/FidelityFX/host" "${FFX_DIR}/include/FidelityFX/host/backends/vk")
target_link_libraries(veyamux_ffx PUBLIC Vulkan::Vulkan)

# ---- Core (no Qt): Vulkan context + enhancement chain ----
find_package(Threads REQUIRED)
add_library(veyamux_core STATIC third_party/smaa/SmaaLookupTables.cpp src/VkContext.cpp src/EnhanceChain.cpp src/GpuTimer.cpp src/NvidiaVfg.cpp src/FsrUpscaler.cpp src/FsrFrameGen.cpp src/MotionEstimator.cpp)
target_include_directories(veyamux_core PUBLIC src third_party/smaa)
target_link_libraries(veyamux_core PUBLIC Vulkan::Vulkan veyamux_ffx Threads::Threads ${CMAKE_DL_LIBS})
include(cmake/VeyamuxDlss.cmake)
target_compile_options(veyamux_core PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx_linux/ffx_linux_compat.h")
add_executable(veyamux-selftest src/selftest.cpp)
target_link_libraries(veyamux-selftest PRIVATE veyamux_core)
add_dependencies(veyamux-selftest veyamux_shaders)

# ---- Media engine (FFmpeg + PulseAudio/PipeWire), still Qt-free ----
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)
# ---- chiaki-ng (PlayStation Remote Play, AGPL-3.0) ----
include(cmake/chiaki.cmake)

add_library(veyamux_media STATIC src/MediaEngine.cpp src/CaptureSource.cpp src/DemoSource.cpp src/RemotePlay.cpp src/ContentProbe.cpp src/Recorder.cpp src/ScreenCast.cpp src/PulseDevices.cpp src/AudioMeter.cpp)
target_include_directories(veyamux_media PUBLIC src)
# PipeWire (window/screen source): headers only, libpipewire-0.3 is loaded from the system at run time
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
target_include_directories(veyamux_media PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(veyamux_media PRIVATE ${CMAKE_DL_LIBS})
target_link_libraries(veyamux_media PUBLIC veyamux_core PkgConfig::FFMPEG PkgConfig::PULSE Threads::Threads veyamux_chiaki)
find_package(Threads REQUIRED)
add_executable(veyamux-ps-decode-test src/ps_decode_test.cpp)
target_link_libraries(veyamux-ps-decode-test PRIVATE veyamux_media)
add_executable(veyamux-play-test src/play_test.cpp)
target_link_libraries(veyamux-play-test PRIVATE veyamux_media)
add_dependencies(veyamux-play-test veyamux_shaders)

# ---- Demo scene "Abyssal Garden" (WebGPU through wgpu-native, prebuilt: scripts/fetch-tools.sh) ----
set(VEYAMUX_WGPU_ROOT "${CMAKE_SOURCE_DIR}/.tools/wgpu" CACHE PATH "wgpu-native release (include/, lib/)")
if(EXISTS "${VEYAMUX_WGPU_ROOT}/lib/libwgpu_native.a")
  file(GLOB ABYSSAL_WGSL "${CMAKE_SOURCE_DIR}/src/abyssal/wgsl/*.wgsl")
  set(ABYSSAL_WGSL_H "${CMAKE_BINARY_DIR}/generated/abyssal_wgsl.h")
  add_custom_command(OUTPUT "${ABYSSAL_WGSL_H}"
    COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_SOURCE_DIR}/src/abyssal/wgsl -DOUT=${ABYSSAL_WGSL_H} -P ${CMAKE_SOURCE_DIR}/cmake/embed_wgsl.cmake
    DEPENDS ${ABYSSAL_WGSL} "${CMAKE_SOURCE_DIR}/cmake/embed_wgsl.cmake" VERBATIM)
  add_library(veyamux_abyssal STATIC src/abyssal/AbyssalMath.cpp src/abyssal/AbyssalWorld.cpp src/abyssal/AbyssalScene.cpp
    src/abyssal/AbyssalAudio.cpp "${ABYSSAL_WGSL_H}")
  target_include_directories(veyamux_abyssal PUBLIC src/abyssal PRIVATE "${CMAKE_BINARY_DIR}/generated" "${VEYAMUX_WGPU_ROOT}/include")
  target_link_libraries(veyamux_abyssal PRIVATE "${VEYAMUX_WGPU_ROOT}/lib/libwgpu_native.a" ${CMAKE_DL_LIBS} Threads::Threads m)
  target_compile_definitions(veyamux_abyssal PUBLIC VEYAMUX_HAVE_ABYSSAL=1)
  add_executable(veyamux-abyssal-test src/abyssal/abyssal_test.cpp)
  target_link_libraries(veyamux-abyssal-test PRIVATE veyamux_abyssal)
  target_link_libraries(veyamux_media PUBLIC veyamux_abyssal)
  message(STATUS "Abyssal Garden demo: wgpu-native from ${VEYAMUX_WGPU_ROOT}")
else()
  message(WARNING "wgpu-native not found in ${VEYAMUX_WGPU_ROOT}: the demo scene is left out (scripts/fetch-tools.sh fetches it)")
endif()

# ---- Application (Qt 6 / QML) ----
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick Widgets Svg Network DBus)
qt_standard_project_setup()
qt_add_executable(veyamux src/DisplayIdleInhibitor.cpp src/VfgAmpereCapability.cpp src/PlayerBridgeNvidia.cpp src/main.cpp src/PlayerBridge.cpp src/PlayerBridgeRemotePlay.cpp src/PlayerBridgeSetup.cpp src/PlayerBridgeUpdate.cpp src/AppUpdate.cpp src/AppUpdate.h src/PlayerBridge.h src/VideoItem.cpp src/VideoItem.h src/LiveVulkan.cpp src/LiveVulkan.h src/LosslessScaling.cpp src/ObsStream.cpp src/LosslessScaling.h src/LsfgInstall.cpp src/LsfgInstall.h src/DisplayVrr.cpp src/DisplayVrr.h src/FramePacingRepair.h src/SteamShortcut.cpp src/SteamShortcut.h src/Watcher.cpp src/Watcher.h src/ScreenCastPortal.cpp src/ScreenCastPortal.h src/Gamepad.cpp src/Gamepad.h)
set(VEYAMUX_RELEASE_VERSION "${PROJECT_VERSION}-nvidia-test2.2" CACHE STRING "Version displayed by the NVIDIA fork")
if(NOT VEYAMUX_RELEASE_VERSION MATCHES "^[0-9]+([.][0-9]+)*([+-][0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "Invalid VEYAMUX_RELEASE_VERSION")
endif()
target_compile_definitions(veyamux PRIVATE VEYAMUX_RELEASE_VERSION="${VEYAMUX_RELEASE_VERSION}")
# Qt Quick shaders for the HDR display output (Qt Shader Baker: qsb, package qt6-shader-baker)
find_program(VEYAMUX_QSB qsb PATHS /usr/lib/qt6/bin /usr/lib/qt6/libexec ${QT6_INSTALL_PREFIX}/bin REQUIRED)
set(QSB_OUT "${CMAKE_BINARY_DIR}/qsb")
set(QSB_FILES "")
foreach(f pq_video hdr_window)
  add_custom_command(OUTPUT "${QSB_OUT}/${f}.frag.qsb"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${QSB_OUT}"
    COMMAND "${VEYAMUX_QSB}" --glsl "100 es,120,150" -o "${QSB_OUT}/${f}.frag.qsb" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/qt/${f}.frag"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/qt/${f}.frag" COMMENT "qsb ${f}.frag" VERBATIM)
  list(APPEND QSB_FILES "${QSB_OUT}/${f}.frag.qsb")
  set_source_files_properties("${QSB_OUT}/${f}.frag.qsb" PROPERTIES QT_RESOURCE_ALIAS "shaders/${f}.frag.qsb")
endforeach()
qt_add_resources(veyamux "hdrshaders" PREFIX "/" FILES ${QSB_FILES})
qt_add_resources(veyamux "qml" PREFIX "/" FILES
  qml/qmldir qml/Theme.qml qml/Strings.js qml/Main.qml qml/Menu.qml qml/CaptureDialog.qml qml/RemotePlayDialog.qml qml/FrameGenSection.qml qml/ObsSection.qml qml/RecordSection.qml qml/IconButton.qml qml/VSlider.qml qml/Segmented.qml qml/ToggleSwitch.qml
  qml/icons/capture.svg qml/icons/play.svg qml/icons/pause.svg qml/icons/volume.svg qml/icons/mute.svg qml/icons/fullscreen.svg
  qml/icons/exitfs.svg qml/icons/folder.svg qml/icons/sparkles.svg qml/icons/back10.svg qml/icons/fwd10.svg qml/icons/logo.svg
  qml/icons/picture.svg qml/icons/display.svg qml/icons/record.svg qml/icons/system.svg qml/icons/gamepad.svg qml/icons/download.svg qml/icons/check.svg qml/icons/close.svg qml/icons/waves.svg
  qml/icons/logo-vice.svg qml/icons/logo-code.svg qml/icons/logo-inferno.svg qml/icons/logo-retro.svg
  qml/fonts/PressStart2P-Regular.ttf qml/fonts/Cinzel-Variable.ttf
  i18n/messages.json)
target_link_libraries(veyamux PRIVATE veyamux_media Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets Qt6::Svg Qt6::Network Qt6::DBus)
add_dependencies(veyamux veyamux_shaders)

include(GNUInstallDirs)
add_executable(veyamux-nvidia-vfg-probe tools/nvidia_vfg_probe.cpp src/VfgAmpereCapability.cpp)
target_link_libraries(veyamux-nvidia-vfg-probe PRIVATE veyamux_core)
target_link_options(veyamux-nvidia-vfg-probe PRIVATE "LINKER:--export-dynamic-symbol=vxDeviceGetAttribute")
target_link_options(veyamux PRIVATE "LINKER:--export-dynamic-symbol=vxDeviceGetAttribute")
install(FILES scripts/install-nvidia-vfg.py DESTINATION ${CMAKE_INSTALL_DATADIR}/veyamux)
install(FILES docs/licenses/RTX30-Broadcast-FrameGen-MIT.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/veyamux/licenses)
install(TARGETS veyamux veyamux-nvidia-vfg-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${SPV_DIR}/" DESTINATION ${CMAKE_INSTALL_DATADIR}/veyamux/shaders FILES_MATCHING PATTERN "*.spv")
install(FILES packaging/veyamux.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/veyamux.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/veyamux.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES THIRD_PARTY_NOTICES_LINUX.md third_party/fidelityfx/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/veyamux)
install(FILES third_party/ffx_tools/SPIRV-Reflect/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/veyamux RENAME LICENSE.SPIRV-Reflect)

add_executable(veyamux-capture-test src/capture_test.cpp)
target_link_libraries(veyamux-capture-test PRIVATE veyamux_media)

add_executable(veyamux-update-test src/update_test.cpp src/AppUpdate.cpp)
target_link_libraries(veyamux-update-test PRIVATE Qt6::Core CURL::libcurl)
add_executable(veyamux-probe-test src/probe_test.cpp src/ContentProbe.cpp)
target_link_libraries(veyamux-probe-test PRIVATE Threads::Threads)
add_executable(veyamux-upscale-test src/upscale_test.cpp)
target_link_libraries(veyamux-upscale-test PRIVATE veyamux_core)
add_dependencies(veyamux-upscale-test veyamux_shaders)
