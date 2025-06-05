FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/main/*.*)
idf_component_register(
    SRCS ${app_sources}          
    REQUIRES usb_host_uac
             usb
             "spiffs"  
)
add_compile_options(-Wno-format)
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)

