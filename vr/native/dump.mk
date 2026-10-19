# Volcados de escena nativa (cargador + escritor + contrato). Desde la raíz: make -f vr/native/dump.mk test | test-san
CC ?= gcc
B ?= vr/native/build
CF = -O2 -g -Wall -Wextra -std=gnu11 -Iruntime -Ivr/native
SRC = vr/native/test_ns_dump.c vr/native/ns_dump.c vr/native/ns_dumpw.c
$(B):
	mkdir -p $(B)
$(B)/test_ns_dump: $(SRC) vr/native/ns_types.h vr/native/ns_pub.h | $(B)
	$(CC) $(CF) $(SRC) -o $@
$(B)/test_ns_dump_san: $(SRC) vr/native/ns_types.h vr/native/ns_pub.h | $(B)
	$(CC) $(CF) -fsanitize=address,undefined -fno-omit-frame-pointer $(SRC) -o $@
test: $(B)/test_ns_dump
	$(B)/test_ns_dump
test-san: $(B)/test_ns_dump_san
	$(B)/test_ns_dump_san
.PHONY: test test-san
