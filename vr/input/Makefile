# Entrada VR (C puro). Desde la raíz del repo:
#   make -C vr/input test        pruebas
#   make -C vr/input test-san    pruebas con -fsanitize=address,undefined
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
all: $(B)/libvr_input.a
$(B):
	mkdir -p $(B)
$(B)/vr_input.o: vr_input.c vr_input.h | $(B)
	$(CC) $(CFLAGS) -I. -c $< -o $@
$(B)/vr_gesture.o: vr_gesture.c vr_gesture.h | $(B)
	$(CC) $(CFLAGS) -I. -c $< -o $@
$(B)/vr_melee.o: vr_melee.c vr_melee.h | $(B)
	$(CC) $(CFLAGS) -I. -c $< -o $@
$(B)/libvr_input.a: $(B)/vr_input.o $(B)/vr_gesture.o $(B)/vr_melee.o
	rm -f $@ && ar rcs $@ $^
$(B)/test_vr_gesture: test_vr_gesture.c $(B)/libvr_input.a
	$(CC) $(CFLAGS) -I. $< $(B)/libvr_input.a -lm -o $@
$(B)/test_vr_input: test_vr_input.c $(B)/libvr_input.a
	$(CC) $(CFLAGS) -I. $< $(B)/libvr_input.a -lm -o $@
$(B)/test_vr_melee: test_vr_melee.c $(B)/libvr_input.a
	$(CC) $(CFLAGS) -I. $< $(B)/libvr_input.a -lm -o $@
$(B)/test_vr_nopunch: test_vr_nopunch.c vr_nopunch.h | $(B)
	$(CC) $(CFLAGS) -I. $< -o $@
test: $(B)/test_vr_input $(B)/test_vr_gesture $(B)/test_vr_melee $(B)/test_vr_nopunch
	$(B)/test_vr_input
	$(B)/test_vr_gesture
	$(B)/test_vr_melee
	$(B)/test_vr_nopunch
# rutinas reales del motor interpretadas sobre una instantánea de RAM local (VRM_DUMPS=<dir con body.bin>; sin ella la prueba se omite)
RTSRC = $(addprefix ../../runtime/,$(filter-out headless.c test_%.c wip_%.c nogen.c,$(notdir $(wildcard ../../runtime/*.c))))
$(B)/test_vr_melee_real: test_vr_melee_real.c ../host/vr_melee_rt.c $(B)/libvr_input.a | $(B)
	$(CC) -O1 -g -w -std=gnu11 -I. -I../host -I../../runtime $< ../host/vr_melee_rt.c $(RTSRC) $(B)/libvr_input.a -lm -lpthread -o $@
test-real: $(B)/test_vr_melee_real
	$(B)/test_vr_melee_real
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clean:
	rm -rf build build-san
.PHONY: all test test-real test-san clean
