# Pruebas del modelo de iluminación nativo. Uso (desde la raíz del repo):  make -f vr/native/luz.mk test
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= vr/native/build-luz
$(B):
	mkdir -p $(B)
$(B)/gte.o: runtime/gte.c | $(B)
	$(CC) -O1 -w -std=gnu11 -Iruntime -c $< -o $@
$(B)/test_ns_light: vr/native/test_ns_light.c vr/native/ns_light.c vr/native/ns_light.h $(B)/gte.o
	$(CC) $(CFLAGS) -Iruntime -Ivr/native vr/native/test_ns_light.c vr/native/ns_light.c $(B)/gte.o -lm -o $@
test: $(B)/test_ns_light
	$(B)/test_ns_light
test-san:
	$(MAKE) -f vr/native/luz.mk test B=vr/native/build-luz-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer"
clean:
	rm -rf vr/native/build-luz vr/native/build-luz-san
.PHONY: test test-san clean
