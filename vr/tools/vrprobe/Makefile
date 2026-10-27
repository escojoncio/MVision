# Probador de la capa VR (herramienta LOCAL: necesita el disco del usuario; no entra en la app ni en CI). Uso (desde la raíz del repo):
#   make -C vr/tools/vrprobe run  OUT=<carpeta de salida> [SIGS_REF=<sigs_ref.json>] [ARGS="--zonas a,b --solo barrido,stick"]
#   make -C vr/tools/vrprobe selftest      comprobaciones del rasterizador y del PNG (sin juego)
# Todo lo generado va a BUILD (ignorado por git). Los datos del juego (gamedata/) y las firmas son locales.
ifeq ($(origin CC),default)
CC = clang
endif
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
RT := ../../../runtime
VR := ../..
BUILD ?= ../../../build-probe
OUT ?= $(BUILD)/informe
EXE ?= ../../../gamedata/main.exe
DISC ?= ../../../gamedata/disc
SIGS_REF ?= ../../../gamedata/sigs_ref.json
SIGS ?= $(BUILD)/sigs_hle.json
SRC_VR := $(VR)/bridge/bridge.c $(VR)/bridge/bridge_addrs.c $(VR)/camera/vr_camera.c $(VR)/camera/vr_heading.c $(VR)/camera/vr_headcol.c $(VR)/camera/vr_camera_hle.c $(VR)/input/vr_input.c $(VR)/input/vr_gesture.c $(VR)/input/vr_melee.c $(VR)/scene/vr_scene.c $(VR)/scene/vr_layer2d.c $(VR)/scene/vr_layer2d_async.c $(VR)/scene/vr_prov.c $(VR)/scene/vr_prov_rules.c \
  $(VR)/body/vr_body.c $(VR)/body/vr_fist.c $(VR)/body/vr_hand_skin.c $(VR)/body/vr_hand_asset.c $(VR)/body/vr_body_cam.c $(VR)/body/vr_pose.c $(VR)/radar/vr_radar.c $(VR)/radar/vr_radar_hle.c \
  $(VR)/hands/vr_hands.c $(VR)/hands/vr_glove.c $(VR)/hands/vr_grip.c $(VR)/hands/vr_hand_pose.c $(filter-out $(VR)/weapons/test_%.c,$(wildcard $(VR)/weapons/*.c)) \
  $(VR)/gameplay/vr_fire.c $(VR)/gameplay/vr_fire_hle.c $(VR)/gameplay/vr_aa.c $(VR)/gameplay/vr_missile.c $(VR)/gameplay/vr_scope.c $(VR)/gameplay/vr_reload.c \
  $(VR)/ai/vrai.c $(VR)/ai/vr_ai_scan.c $(VR)/ai/vr_perception.c $(VR)/ai/vr_perception_rt.c $(VR)/ai/vr_cameras.c $(VR)/ai/vr_activity.c $(VR)/ai/vr_activity_rt.c \
  $(VR)/host/vrgame.c $(VR)/host/vrbodyg.c $(VR)/host/vr_melee_rt.c $(VR)/host/vr_view.c $(VR)/host/vr_cine.c $(VR)/host/vr_smooth.c $(VR)/host/vrhost.c $(VR)/native/ns_dumpw.c $(VR)/native/ns_bridge.c $(VR)/native/ns_player.c $(VR)/native/ns_wphide.c $(VR)/native/ns_hook.c $(VR)/native/ns_hook_rt.c
SRC_RT := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c,$(wildcard $(RT)/*.c))
INC := -I. -I$(RT) -I$(VR)/bridge -I$(VR)/camera -I$(VR)/input -I$(VR)/scene -I$(VR)/body -I$(VR)/radar -I$(VR)/hands -I$(VR)/weapons -I$(VR)/gameplay -I$(VR)/ai -I$(VR)/host -I$(VR)/native

$(BUILD)/vrprobe: vrprobe.c probe_raster.c probe_raster.h probe_view.c probe_view.h $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) $(wildcard $(VR)/*/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ vrprobe.c probe_raster.c probe_view.c $(SRC_VR) $(SRC_RT) -lm -lpthread

# probe2d: comparación por fotograma real (ref_raster) vs composición 2D+3D (vr/scene/hud_emu.c) con la cámara del juego
$(BUILD)/probe2d: probe2d.c probe_raster.c probe_raster.h $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) $(wildcard $(VR)/*/*.h) ../../../render/ref_raster.c $(VR)/scene/hud_emu.c
	@mkdir -p $(BUILD)
	$(CC) $(INC) -I../../../render -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ probe2d.c probe_raster.c ../../../render/ref_raster.c $(VR)/scene/hud_emu.c $(SRC_VR) $(SRC_RT) -lm -lpthread

run2d: $(BUILD)/probe2d $(SIGS)
	@mkdir -p $(OUT)
	MV_SIGS=$(SIGS) $(BUILD)/probe2d $(EXE) $(DISC) --out $(OUT) $(ARGS)

$(SIGS): $(SIGS_REF)
	@mkdir -p $(BUILD)
	python3 -I ../../../tools/sig/hlesigs.py $< $@ $(wildcard $(RT)/hle*.txt)

run: $(BUILD)/vrprobe $(SIGS)
	@mkdir -p $(OUT)
	MV_SIGS=$(SIGS) $(BUILD)/vrprobe $(EXE) $(DISC) --out $(OUT) $(ARGS)

$(BUILD)/selftest: selftest.c probe_raster.c probe_raster.h
	@mkdir -p $(BUILD)
	gcc -I. $(CFLAGS) -fsanitize=address,undefined -o $@ selftest.c probe_raster.c -lm

selftest: $(BUILD)/selftest
	cd $(BUILD) && ./selftest
	python3 -I checkpng.py $(BUILD)/selftest.png 8 8

.PHONY: run run2d selftest
