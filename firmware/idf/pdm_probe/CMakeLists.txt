# V829 PDM 麦克风 bring-up（ESP-IDF 5.4 / ESP32-S3）
# 独立于 PlatformIO 工程：这里只做"麦克风能不能出数"的硬件验证。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pdm_probe)
