# Pegamento host <-> capa VR. Uso (desde la raíz del repo):
#   make -C vr/host check          las direcciones de vrhost.c coinciden con vr/camera/vr_names.json
#   make -C vr/host test           prueba de vr_view (motivos e histéresis de la vista mundo/cine)
#   make -C vr/host run            ejecuta el host completo en MV_NOGEN con datos locales (ver vrhost_run.c) y comprueba las escenas
# Todo lo generado va a OUT (por defecto ../../build-host, ignorado por git); los datos del juego y el fichero de firmas
# son locales y no se suben.
ifeq ($(origin CC),default)
CC = clang
endif
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
RT := ../../runtime
OUT ?= ../../build-host
EXE ?= ../../gamedata/main.exe
DISC ?= ../../gamedata/disc
SIGS ?= $(OUT)/sigs_hle.json
SIGS_REF ?= ../../gamedata/sigs_ref.json
FRAMES ?= 7000
# Capas de vr/ que enlaza el host: puente, cámara, entrada, escena, cuerpo (IK), radar, manos/armas, disparo y recarga.
SRC_VR := ../bridge/bridge.c ../bridge/bridge_addrs.c ../camera/vr_camera.c ../camera/vr_heading.c ../camera/vr_camera_hle.c ../input/vr_input.c ../input/vr_gesture.c ../scene/vr_scene.c \
  ../body/vr_body.c ../body/vr_body_cam.c ../body/vr_pose.c ../radar/vr_radar.c ../radar/vr_radar_hle.c \
  ../hands/vr_hands.c $(filter-out ../weapons/test_%.c,$(wildcard ../weapons/*.c)) \
  ../gameplay/vr_fire.c ../gameplay/vr_fire_hle.c ../gameplay/vr_aa.c ../gameplay/vr_missile.c ../gameplay/vr_scope.c ../gameplay/vr_reload.c \
  ../ai/vrai.c ../ai/vr_ai_scan.c ../ai/vr_perception.c ../ai/vr_perception_rt.c ../ai/vr_cameras.c ../ai/vr_activity.c ../ai/vr_activity_rt.c vrgame.c vrbodyg.c vr_view.c vr_cine.c vrhost.c
SRC_RT := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c,$(wildcard $(RT)/*.c))
INC := -I. -I$(RT) -I../bridge -I../camera -I../input -I../scene -I../body -I../radar -I../hands -I../weapons -I../gameplay -I../ai

check:
	python3 -I -c "import json,re,sys; \
e={}; \
[e.update({n:int(d['target'],16) for n,d in json.load(open(f)).items()}) for f in ('../camera/vr_names.json','../gameplay/vr_fire_names.json','../body/vr_names_body.json')]; \
s=open('vrhost.c').read(); \
t={n:int(a,16) for n,a in re.findall(r'\{\"(\w+)\", 0x([0-9a-fA-F]+)u\}',s)}; \
t['vrbody_sort']=int(re.search(r'BODY_SORT_ADDR 0x([0-9a-fA-F]+)u',s).group(1),16); \
print('OK' if t==e else 'DIFERENTE'); sys.exit(0 if t==e else 1)"

$(OUT)/vrhost_run: vrhost_run.c ../scene/hud_emu.c ../scene/hud_emu.h $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ vrhost_run.c ../scene/hud_emu.c $(SRC_VR) $(SRC_RT) -lm -lpthread

$(SIGS): $(SIGS_REF)
	@mkdir -p $(OUT)
	python3 ../../tools/sig/hlesigs.py $< $@ $(wildcard $(RT)/hle*.txt)

# Validación de la IA VR sobre el host completo (guion de pruebas por fotograma; ver ../ai/test_ai_run.c y la bitácora)
$(OUT)/test_ai_run: ../ai/test_ai_run.c ../scene/hud_emu.c $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ ../ai/test_ai_run.c ../scene/hud_emu.c $(SRC_VR) $(SRC_RT) $(AITEST_EXTRA) -lm -lpthread

run: $(OUT)/vrhost_run $(SIGS)
	MV_SIGS=$(SIGS) $(OUT)/vrhost_run $(EXE) $(DISC) $(FRAMES) $(ARGS)

$(OUT)/test_view: test_view.c vr_view.c vr_view.h
	@mkdir -p $(OUT)
	$(CC) -I. $(CFLAGS) -o $@ test_view.c vr_view.c

$(OUT)/test_cine: test_cine.c vr_cine.c vr_cine.h
	@mkdir -p $(OUT)
	$(CC) -I. $(CFLAGS) -o $@ test_cine.c vr_cine.c -lm

test: $(OUT)/test_view $(OUT)/test_cine
	$(OUT)/test_view
	$(OUT)/test_cine

.PHONY: check run test

# Banco de pruebas de las mecánicas de cuerpo (bodyrun.c): host completo, sin escenas, con guion de mando/manos sintéticas por VBlank.
$(OUT)/bodyrun: bodyrun.c eyeshot.c eyeshot.h $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h vrgame.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ bodyrun.c eyeshot.c $(SRC_VR) $(SRC_RT) -lm -lpthread
.PHONY: bodyrun
bodyrun: $(OUT)/bodyrun

# Cinemáticas del motor en 3D: clasificación, planos y emulación por CPU de la pantalla 3D estéreo (cinerun.c).
$(OUT)/cinerun: cinerun.c ../scene/hud_emu.c ../scene/hud_emu.h vr_cine.h $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ cinerun.c ../scene/hud_emu.c $(SRC_VR) $(SRC_RT) -lm -lpthread
.PHONY: cinerun
cinerun: $(OUT)/cinerun

# Recorrido título -> partida con acciones de Sense sintéticas y la tabla de mandos de vr_input (padrun.c).
$(OUT)/padrun: padrun.c $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h vrgame.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ padrun.c $(SRC_VR) $(SRC_RT) -lm -lpthread
.PHONY: padrun
padrun: $(OUT)/padrun

# Prueba de integración del apuntado del lanzador antiaéreo (test_aa_run.c): host completo, manos y blanco sintéticos, juego real en MV_NOGEN.
$(OUT)/test_aa_run: test_aa_run.c $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h vrgame.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ test_aa_run.c $(SRC_VR) $(SRC_RT) -lm -lpthread
.PHONY: test_aa_run
test_aa_run: $(OUT)/test_aa_run

# Vistas especiales del arma (test_special_run.c): mira óptica del francotirador, visor antiaéreo y pantalla del misil guiado, juego real en MV_NOGEN.
$(OUT)/test_special_run: test_special_run.c $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h vrgame.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ test_special_run.c $(SRC_VR) $(SRC_RT) -lm -lpthread
.PHONY: test_special_run
test_special_run: $(OUT)/test_special_run
