# Verzeichnisdienst (CMake-Build des ESP8266_RTOS_SDK); gleiche Quellen wie component.mk.
set(COMPONENT_SRCDIRS "src")
set(COMPONENT_ADD_INCLUDEDIRS "include")
set(COMPONENT_REQUIRES kiwi_core)
register_component()
target_compile_definitions(${COMPONENT_LIB} PRIVATE KIWI_DIR_VAL_W=128 KIWI_DIR_WRITER_COUNTRIES=96)
