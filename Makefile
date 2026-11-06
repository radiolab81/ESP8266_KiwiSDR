#
# Make-Build fuer das ESP8266_RTOS_SDK (Standardweg des SDK v3.x):
#
#   export IDF_PATH=~/ESP8266_RTOS_SDK
#   make menuconfig          # Menue "KiwiSDR Client (ESP8266)" ausfuellen
#   make -j4 flash monitor
#
PROJECT_NAME := kiwi_esp8266

include $(IDF_PATH)/make/project.mk
