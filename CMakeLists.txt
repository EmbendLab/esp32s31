# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(softAP)

# CMSIS-DAP HID 兼容口使用 512B vendor report（与 bulk 包 / DAP_Info PacketSize 对齐）。
# esp_tinyusb 未暴露该 Kconfig，默认 CFG_TUD_HID_EP_BUFSIZE=64 装不下整帧；
# 作为全局编译定义下发给所有组件（tinyusb 的 hid_device.c 需据此放大内部 EP 缓冲）。
idf_build_set_property(COMPILE_OPTIONS "-DCFG_TUD_HID_EP_BUFSIZE=512" APPEND)
