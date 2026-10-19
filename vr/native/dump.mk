# Volcados de escena nativa (cargador + escritor). Desde la raíz: make -f vr/native/dump.mk test
CC ?= gcc
B ?= vr/native/build
CF = -O2 -g -Wall -Wextra -std=gnu11 -Iruntime -Ivr/native
$(B):
	mkdir -p $(B)
$(B)/test_ns_dump: vr/native/test_ns_dump.c vr/native/ns_dump.c vr/native/ns_dumpw.c | $(B)
	$(CC) $(CF) $^ -o $@
test: $(B)/test_ns_dump
	$(B)/test_ns_dump
.PHONY: test
