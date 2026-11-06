# Portabler KiwiSDR-Kern als Komponente des ESP8266_RTOS_SDK (CMake-Build).
set(COMPONENT_SRCDIRS "src")
set(COMPONENT_ADD_INCLUDEDIRS "include")
set(COMPONENT_REQUIRES lwip)

register_component()

# schaltet in port_sockets.c den ESP8266-Zweig ein
target_compile_definitions(${COMPONENT_LIB} PRIVATE KIWI_TARGET_ESP8266)
if(CONFIG_KIWI_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIWI_DEBUG)
endif()
