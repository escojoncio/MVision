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
CMPSRC = vr/native/ns_fxcmp.c
$(B)/test_ns_fxcmp: vr/native/test_ns_fxcmp.c $(SRC) $(GPUSRC) $(CMPSRC) $(HDR) vr/native/ns_fxcmp.h vr/native/ns_gpu.h | $(B)
	$(CC) $(CF) vr/native/test_ns_fxcmp.c $(SRC) $(GPUSRC) $(CMPSRC) -lm -o $@
$(B)/test_ns_fxcmp_san: vr/native/test_ns_fxcmp.c $(SRC) $(GPUSRC) $(CMPSRC) $(HDR) vr/native/ns_fxcmp.h vr/native/ns_gpu.h | $(B)
	$(CC) $(CF) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined vr/native/test_ns_fxcmp.c $(SRC) $(GPUSRC) $(CMPSRC) -lm -o $@
SCRSRC = vr/native/ns_screen.c
$(B)/test_ns_screen: vr/native/test_ns_screen.c $(SCRSRC) vr/native/ns_screen.h vr/native/ns_gpu_types.h $(HDR) | $(B)
	$(CC) $(CF) vr/native/test_ns_screen.c $(SCRSRC) -lm -o $@
$(B)/test_ns_screen_san: vr/native/test_ns_screen.c $(SCRSRC) vr/native/ns_screen.h vr/native/ns_gpu_types.h $(HDR) | $(B)
	$(CC) $(CF) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined vr/native/test_ns_screen.c $(SCRSRC) -lm -o $@
PRSRC = vr/native/ns_present.c vr/native/ns_cinema.c
$(B)/test_ns_present: vr/native/test_ns_present.c $(PRSRC) vr/native/ns_present.h vr/native/ns_cinema.h vr/native/ns_types.h | $(B)
	$(CC) $(CF) vr/native/test_ns_present.c $(PRSRC) -lm -o $@
$(B)/test_ns_present_san: vr/native/test_ns_present.c $(PRSRC) vr/native/ns_present.h vr/native/ns_cinema.h vr/native/ns_types.h | $(B)
	$(CC) $(CF) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined vr/native/test_ns_present.c $(PRSRC) -lm -o $@
$(B)/test_ns_wave: vr/native/test_ns_wave.c $(SCRSRC) vr/native/ns_screen.h vr/native/ns_gpu_types.h $(HDR) | $(B)
	$(CC) $(CF) vr/native/test_ns_wave.c $(SCRSRC) -lm -o $@
$(B)/test_no_gpu3d: vr/native/test_no_gpu3d.c app/Sources/RenderRG.swift app/Sources/Native/NativeRenderer.swift app/Sources/Native/NativeShaders.metal vr/host/vrhost.c | $(B)
	$(CC) $(CF) vr/native/test_no_gpu3d.c -o $@
$(B)/ns_fx_dump: vr/native/ns_fx_dump.c $(SRC) $(HDR) | $(B)
	$(CC) $(CF) vr/native/ns_fx_dump.c $(SRC) -lm -o $@
dump: $(B)/ns_fx_dump
test: $(B)/test_ns_fx $(B)/test_ns_gpu_fx $(B)/test_ns_fxcmp $(B)/test_ns_screen $(B)/test_ns_present $(B)/test_ns_wave $(B)/test_no_gpu3d
	$(B)/test_ns_fx
	$(B)/test_ns_gpu_fx
	$(B)/test_ns_fxcmp
	$(B)/test_ns_screen
	$(B)/test_ns_present
	$(B)/test_ns_wave
	$(B)/test_no_gpu3d
test-san: $(B)/test_ns_fx_san $(B)/test_ns_gpu_fx_san $(B)/test_ns_fxcmp_san $(B)/test_ns_screen_san $(B)/test_ns_present_san $(B)/test_no_gpu3d
	$(B)/test_ns_fx_san
	$(B)/test_ns_gpu_fx_san
	$(B)/test_ns_fxcmp_san
	$(B)/test_ns_screen_san
	$(B)/test_ns_present_san
	$(B)/test_no_gpu3d
.PHONY: dump test test-san
