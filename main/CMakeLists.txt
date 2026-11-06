set(COMPONENT_SRCS "app_main.c" "sink_pdm.c" "net_console.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES kiwi_core)

register_component()
