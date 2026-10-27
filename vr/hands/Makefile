# Manos/armas en el renderer VR (vr/hands). Uso (desde la raíz del repo):
#   make -C vr/hands test          pruebas sintéticas (no necesitan datos del juego) + vr_glove (con volcados reales de MV_DUMP o /home/claude/mv2/Dumps/d0001_* si existen)
#                                  + vr_hand_pose (poses de la mano nueva) + vr_grip (agarre por arma; con STAGES y datos del juego, también con el modelo real)
#   make -C vr/hands test-san      lo mismo con ASan/UBSan
#   make -C vr/hands preview       render de referencia por software de las pistolas 36f6/21b6 (necesita gamedata/stages.bin)
#                                  -> PNG en OUT (por defecto build/preview); STAGES=ruta, STAGE=init
#   make -C vr/hands preview-poses una imagen por pose de la mano nueva (no necesita datos del juego) -> PNG en POSES_OUT (por defecto $(B)/poses)
#   make -C vr/hands msl           comprueba la sintaxis de render/metal/vr_weapon.metal compilándolo como C++ con cabeceras simuladas
CC ?= gcc
CXX ?= g++
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
W := ../weapons
WB := $(W)/build
STAGES ?= ../../gamedata/stages.bin
STAGE ?= init
OUT ?= $(B)/preview
INC := -I. -I$(W)

# libweapons.a lo construye vr/weapons (no se toca)
$(WB)/libweapons.a:
	$(MAKE) -C $(W) lib

$(B):
	mkdir -p $(B)
$(B)/vr_hands.o: vr_hands.c vr_hands.h vr_hands_frame.h | $(B)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(B)/test_hands: test_hands.c $(B)/vr_hands.o $(WB)/libweapons.a
	$(CC) $(CFLAGS) $(INC) test_hands.c $(B)/vr_hands.o $(WB)/libweapons.a -lm -lpthread -o $@
$(B)/hands_preview: hands_preview.c $(B)/vr_hands.o $(WB)/libweapons.a ../../tools/wmodel/wprev.c vr_hand_pose.c vr_hand_pose.h ../body/vr_hand_mesh.c ../body/vr_hand_mesh.h
	$(CC) $(CFLAGS) -Wno-misleading-indentation -DVRH_PREVIEW_BUILD $(INC) -I../body -I../../tools/wmodel hands_preview.c ../../tools/wmodel/wprev.c vr_hand_pose.c ../body/vr_hand_mesh.c $(B)/vr_hands.o $(WB)/libweapons.a -lm -lpthread -o $@

$(B)/test_glove: test_glove.c vr_glove.c vr_glove.h vr_hands_frame.h ../body/vr_fist.c ../body/vr_fist.h | $(B)
	$(CC) $(CFLAGS) -I. -I../body test_glove.c vr_glove.c ../body/vr_fist.c -lm -o $@

HANDSRC := ../body/vr_hand_mesh.c vr_hand_pose.c
HANDHDR := ../body/vr_hand_mesh.h vr_hand_pose.h
$(B)/test_hand_pose: test_hand_pose.c $(HANDSRC) $(HANDHDR) | $(B)
	$(CC) $(CFLAGS) -I. -I../body test_hand_pose.c $(HANDSRC) -lm -o $@
$(B)/test_grip: test_grip.c vr_grip.c vr_grip.h $(HANDSRC) $(HANDHDR) $(B)/vr_hands.o $(WB)/libweapons.a
	$(CC) $(CFLAGS) $(INC) -I../body test_grip.c vr_grip.c $(HANDSRC) $(B)/vr_hands.o $(WB)/libweapons.a -lm -lpthread -o $@

test: $(B)/test_hands $(B)/test_glove $(B)/test_hand_pose $(B)/test_grip
	$(B)/test_hands
	$(B)/test_glove
	$(B)/test_hand_pose
	$(B)/test_grip $(STAGES)
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
POSES_OUT ?= $(B)/poses
preview-poses: $(B)/hands_preview
	mkdir -p $(POSES_OUT)
	$(B)/hands_preview --poses $(POSES_OUT)
preview: $(B)/hands_preview
	mkdir -p $(OUT)
	$(B)/hands_preview $(STAGES) $(STAGE) $(OUT)
msl: ../../render/metal/vr_weapon.metal mock_metal/metal_stdlib
	$(CXX) -std=c++17 -fsyntax-only -Wall -Wno-attributes -Wno-unknown-attributes -Imock_metal -I../../render/metal -x c++ ../../render/metal/vr_weapon.metal
clean:
	rm -rf build build-san
.PHONY: test test-san preview preview-poses msl clean
