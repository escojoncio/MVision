# IA VR: percepción enemiga (soldados) y localizador de cámaras. Uso (desde la raíz del repo):
#   make -C vr/ai test        pruebas con datos sintéticos (+ overlays extraídas si hay build/ov)
#   make -C vr/ai test-san    lo mismo con ASan/UBSan
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
SRC := vr_ai_scan.c vr_perception.c vr_cameras.c
$(B)/test_perception: test_perception.c $(SRC) vr_ai_scan.h vr_perception.h vr_cameras.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I. -o $@ test_perception.c $(SRC) -lm
test: $(B)/test_perception
	$(B)/test_perception ../../build/ov
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clean:
	rm -rf build build-san
.PHONY: test test-san clean

# --- vr_activity: actividad de soldados y sensores independiente de la cámara original ---
#   make -C vr/ai test-activity        pruebas sintéticas
#   make -C vr/ai test-activity-san    lo mismo con ASan/UBSan
#   vr/ai/vr_activity_build_ai2.sh all  copia local del runtime (build-ai2/) con coste y parches; ver vr_activity_run.c
ACT_SRC := vr_activity.c ../bridge/bridge.c ../bridge/bridge_addrs.c
$(B)/test_vr_activity: test_vr_activity.c $(ACT_SRC) vr_activity.h ../bridge/bridge.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I. -I../bridge -o $@ test_vr_activity.c $(ACT_SRC)
test-activity: $(B)/test_vr_activity
	$(B)/test_vr_activity
test-activity-san:
	$(MAKE) test-activity B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: test-activity test-activity-san

# --- vr_cameras: cámaras de vigilancia (visibles, audibles de lejos, destruibles) ---
#   make -C vr/ai test-cameras        pruebas sintéticas + clasificación sobre los overlays reales (../../build/ov)
#   make -C vr/ai test-cameras-san    lo mismo con ASan/UBSan
CAM_SRC := vr_ai_scan.c vr_cameras.c
$(B)/test_cameras: test_cameras.c $(CAM_SRC) vr_ai_scan.h vr_cameras.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I. -o $@ test_cameras.c $(CAM_SRC) -lm
test-cameras: $(B)/test_cameras
	$(B)/test_cameras ../../build/ov
test-cameras-san:
	$(MAKE) test-cameras B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: test-cameras test-cameras-san
