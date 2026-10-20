# Extractor de la escena nativa (ns_extract + ns_adapt + herramienta nshito1). Uso (desde la raíz del repo):
#   make -f vr/native/extract.mk test         pruebas sintéticas del extractor y del adaptador
#   make -f vr/native/extract.mk test-san     lo mismo con ASan/UBSan (UB = fallo)
#   make -f vr/native/extract.mk nshito1      compila vr/native/build-extract/nshito1 (ver tools/nshito1.c)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= vr/native/build-extract
SRC := vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_cpuref.c
HDR := vr/native/ns_extract.h vr/native/ns_adapt.h vr/native/ns_types.h vr/native/ns_light.h vr/native/ns_cpuref.h
$(B):
	mkdir -p $(B)
$(B)/test_ns_extract: vr/native/test_ns_extract.c $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS) -Ivr/native vr/native/test_ns_extract.c $(SRC) -lm -lpthread -o $@
$(B)/nshito1: vr/native/tools/nshito1.c vr/native/ns_dump.c vr/native/ns_dump.h $(SRC) $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/tools/nshito1.c vr/native/ns_dump.c $(SRC) -lm -lpthread -o $@
nshito1: $(B)/nshito1
test: $(B)/test_ns_extract
	$(B)/test_ns_extract
test-san:
	$(MAKE) -f vr/native/extract.mk test B=vr/native/build-extract-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clean:
	rm -rf vr/native/build-extract vr/native/build-extract-san
.PHONY: test test-san clean nshito1

# Cobertura de la lista de la GPU por la escena nativa (ns_cover): prueba sintética y herramienta de volcados.
#   make -f vr/native/extract.mk test-cover    pruebas sintéticas de ns_cover
#   make -f vr/native/extract.mk nscover       compila vr/native/build-extract/nscover (ver tools/nscover.c)
COVSRC := vr/native/ns_cover.c vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_fx.c
$(B)/test_ns_cover: vr/native/test_ns_cover.c vr/native/ns_cover_glue.c $(COVSRC) vr/native/ns_cover.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/test_ns_cover.c vr/native/ns_cover_glue.c $(COVSRC) -lm -lpthread -o $@
$(B)/nscover: vr/native/tools/nscover.c vr/native/ns_dump.c $(COVSRC) vr/native/ns_cover.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/tools/nscover.c vr/native/ns_dump.c $(COVSRC) -lm -lpthread -o $@
nscover: $(B)/nscover
test-cover: $(B)/test_ns_cover
	$(B)/test_ns_cover
test-cover-san:
	$(MAKE) -f vr/native/extract.mk test-cover B=vr/native/build-extract-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: nscover test-cover test-cover-san

# Geometría sólida de la escena nativa (ns_solid: colisión de la cabeza) y análisis de huecos del escenario sobre volcados.
#   make -f vr/native/extract.mk test-solid   pruebas sintéticas de ns_solid
#   make -f vr/native/extract.mk nssolid      compila vr/native/build-extract/nssolid (ver tools/nssolid.c)
SOLSRC := vr/native/ns_solid.c vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c
$(B)/test_ns_solid: vr/native/test_ns_solid.c $(SOLSRC) vr/native/ns_solid.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native -Ivr/camera -Ivr/scene -Ivr/bridge vr/native/test_ns_solid.c $(SOLSRC) vr/camera/vr_headcol.c -lm -lpthread -o $@
$(B)/nssolid: vr/native/tools/nssolid.c vr/native/ns_dump.c $(SOLSRC) vr/native/ns_solid.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/tools/nssolid.c vr/native/ns_dump.c $(SOLSRC) -lm -lpthread -o $@
nssolid: $(B)/nssolid
test-solid: $(B)/test_ns_solid
	$(B)/test_ns_solid
test-solid-san:
	$(MAKE) -f vr/native/extract.mk test-solid B=vr/native/build-extract-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: nssolid test-solid test-solid-san

# Manos articuladas propias (ns_hand + vr/body/vr_hand_mesh): escena y manopla sintéticas.
#   make -f vr/native/extract.mk test-hand
HANDSRC := vr/native/ns_hand.c vr/native/ns_player.c vr/native/ns_solid.c vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_gpu.c vr/body/vr_hand_mesh.c
$(B)/test_ns_hand: vr/native/test_ns_hand.c $(HANDSRC) vr/native/ns_hand.h vr/body/vr_hand_mesh.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native -Ivr/body vr/native/test_ns_hand.c $(HANDSRC) -lm -lpthread -o $@
test-hand: $(B)/test_ns_hand
	$(B)/test_ns_hand
test-hand-san:
	$(MAKE) -f vr/native/extract.mk test-hand B=vr/native/build-extract-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: test-hand test-hand-san

# Red de seguridad de vértices (ns_gpu_net_*, réplica de NativeShaders.metal), soldaduras encadenadas, manos propias e interpolación: pruebas sintéticas.
#   make -f vr/native/extract.mk test-net
NETSRC := vr/native/ns_gpu.c vr/native/ns_adapt.c vr/native/ns_extract.c vr/native/ns_light.c vr/native/ns_bridge.c vr/body/vr_hand_mesh.c
$(B)/test_ns_net: vr/native/test_ns_net.c $(NETSRC) vr/native/ns_gpu.h vr/native/ns_gpu_types.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Ivr/native -Ivr/body vr/native/test_ns_net.c $(NETSRC) -lm -lpthread -o $@
test-net: $(B)/test_ns_net
	$(B)/test_ns_net
test-net-san:
	$(MAKE) -f vr/native/extract.mk test-net B=vr/native/build-extract-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: test-net test-net-san
