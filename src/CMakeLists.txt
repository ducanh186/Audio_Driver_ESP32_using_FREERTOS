
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources})
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)