FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(
    SRCS SRCS ${app_sources}          
    REQUIRES usb_host_uac
             usb
             "spiffs"  
)
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)

