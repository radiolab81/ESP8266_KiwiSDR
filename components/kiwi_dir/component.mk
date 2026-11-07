#
# Verzeichnisdienst (KiwiSDR-Serverliste) als Komponente des ESP8266_RTOS_SDK (make-Build).
# Dieselben Quelldateien baut auch das PC-Makefile (pc/Makefile).
#
COMPONENT_ADD_INCLUDEDIRS := include
COMPONENT_SRCDIRS := src
COMPONENT_DEPENDS := kiwi_core

# RAM-Sparprofil fuer den ESP8266 (~50 KB Heap): kleinere Parser-Puffer, kleinere Laendertabelle
CFLAGS += -DKIWI_DIR_VAL_W=128 -DKIWI_DIR_WRITER_COUNTRIES=96
