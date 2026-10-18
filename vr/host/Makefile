# Pegamento host <-> capa VR. Uso (desde la raíz del repo):
#   make -C vr/host check          las direcciones de vrhost.c coinciden con vr/camera/vr_names.json
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
SRC_VR := ../bridge/bridge.c ../bridge/bridge_addrs.c ../camera/vr_camera.c ../camera/vr_camera_hle.c ../input/vr_input.c ../scene/vr_scene.c vrhost.c
SRC_RT := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c,$(wildcard $(RT)/*.c))
INC := -I. -I$(RT) -I../bridge -I../camera -I../input -I../scene

check:
	python3 -I -c "import json,re,sys; j=json.load(open('../camera/vr_names.json')); s=open('vrhost.c').read(); \
t={n:int(a,16) for n,a in re.findall(r'\{\"(\w+)\", 0x([0-9a-fA-F]+)u\}',s)}; \
e={n:int(d['target'],16) for n,d in j.items()}; \
print('OK' if t==e else 'DIFERENTE'); sys.exit(0 if t==e else 1)"

$(OUT)/vrhost_run: vrhost_run.c $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) vrhost.h
	@mkdir -p $(OUT)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ vrhost_run.c $(SRC_VR) $(SRC_RT) -lm -lpthread

$(SIGS): $(SIGS_REF)
	@mkdir -p $(OUT)
	python3 ../../tools/sig/hlesigs.py $< $@ $(wildcard $(RT)/hle*.txt)

run: $(OUT)/vrhost_run $(SIGS)
	MV_SIGS=$(SIGS) $(OUT)/vrhost_run $(EXE) $(DISC) $(FRAMES) $(ARGS)

.PHONY: check run
