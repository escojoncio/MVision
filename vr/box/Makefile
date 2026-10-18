# Carcasa de visión interior. Uso (desde la raíz del repo):
#   make -C vr/box test                     pruebas headless (usa los datos reales si existen; si no, abertura genérica)
#   make -C vr/box test-san                 lo mismo con -fsanitize=address,undefined
#   make -C vr/box prev                     compila la herramienta de renders (tools/box/boxprev.c)
# Datos opcionales de las pruebas: BOX_STAGES=ruta/al/archivo_de_stages BOX_STAGE=<stage> BOX_SIGHT_ID=<id16> BOX_TEX_ID=<id16>
# (sin variables, test_box busca ../../gamedata/stages.bin y usa stage "init" con los ids por defecto de la app).
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
W := ../weapons
SRC := box_view.c box_data.c $(W)/wcore.c
OBJS := $(B)/box_view.o $(B)/box_data.o $(B)/wcore.o
PV := ../../tools/box

all: $(B)/libbox.a
$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -I. -I$(W) -MMD -MP -c $< -o $@
$(B)/wcore.o: $(W)/wcore.c | $(B)
	$(CC) $(CFLAGS) -I$(W) -MMD -MP -c $< -o $@
$(B):
	mkdir -p $(B)
$(B)/libbox.a: $(OBJS)
	rm -f $@ && ar rcs $@ $^
$(B)/test_box: test_box.c $(OBJS)
	$(CC) $(CFLAGS) -I. -I$(W) $< $(OBJS) -lm -o $@
$(B)/boxprev: $(PV)/boxprev.c $(OBJS)
	$(CC) $(CFLAGS) -I. -I$(W) $< $(OBJS) -lm -o $@
prev: $(B)/boxprev
test: $(B)/test_box
	$(B)/test_box
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
clean:
	rm -rf build build-san
.PHONY: all prev test test-san clean
-include $(OBJS:.o=.d)
