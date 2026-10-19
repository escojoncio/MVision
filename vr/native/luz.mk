# Pruebas del modelo de iluminación nativo. Uso (desde la raíz del repo):
#   make -f vr/native/luz.mk test         pruebas de ns_light contra la referencia + herramienta de volcado con RAM sintética
#   make -f vr/native/luz.mk test-san     lo mismo con ASan/UBSan (UB = fallo)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= vr/native/build-luz
GL := NS_DIR_LUZ=0x80100000 NS_AMBIENTE=0x80100100 NS_LUCES_FIJAS=0x80100200
$(B):
	mkdir -p $(B)
$(B)/gte.o: runtime/gte.c | $(B)
	$(CC) -O1 -w -std=gnu11 -Iruntime -c $< -o $@
$(B)/test_ns_light: vr/native/test_ns_light.c vr/native/ns_light.c vr/native/ns_light.h $(B)/gte.o
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/test_ns_light.c vr/native/ns_light.c $(B)/gte.o -lm -o $@
$(B)/ns_lightcheck: vr/native/ns_lightcheck.c vr/native/ns_light.c vr/native/ns_light.h | $(B)
	$(CC) $(CFLAGS) -Ivr/native vr/native/ns_lightcheck.c vr/native/ns_light.c -lm -o $@
$(B)/test_ns_lightcheck_gen: vr/native/test_ns_lightcheck_gen.c vr/native/ns_light.c vr/native/ns_light.h | $(B)
	$(CC) $(CFLAGS) -Ivr/native vr/native/test_ns_lightcheck_gen.c vr/native/ns_light.c -lm -o $@
test: $(B)/test_ns_light $(B)/ns_lightcheck $(B)/test_ns_lightcheck_gen
	$(B)/test_ns_light
	$(B)/test_ns_lightcheck_gen $(B)/ok.bin $(B)/borde.bin
	$(GL) $(B)/ns_lightcheck $(B)/ok.bin | tee $(B)/ok.txt
	grep -q "dinamicos.*: 4 vertices, exactos 4 (100.00%)" $(B)/ok.txt
	grep -q "mapa.*: 4 vertices, exactos 4 (100.00%)" $(B)/ok.txt
	$(GL) $(B)/ns_lightcheck $(B)/borde.bin > /dev/null; test $$? -le 1
	env -u NS_DIR_LUZ $(B)/ns_lightcheck $(B)/ok.bin 2> /dev/null; test $$? -eq 2
	NS_DIR_LUZ=zz NS_AMBIENTE=0x80100100 NS_LUCES_FIJAS=0x80100200 $(B)/ns_lightcheck $(B)/ok.bin 2> /dev/null; test $$? -eq 2
test-san:
	$(MAKE) -f vr/native/luz.mk test B=vr/native/build-luz-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clean:
	rm -rf vr/native/build-luz vr/native/build-luz-san
.PHONY: test test-san clean
