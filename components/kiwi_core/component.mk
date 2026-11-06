#
# Portabler KiwiSDR-Kern als Komponente des ESP8266_RTOS_SDK (make-Build).
# Dieselben Quelldateien baut auch das PC-Makefile (pc/Makefile).
#
COMPONENT_ADD_INCLUDEDIRS := include
COMPONENT_SRCDIRS := src

# schaltet in port_sockets.c den ESP8266-Zweig ein (lwip_-Funktionen, FreeRTOS-Takt, esp_random)
CFLAGS += -DKIWI_TARGET_ESP8266

# Protokoll-Debugausgabe (Menue "KiwiSDR Client": KIWI_DEBUG)
ifdef CONFIG_KIWI_DEBUG
CFLAGS += -DKIWI_DEBUG
endif
