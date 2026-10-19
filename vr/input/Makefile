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
test: $(B)/test_vr_input $(B)/test_vr_gesture $(B)/test_vr_melee
	$(B)/test_vr_input
	$(B)/test_vr_gesture
	$(B)/test_vr_melee
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clean:
	rm -rf build build-san
.PHONY: all test test-san clean
