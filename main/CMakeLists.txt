set(COMPONENT_SRCS "app_main.c" "sink_pdm.c" "net_console.c" "app_dir.c" "dir_store_flash.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES kiwi_core kiwi_dir)

register_component()
