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
