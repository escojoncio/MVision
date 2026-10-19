# Pruebas del puente/enganche del renderer nativo (sin datos del juego). Desde la raíz del repo:
#   make -f vr/native/integra.mk test         puente (hilos), jugador, enganche con RAM falsa e intérprete real
#   make -f vr/native/integra.mk test-san     lo mismo con ASan/UBSan
#   make -f vr/native/integra.mk test-tsan    puente con ThreadSanitizer
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= vr/native/build-integra
N := vr/native
RT := $(filter-out runtime/headless.c runtime/test_%.c runtime/wip_%.c runtime/nogen.c,$(wildcard runtime/*.c))
CORE := $(N)/ns_bridge.c $(N)/ns_player.c $(N)/ns_hook.c
$(B):
	mkdir -p $(B)
$(B)/test_ns_bridge: $(N)/test_ns_bridge.c $(N)/ns_bridge.c $(N)/ns_bridge.h $(N)/ns_types.h | $(B)
	$(CC) $(CFLAGS) -I$(N) $(N)/test_ns_bridge.c $(N)/ns_bridge.c -lm -lpthread -o $@
$(B)/test_ns_hook: $(N)/test_ns_hook.c $(CORE) $(wildcard $(N)/*.h) | $(B)
	$(CC) $(CFLAGS) -I$(N) $(N)/test_ns_hook.c $(CORE) -lm -lpthread -o $@
$(B)/test_ns_hook_rt: $(N)/test_ns_hook_rt.c $(N)/ns_hook_rt.c $(CORE) $(RT) $(wildcard $(N)/*.h runtime/*.h) | $(B)
	$(CC) -O1 -g -w -std=gnu11 -Iruntime -I$(N) $(N)/test_ns_hook_rt.c $(N)/ns_hook_rt.c $(CORE) $(RT) -lm -lpthread -o $@
test: $(B)/test_ns_bridge $(B)/test_ns_hook $(B)/test_ns_hook_rt
	$(B)/test_ns_bridge
	$(B)/test_ns_hook
	$(B)/test_ns_hook_rt
test-san:
	$(MAKE) -f $(N)/integra.mk test B=$(N)/build-integra-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
test-tsan:
	$(MAKE) -f $(N)/integra.mk $(N)/build-integra-tsan/test_ns_bridge B=$(N)/build-integra-tsan CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=thread -fno-omit-frame-pointer"
	$(N)/build-integra-tsan/test_ns_bridge
clean:
	rm -rf $(N)/build-integra $(N)/build-integra-san $(N)/build-integra-tsan
.PHONY: test test-san test-tsan clean
