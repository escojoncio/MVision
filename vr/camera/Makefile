# Cámara VR. Uso (desde la raíz del repo):
#   make -C vr/camera test            pruebas de la matemática (no necesitan datos del juego)
#   make -C vr/camera gen             parchea el EXE (recorte), fusiona nombres y recompila el C con los 5 sustitutos
#   make -C vr/camera camtest         compila la herramienta de validación (necesita `gen` y overlays en build/gen_ov)
#   make -C vr/camera run             ejecuta la validación con datos locales y escribe vistas previas PPM en OUT/img
#   make -C vr/camera snap LIST=l.txt servidor de instantáneas: arranca una vez, hace fork() por escenario de la lista (guiones CAM_SCRIPT) desde
#                                     la parte jugable y escribe en OUT/snap/<nombre>/ (con CAM_SERVER_WATCH=1 se queda esperando listas nuevas)
# Nada de lo generado se sube: todo va a OUT (por defecto ../../build/camvr).
CC ?= gcc
CLANG ?= clang
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
RT := ../../runtime
BR := ../bridge
RD := ../../render
OUT ?= ../../build/camvr
EXE ?= ../../gamedata/main.exe
NAMES ?= ../../gamedata/names_eu.json
GENOV ?= ../../build/gen_ov
DISC ?= ../../gamedata/disc
HFOV ?= 179
VFOV ?= 179
HH ?= 160
PATCHFLAGS ?= --no-divide --no-cull
RECOMP := ../../tools/recomp/recomp.py

all: $(OUT)/test_camera
SC := ../scene
$(OUT)/test_camera: test_camera.c vr_camera.c vr_camera.h vr_usermap.h $(SC)/vr_scene.c $(SC)/vr_scene.h $(BR)/bridge.c $(BR)/bridge_addrs.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. -I$(BR) -I$(SC) -I$(RT) test_camera.c vr_camera.c $(SC)/vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c -o $@ -lm
# Prueba sintética: el render nativo cae en el mismo sitio que la lista de la GPU (mismo mapa mundo -> usuario) y la red de coherencia lo comprueba.
$(OUT)/test_native_coh: test_native_coh.c vr_usermap.h $(SC)/vr_scene.c $(SC)/vr_scene.h $(BR)/bridge.c $(BR)/bridge_addrs.c ../native/ns_bridge.c ../native/ns_bridge.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. -I$(BR) -I$(SC) -I$(RT) -I../native test_native_coh.c $(SC)/vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c ../native/ns_bridge.c -lm -lpthread -o $@
test-native-coh: $(OUT)/test_native_coh
	$(OUT)/test_native_coh
# EXE sintético (solo las instrucciones esperadas en los puntos de parche; sin datos del juego) para comparar vrcam_patch_ram con
# patch_exe.py byte a byte. Si existe el EXE real (EXE=...), se prueba también con él.
$(OUT)/syn/orig.exe: patch_exe.py
	@mkdir -p $(OUT)/syn
	python3 -I -c "import struct,sys; sys.path.insert(0,'.'); import patch_exe as P; b=0x80010000; n=0x9D000; d=bytearray(0x800+n); d[:8]=bytes.fromhex('50532d5820455845'); struct.pack_into('<IIII',d,0x10,b,0,b,n); [struct.pack_into('<I',d,0x800+a-b,(P.OLD[r][0]<<26)|(P.OLD[r][1]&0xFFFF)) for a,r in P.SITES]; struct.pack_into('<I',d,0x800+0x8001A11C-b,0x24020800); [struct.pack_into('<I',d,0x800+a-b,o) for n,a,o,w in P.NOCULL]; open('$@','wb').write(d)"
$(OUT)/syn/patched.exe: $(OUT)/syn/orig.exe patch_exe.py
	python3 patch_exe.py $< $@ --h 160 --hfov 179 --vfov 179 --no-divide --no-cull
$(OUT)/syn/patched_wide.exe: $(OUT)/syn/orig.exe patch_exe.py
	python3 patch_exe.py $< $@ --h 160 --hfov 179 --vfov 179 --no-divide --no-cull --wide-points
test: $(OUT)/test_camera $(OUT)/syn/patched.exe $(OUT)/syn/patched_wide.exe $(OUT)/test_native_coh
	$(OUT)/test_native_coh
	VRCAM_EXE=$(OUT)/syn/orig.exe VRCAM_EXE_PATCHED=$(OUT)/syn/patched.exe VRCAM_EXE_PATCHED_WIDE=$(OUT)/syn/patched_wide.exe $(OUT)/test_camera
	@if [ -f "$(EXE)" ]; then python3 patch_exe.py $(EXE) $(OUT)/syn/real_patched.exe --h 160 --hfov 179 --vfov 179 --no-divide --no-cull && \
	  VRCAM_EXE=$(EXE) VRCAM_EXE_PATCHED=$(OUT)/syn/real_patched.exe $(OUT)/test_camera; fi
test-san: test_camera.c vr_camera.c vr_camera.h $(SC)/vr_scene.c $(SC)/vr_scene.h $(BR)/bridge.c $(BR)/bridge_addrs.c $(OUT)/syn/patched.exe $(OUT)/syn/patched_wide.exe
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. -I$(BR) -I$(SC) -I$(RT) test_camera.c vr_camera.c $(SC)/vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c -o $(OUT)/test_camera_san -lm
	VRCAM_EXE=$(OUT)/syn/orig.exe VRCAM_EXE_PATCHED=$(OUT)/syn/patched.exe VRCAM_EXE_PATCHED_WIDE=$(OUT)/syn/patched_wide.exe $(OUT)/test_camera_san

# ---- EXE parcheado + C recompilado con los sustitutos (vr_names.json/hle_vrcam.txt) ----
$(OUT)/exe/main.exe: patch_exe.py $(EXE)
	@mkdir -p $(OUT)/exe
	python3 patch_exe.py $(EXE) $@ --h $(HH) --hfov $(HFOV) --vfov $(VFOV) $(PATCHFLAGS)
	cp $(dir $(EXE))extra_funcs.txt $(OUT)/exe/ 2>/dev/null || true
$(OUT)/names.json: vr_names.py vr_names.json $(NAMES)
	python3 vr_names.py $(NAMES) $@
$(OUT)/gen/dispatch.c: $(OUT)/exe/main.exe $(OUT)/names.json hle_vrcam.txt $(wildcard $(RT)/hle*.txt) $(RECOMP)
	rm -rf $(OUT)/gen
	python3 $(RECOMP) $(OUT)/exe/main.exe $(OUT)/gen --names $(OUT)/names.json $(foreach f,$(wildcard $(RT)/hle*.txt),--hle $(f)) --hle hle_vrcam.txt
gen: $(OUT)/gen/dispatch.c
GENC := $(wildcard $(OUT)/gen/*.c)
GENO := $(patsubst $(OUT)/gen/%.c,$(OUT)/obj/%.o,$(GENC))
$(OUT)/obj/%.o: $(OUT)/gen/%.c $(OUT)/gen/funcs.h
	@mkdir -p $(OUT)/obj
	$(CLANG) -I$(RT) -I$(OUT)/gen -O2 -w -foptimize-sibling-calls -c -o $@ $<

# Copia del runtime con el parche runtime_seq.patch (gpu_capture_seq) mientras no esté en el repo.
RTP := $(OUT)/rt
RTSRC := $(patsubst $(RT)/%,$(RTP)/%,$(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c $(RT)/nogen.c,$(wildcard $(RT)/*.c)))
# FREEZE_RT=1: no recopiar si el runtime cambia (útil con varios agentes editando runtime/ a la vez).
RTDEPS := $(if $(FREEZE_RT),,$(wildcard $(RT)/*.c $(RT)/*.h))
$(RTP)/stamp: $(RTDEPS) runtime_seq.patch
	rm -rf $(RTP); mkdir -p $(RTP)
	cp $(RT)/*.c $(RT)/*.h $(RTP)/
	grep -q gpu_capture_seq $(RTP)/gpu.h || patch -s -p2 -d $(RTP) -i $(abspath runtime_seq.patch)
	touch $@
$(RTSRC): $(RTP)/stamp
OVOBJ := $(wildcard $(GENOV)/*.o)
$(OUT)/camtest: camtest.c vr_camera.c vr_camera_hle.c vrbody_weak.c vr_camera.h $(SC)/vr_scene.c $(SC)/vr_scene.h $(SC)/hud_emu.c $(SC)/hud_emu.h $(BR)/bridge.c $(BR)/bridge_addrs.c $(RD)/ref_raster.c $(RTSRC) $(GENO) $(OVOBJ)
	@mkdir -p $(OUT)
	$(CLANG) -I. -I../body -I$(BR) -I$(SC) -I$(RTP) -I$(RD) -I$(OUT)/gen -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ camtest.c vr_camera.c vr_camera_hle.c vrbody_weak.c $(SC)/vr_scene.c $(SC)/hud_emu.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RD)/ref_raster.c $(RTSRC) $(GENO) $(OVOBJ) -lm
# GENC se evalúa al leer el Makefile: primero se genera el C y luego se vuelve a invocar make.
camtest: gen
	$(MAKE) $(OUT)/camtest
run: camtest
	MV_DISC=$(DISC) $(OUT)/camtest $(OUT)/exe/main.exe $(FRAMES) $(OUT)/img
FRAMES ?= 8200
LIST ?=
snap: camtest
	@test -n "$(LIST)" || { echo "uso: make snap LIST=lista.txt (ver cabecera de camtest.c)"; exit 1; }
	CAM_SERVER=$(abspath $(LIST)) MV_DISC=$(DISC) $(OUT)/camtest $(OUT)/exe/main.exe $(FRAMES) $(OUT)/snap
clean:
	rm -rf $(OUT)
.PHONY: all test test-san gen camtest run snap clean test-native-coh

# ---- rumbo del personaje (vr_heading) y calibración de altura/escala (vr_calib): pruebas sin datos del juego ----
$(OUT)/test_heading: test_heading.c vr_heading.c vr_heading.h vr_calib.h vr_camera.c vr_camera.h $(BR)/bridge.c $(BR)/bridge_addrs.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. -I$(BR) test_heading.c vr_heading.c vr_camera.c $(BR)/bridge.c $(BR)/bridge_addrs.c -o $@ -lm
test-heading: $(OUT)/test_heading
	$(OUT)/test_heading
test-heading-san: test_heading.c vr_heading.c vr_heading.h vr_calib.h vr_camera.c vr_camera.h $(BR)/bridge.c $(BR)/bridge_addrs.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. -I$(BR) test_heading.c vr_heading.c vr_camera.c $(BR)/bridge.c $(BR)/bridge_addrs.c -o $(OUT)/test_heading_san -lm
	$(OUT)/test_heading_san
.PHONY: test-heading test-heading-san

# ---- límite de la cabeza contra el escenario (vr_headcol): pruebas sin datos del juego ----
$(OUT)/test_headcol: test_headcol.c vr_headcol.c vr_headcol.h vr_camera.h $(SC)/vr_scene.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. -I$(BR) -I$(SC) -I$(RT) test_headcol.c vr_headcol.c -o $@ -lm
# Pared de colisión del juego (hzd): el ojo no sale del conducto. Con VRHZD_DUMPS=<carpeta de volcados> comprueba también los volcados reales.
$(OUT)/test_hzd: test_hzd.c vr_hzd.c vr_hzd.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DVRHZD_STANDALONE -I. test_hzd.c vr_hzd.c -o $@ -lm
test-hzd: $(OUT)/test_hzd
	$(OUT)/test_hzd
test-headcol: $(OUT)/test_headcol
	$(OUT)/test_headcol
test-headcol-san: test_headcol.c vr_headcol.c vr_headcol.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. -I$(BR) -I$(SC) -I$(RT) test_headcol.c vr_headcol.c -o $(OUT)/test_headcol_san -lm
	$(OUT)/test_headcol_san
.PHONY: test-headcol test-headcol-san
