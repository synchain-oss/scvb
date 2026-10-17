# M01 AUHostingService 进程外 shm spike —— 独立 CMake 工程。
#
# **不进根工程**:根 CMakeLists.txt / tests/CMakeLists.txt 都不 add_subdirectory 本目录,Windows 构建
# 与 gates 零影响。只在 .github/workflows/macos-oop.yml 里单独 configure:
#   cmake -S tests/macos/auhost-spike -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release -DJUCE_PATH=<JUCE>
# JUCE 版本取仓库根 .juce-version(workflow 负责按它浅克隆并断言 tag 身份)。
#
# 产物:两个 AU(同一份 SpikeProcessor.cpp,subtype 不同,模拟 SCVB 三个独立二进制)+ 宿主 auhost_spike。
# AU 选项与 SCVB 将来的三个插件一致:aufx、AU_SANDBOX_SAFE FALSE、resourceUsage 用 JUCE 默认值。
cmake_minimum_required(VERSION 3.22)

# 必须在 project() 之前(参与编译器探测);写法与 bridge 仓根 CMakeLists.txt 相同。
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SCVBAuHostSpike VERSION 0.0.1 LANGUAGES C CXX)

if(NOT APPLE)
  message(FATAL_ERROR "tests/macos/auhost-spike is macOS-only (AudioToolbox / AUHostingService).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_PATH "" CACHE PATH "Path to JUCE (tag = repo root .juce-version)")
if(NOT JUCE_PATH)
  message(FATAL_ERROR "JUCE_PATH must be set, e.g. -DJUCE_PATH=/path/to/JUCE")
endif()
add_subdirectory(${JUCE_PATH} juce)

# 两个探针的 subtype。默认值必须与 auhost_spike.cpp 的 kDefaultSubtypeA / kDefaultSubtypeB 一致。
# 留成 cache 变量是为了反向注入:把其中一个改错,宿主找不到组件 => 判定必须是 inconclusive(退出码 1)。
set(SPIKE_SUBTYPE_A "Sxsa" CACHE STRING "AU subtype of probe A")
set(SPIKE_SUBTYPE_B "Sxsb" CACHE STRING "AU subtype of probe B")

function(spike_add_probe target code role_a)
  juce_add_plugin(${target}
    COMPANY_NAME "Synchain"
    PLUGIN_MANUFACTURER_CODE Snch
    PLUGIN_CODE ${code}
    FORMATS AU
    PRODUCT_NAME "SCVB${target}"
    BUNDLE_ID "com.synchain.scvb.spike.${code}"
    AU_EXPORT_PREFIX "${target}AU"
    AU_MAIN_TYPE kAudioUnitType_Effect
    AU_SANDBOX_SAFE FALSE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
  target_sources(${target} PRIVATE SpikeProcessor.cpp)
  target_compile_definitions(${target}
    PRIVATE SPIKE_ROLE_A=${role_a}
    PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  # objc:SpikeProcessor.cpp 直接调 ObjC 运行时 C API(会合类)。
  target_link_libraries(${target}
    PRIVATE juce::juce_audio_processors objc
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endfunction()

spike_add_probe(SpikeA ${SPIKE_SUBTYPE_A} 1)
spike_add_probe(SpikeB ${SPIKE_SUBTYPE_B} 0)

add_executable(auhost_spike auhost_spike.cpp)
target_compile_options(auhost_spike PRIVATE -fblocks -Wall -Wextra)
target_link_libraries(auhost_spike PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
