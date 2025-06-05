
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-ignored-qualifier)
add_compile_options(-Wno-format)

project(FreeRTOS_Audio_Driver_with_Interrupts_ESP32)
