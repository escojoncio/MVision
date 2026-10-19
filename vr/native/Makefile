# ns_cpuref: rasterizador de referencia por CPU del renderer nativo (herramienta/validador de escritorio; NO entra en la app). Desde la raíz del repo:
#   make -C vr/native test        pruebas sintéticas (sin datos del juego)
#   make -C vr/native test-san    lo mismo con ASan/UBSan
#   make -C vr/native nsrender    compila vr/native/build/nsrender (ver tools/nsrender.c)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
all: $(B)/nsrender
$(B):
	mkdir -p $(B)
$(B)/test_ns_cpuref: test_ns_cpuref.c ns_cpuref.c ns_cpuref.h | $(B)
	$(CC) $(CFLAGS) test_ns_cpuref.c ns_cpuref.c -lm -lpthread -o $@
$(B)/test_ns_cpuref_san: test_ns_cpuref.c ns_cpuref.c ns_cpuref.h | $(B)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined test_ns_cpuref.c ns_cpuref.c -lm -lpthread -o $@
$(B)/nsrender: tools/nsrender.c ns_cpuref.c ns_cpuref.h | $(B)
	$(CC) $(CFLAGS) -I. tools/nsrender.c ns_cpuref.c -lm -lpthread -o $@
nsrender: $(B)/nsrender
test: $(B)/test_ns_cpuref
	$(B)/test_ns_cpuref
test-san: $(B)/test_ns_cpuref_san
	$(B)/test_ns_cpuref_san
.PHONY: all test test-san nsrender
