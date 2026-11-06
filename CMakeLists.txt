# Alternativ: CMake-Build (idf.py) des ESP8266_RTOS_SDK ab v3.3.
# Die Komponenten nutzen das SDK-v3-Muster (register_component), das auch dort gilt.
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kiwi_esp8266)
