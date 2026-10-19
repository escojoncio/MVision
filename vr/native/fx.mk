# ns_fx: efectos del renderer nativo (pruebas sintéticas y herramienta de escritorio). Desde la raíz del repo:
#   make -f vr/native/fx.mk test        pruebas sintéticas (sin datos del juego): extractor/clasificador y geometría de efectos del renderer (test_ns_gpu_fx)
#   make -f vr/native/fx.mk test-san    lo mismo con ASan/UBSan
#   make -f vr/native/fx.mk dump        compila vr/native/build-fx/ns_fx_dump (ver ns_fx_dump.c)
CC ?= gcc
B ?= vr/native/build-fx
CF = -O2 -g -Wall -Wextra -std=gnu11 -Iruntime -Ivr/native
SRC = vr/native/ns_fx.c vr/native/ns_fx_export.c
HDR = vr/native/ns_fx.h vr/native/ns_fx_export.h vr/native/ns_types.h
$(B):
	mkdir -p $(B)
$(B)/test_ns_fx: vr/native/test_ns_fx.c $(SRC) $(HDR) | $(B)
	$(CC) $(CF) vr/native/test_ns_fx.c $(SRC) -lm -o $@
$(B)/test_ns_fx_san: vr/native/test_ns_fx.c $(SRC) $(HDR) | $(B)
	$(CC) $(CF) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined vr/native/test_ns_fx.c $(SRC) -lm -o $@
GPUSRC = vr/native/ns_gpu.c
$(B)/test_ns_gpu_fx: vr/native/test_ns_gpu_fx.c $(SRC) $(GPUSRC) $(HDR) vr/native/ns_gpu.h vr/native/ns_gpu_types.h | $(B)
	$(CC) $(CF) vr/native/test_ns_gpu_fx.c $(SRC) $(GPUSRC) -lm -o $@
$(B)/test_ns_gpu_fx_san: vr/native/test_ns_gpu_fx.c $(SRC) $(GPUSRC) $(HDR) vr/native/ns_gpu.h vr/native/ns_gpu_types.h | $(B)
	$(CC) $(CF) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined vr/native/test_ns_gpu_fx.c $(SRC) $(GPUSRC) -lm -o $@
$(B)/ns_fx_dump: vr/native/ns_fx_dump.c $(SRC) $(HDR) | $(B)
	$(CC) $(CF) vr/native/ns_fx_dump.c $(SRC) -lm -o $@
dump: $(B)/ns_fx_dump
test: $(B)/test_ns_fx $(B)/test_ns_gpu_fx
	$(B)/test_ns_fx
	$(B)/test_ns_gpu_fx
test-san: $(B)/test_ns_fx_san $(B)/test_ns_gpu_fx_san
	$(B)/test_ns_fx_san
	$(B)/test_ns_gpu_fx_san
.PHONY: dump test test-san
