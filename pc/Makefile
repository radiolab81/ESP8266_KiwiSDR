# PC-Build des portablen Kerns (gleiche Quellen wie im ESP-IDF-Projekt).
#   make          -> kiwi_pc (Client)  und  test_core
#   make test     -> Kern-Tests + End-to-End-Test gegen lokalen Mock-Kiwi
CORE := ../components/kiwi_core
CC   ?= gcc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I$(CORE)/include
SRCS := $(CORE)/src/kiwi_ws.c $(CORE)/src/kiwi_adpcm.c $(CORE)/src/kiwi_client.c $(CORE)/src/port_sockets.c $(CORE)/src/kiwi_pdm.c

# Verzeichnisdienst (Serverliste, Datenbank, Auswahl) - eigene Komponente neben kiwi_core
DIR  := ../components/kiwi_dir
CFLAGS += -I$(DIR)/include
DSRCS := $(wildcard $(DIR)/src/*.c)
# RAM-Sparprofil des ESP8266 (wie components/kiwi_dir/component.mk)
SMALL := -DKIWI_DIR_VAL_W=128 -DKIWI_DIR_WRITER_COUNTRIES=96

all: kiwi_pc test_core test_pdm sim_sink kiwi_dirtool test_dir test_dir_small

kiwi_pc: main_pc.c sink_wav.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

kiwi_pc_debug: main_pc.c sink_wav.c $(SRCS)
	$(CC) $(CFLAGS) -DKIWI_DEBUG -o $@ $^

test_core: test_core.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_pdm: test_pdm.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

kiwi_dirtool: kiwi_dirtool.c store_file.c $(DSRCS) $(SRCS)
	$(CC) $(CFLAGS) $(SMALL) -o $@ $^

test_dir: test_dir.c $(DSRCS) $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# gleicher Test mit dem kleinen ESP8266-Profil
test_dir_small: test_dir.c $(DSRCS) $(SRCS)
	$(CC) $(CFLAGS) $(SMALL) -o $@ $^

# Simulation der ECHTEN main/sink_pdm.c am PC (FreeRTOS/I2S durch Simulationen ersetzt)
sim_sink: sim_sink_pdm.c ../main/sink_pdm.c $(CORE)/src/kiwi_pdm.c
	$(CC) $(CFLAGS) -Isim -I../main -o $@ $^ -lm -lpthread

test: all
	./test_core
	./test_pdm
	./sim_sink
	python3 e2e_test.py
	./test_dir
	./test_dir_small
	python3 e2e_dir_test.py

clean:
	rm -f kiwi_pc kiwi_pc_debug test_core test_pdm sim_sink kiwi_dirtool test_dir test_dir_small *.wav e2e_dir_*.bin

.PHONY: all test clean
