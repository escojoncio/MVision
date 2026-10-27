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
COVSRC := vr/native/ns_cover.c vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_fx.c vr/native/ns_fxcmp.c vr/native/ns_fx_export.c vr/native/ns_gpu.c
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

# Pantalla 3D de las cinemáticas reproducida fuera de la app (ns_cinema_ref + herramienta nscine sobre volcados; ver tools/nscine.c).
#   make -f vr/native/extract.mk nscine       compila vr/native/build-extract/nscine
CINSRC := vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_cpuref.c vr/native/ns_cinema.c vr/native/ns_cinema_ref.c vr/native/ns_gpu.c vr/native/ns_fx.c vr/native/ns_fxcmp.c vr/native/ns_fx_export.c vr/native/ns_screen.c
$(B)/nscine: vr/native/tools/nscine.c vr/native/ns_dump.c $(CINSRC) vr/native/ns_cinema_ref.h vr/native/ns_cinema.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/tools/nscine.c vr/native/ns_dump.c $(CINSRC) -lm -lpthread -o $@
nscine: $(B)/nscine
.PHONY: nscine

# Regresión sintética de la pantalla 3D de las cinemáticas (orientación y descarte por detrás): make -f vr/native/extract.mk test-cine
$(B)/test_ns_cine: vr/native/test_ns_cine.c vr/native/ns_cinema.c vr/native/ns_cinema_ref.c vr/native/ns_cpuref.c vr/native/ns_gpu.c vr/native/ns_cinema_ref.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/test_ns_cine.c vr/native/ns_cinema.c vr/native/ns_cinema_ref.c vr/native/ns_cpuref.c vr/native/ns_gpu.c -lm -lpthread -o $@
test-cine: $(B)/test_ns_cine
	$(B)/test_ns_cine
.PHONY: test-cine

# Relleno por la verdad del motor (ns_fill): pruebas sintéticas y herramienta de volcados.
#   make -f vr/native/extract.mk test-fill     pruebas sintéticas de ns_fill
#   make -f vr/native/extract.mk nsfill        compila vr/native/build-extract/nsfill (ver tools/nsfill.c)
FILLSRC := vr/native/ns_fill.c vr/native/ns_fill_run.c vr/native/ns_cover.c vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_fx.c vr/native/ns_fxcmp.c vr/native/ns_fx_export.c vr/native/ns_gpu.c vr/native/ns_player.c
$(B)/test_ns_fill: vr/native/test_ns_fill.c vr/native/ns_fill_glue.c $(FILLSRC) vr/native/ns_fill.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -DNS_FILL_GLUE_TEST -Iruntime -Ivr/native vr/native/test_ns_fill.c vr/native/ns_fill_glue.c $(FILLSRC) -lm -lpthread -o $@
$(B)/nsfill: vr/native/tools/nsfill.c vr/native/ns_dump.c $(FILLSRC) vr/native/ns_fill.h $(HDR) | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/tools/nsfill.c vr/native/ns_dump.c $(FILLSRC) -lm -lpthread -o $@
nsfill: $(B)/nsfill
test-fill: $(B)/test_ns_fill
	$(B)/test_ns_fill
test-fill-san:
	$(MAKE) -f vr/native/extract.mk test-fill B=vr/native/build-extract-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
.PHONY: nsfill test-fill test-fill-san

# Banco sin conexión del enganche (envoltura / respaldo, jugador oculto por el motor) sobre volcados reales NSD1. Los volcados llevan datos del juego: no se suben al repositorio.
#   make -f vr/native/extract.mk test-dumps DUMPS=<carpeta con volcados>     (sin DUMPS se salta)
DUMPS ?=
DUMPSRC := vr/native/ns_extract_glue.c vr/native/ns_hook.c vr/native/ns_bridge.c vr/native/ns_player.c vr/native/ns_dump.c vr/native/ns_extract.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_cpuref.c vr/native/ns_fx.c vr/native/ns_fxcmp.c vr/native/ns_fx_export.c vr/native/ns_gpu.c
$(B)/test_ns_dumps: vr/native/test_ns_dumps.c $(DUMPSRC) $(HDR) vr/native/ns_hook.h vr/native/ns_dump.h | $(B)
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/test_ns_dumps.c $(DUMPSRC) -lm -lpthread -o $@
test-dumps: $(B)/test_ns_dumps
	$(B)/test_ns_dumps $(DUMPS)
.PHONY: test-dumps
