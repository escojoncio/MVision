# Manos/armas en el renderer VR (vr/hands). Uso (desde la raíz del repo):
#   make -C vr/hands test          pruebas sintéticas (no necesitan datos del juego) + vr_glove (con volcados reales de MV_DUMP o /home/claude/mv2/Dumps/d0001_* si existen)
#   make -C vr/hands test-san      lo mismo con ASan/UBSan
#   make -C vr/hands preview       render de referencia por software de las pistolas 36f6/21b6 (necesita gamedata/stages.bin)
#                                  -> PNG en OUT (por defecto build/preview); STAGES=ruta, STAGE=init
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
$(B)/hands_preview: hands_preview.c $(B)/vr_hands.o $(WB)/libweapons.a ../../tools/wmodel/wprev.c
	$(CC) $(CFLAGS) -Wno-misleading-indentation -DVRH_PREVIEW_BUILD $(INC) -I../../tools/wmodel hands_preview.c ../../tools/wmodel/wprev.c $(B)/vr_hands.o $(WB)/libweapons.a -lm -lpthread -o $@

$(B)/test_glove: test_glove.c vr_glove.c vr_glove.h vr_hands_frame.h ../body/vr_fist.c ../body/vr_fist.h | $(B)
	$(CC) $(CFLAGS) -I. -I../body test_glove.c vr_glove.c ../body/vr_fist.c -lm -o $@

test: $(B)/test_hands $(B)/test_glove
	$(B)/test_hands
	$(B)/test_glove
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
preview: $(B)/hands_preview
	mkdir -p $(OUT)
	$(B)/hands_preview $(STAGES) $(STAGE) $(OUT)
msl: ../../render/metal/vr_weapon.metal mock_metal/metal_stdlib
	$(CXX) -std=c++17 -fsyntax-only -Wall -Wno-attributes -Wno-unknown-attributes -Imock_metal -I../../render/metal -x c++ ../../render/metal/vr_weapon.metal
clean:
	rm -rf build build-san
.PHONY: test test-san preview msl clean
