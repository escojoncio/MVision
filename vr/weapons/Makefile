# Armas en mano. Uso (desde la raíz del repo):
#   make -C vr/weapons lib                     compila build/libweapons.a
#   make -C vr/weapons test                    pruebas de núcleo, armas y registro (headless)
#   make -C vr/weapons test-san                lo mismo con -fsanitize=address,undefined (en build-san/)
#   make -C vr/weapons test-wload              prueba del cargador de modelos (necesita datos; sin ellos se omite)
#   make -C vr/weapons preview W=pist STATE="0.5:trig0=1;0.6:trig0=0" OBJ=ruta.obj VIEW=three OUT=/ruta/a.png
#       W=id del arma (registrada), STATE=guion --sim (ver cabecera de tools/wmodel/preview.c),
#       OBJ=modelo .obj extraído de los datos del usuario (se enlaza por nombre de hueco de mesh),
#       VIEW=side|left|top|under|front|rear|three|three2|sheet, MODE=manual|simplified|auto,
#       EXTRA="--print --radius 0.2 --ch SLIDE=0.04"
# Las armas NO llevan mallas compiladas: los modelos se cargan en ejecución y se enlazan con weapon_bind().
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
PV := ../../tools/wmodel
SRC := wcore.c registry.c wload.c $(filter-out wcore.c registry.c wload.c,$(wildcard w_*.c))
OBJS := $(SRC:%.c=$(B)/%.o)
W ?= pist
VIEW ?= side
OUT ?= $(B)/$(W).png
MODE ?= manual
STATE ?=

all: lib
lib: $(B)/libweapons.a
$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -MMD -MP -I. -c $< -o $@
$(B):
	mkdir -p $(B)
$(B)/libweapons.a: $(OBJS)
	rm -f $@ && ar rcs $@ $^
$(B)/preview: $(PV)/preview.c $(PV)/wprev.c $(PV)/wprev.h $(OBJS)
	$(CC) $(CFLAGS) -I. $(PV)/preview.c $(PV)/wprev.c $(OBJS) -lm -o $@
preview: $(B)/preview
	$(B)/preview $(W) -o $(OUT) --view $(VIEW) --mode $(MODE) $(if $(OBJ),--obj $(OBJ)) $(if $(STATE),--sim "$(STATE)") $(EXTRA)

# pruebas: cada test_<x>.c enlaza con los objetos; test_registry enlaza contra el .a (comprueba que el
# registro sobrevive al enlazado estático)
TESTS := $(filter-out $(B)/test_wload $(B)/test_registry,$(patsubst test_%.c,$(B)/test_%,$(wildcard test_*.c))) $(B)/test_registry
$(B)/test_registry: test_registry.c $(B)/libweapons.a
	$(CC) $(CFLAGS) -I. test_registry.c -L$(B) -lweapons -lm -o $@
$(B)/test_%: test_%.c $(OBJS)
	$(CC) $(CFLAGS) -I. $< $(OBJS) -lm -o $@
test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
test-wload: $(B)/test_wload
	$(B)/test_wload $(WLOAD_ARGS)
clean:
	rm -rf build build-san
.PHONY: all lib preview test test-san test-wload clean
-include $(OBJS:.o=.d)
