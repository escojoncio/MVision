/* Prueba de la selección de captura del volcado (vrdump.h): pedido en modo gpu (sin punto de enganche) y en nativo. make -C vr/host test */
#include <stdio.h>
#include <string.h>
#include "vrdump.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

int main(void) {
    /* pedido: la pareja en gpu no espera un punto de enganche que no existe */
    CHECK(dump_need_init(0, 1) == DN_FRAME, "gpu + pareja: solo el final de fotograma");
    CHECK(dump_need_init(0, 0) == DN_ANY, "gpu simple: la primera que llegue");
    CHECK(dump_need_init(1, 1) == (DN_STAGE | DN_FRAME), "nativo + pareja: enganche y final");
    CHECK(dump_need_init(1, 0) == DN_ANY, "nativo simple");
    /* captura con lista (gancho de fotograma, dump_point = 0) en gpu: debe tomarla, no devolver 0 */
    CHECK(dump_pick_bit(dump_need_init(0, 1), 0) == DN_FRAME, "gpu pareja: la llamada con lista captura el final");
    CHECK(dump_pick_bit(dump_need_init(0, 1), 1) == 0, "gpu pareja: el punto de enganche no captura");
    CHECK(dump_pick_bit(dump_need_init(0, 0), 0) == DN_ANY, "gpu simple con lista");
    /* nativo: comportamiento previo */
    CHECK(dump_pick_bit(DN_STAGE | DN_FRAME, 0) == 0, "nativo pareja: el final espera al enganche");
    CHECK(dump_pick_bit(DN_STAGE | DN_FRAME, 1) == DN_STAGE, "nativo pareja: el enganche primero");
    CHECK(dump_pick_bit(DN_FRAME, 0) == DN_FRAME, "enganche ya hecho: el final");
    CHECK(dump_pick_bit(DN_FRAME, 1) == 0, "enganche ya hecho: nada en el punto");
    CHECK(dump_pick_bit(DN_ANY, 1) == DN_ANY && dump_pick_bit(DN_ANY, 0) == DN_ANY, "simple: cualquiera");
    CHECK(dump_pick_bit(0, 0) == 0 && dump_pick_bit(0, 1) == 0, "sin pedido");
    /* autovolcado: decisión pura */
    {
        AutoDumpState st = {0, -1.0, {0}}; AutoDumpIn in; memset(&in, 0, sizeof in); in.world = 1;
        CHECK(autodump_decide(&st, &in, 100.0) == AD_NONE, "lista normal: nada");
        in.n_hw_cull = AD_T_HWCULL - 1; in.drop_incoh = AD_T_INCOH - 1; in.quad_noncoh = AD_T_QUAD - 1; in.long_edge = AD_T_LONG - 1;
        CHECK(autodump_decide(&st, &in, 100.0) == AD_NONE, "justo bajo todos los umbrales: nada");
        in.n_hw_cull = AD_T_HWCULL; CHECK(autodump_decide(&st, &in, 100.0) == AD_HWCULL, "hw_cull");
        in.n_hw_cull = 0; in.drop_incoh = AD_T_INCOH; CHECK(autodump_decide(&st, &in, 100.0) == AD_INCOH, "incoherentes");
        in.drop_incoh = 0; in.quad_noncoh = AD_T_QUAD; CHECK(autodump_decide(&st, &in, 100.0) == AD_QUAD, "quads");
        in.quad_noncoh = 0; in.long_edge = AD_T_LONG; CHECK(autodump_decide(&st, &in, 100.0) == AD_LONG, "aristas");
        in.long_edge = 0; in.near_cover = 0.61f; CHECK(autodump_decide(&st, &in, 100.0) == AD_NEAR_COVER, "vista tapada");
        in.near_cover = 0.60f; CHECK(autodump_decide(&st, &in, 100.0) == AD_NONE, "60 % exacto no basta");
        in.near_cover = 0.9f; in.world = 0; CHECK(autodump_decide(&st, &in, 100.0) == AD_NONE, "tapada fuera de la vista mundo: nada");
        in.world = 1; in.near_cover = 0; in.black_cover = 1; CHECK(autodump_decide(&st, &in, 100.0) == AD_BLACK_COVER, "negro en mundo");
        in.world = 0; CHECK(autodump_decide(&st, &in, 100.0) == AD_NONE, "negro fuera del mundo: nada");
        in.world = 1; in.near_cover = 0.8f; CHECK(autodump_decide(&st, &in, 100.0) == AD_NEAR_COVER, "tapada gana al negro");
        /* separación mínima 5 s y máximo 8 por sesión */
        st.n = 1; st.last_s = 100.0;
        CHECK(autodump_decide(&st, &in, 102.0) == AD_NONE && autodump_decide(&st, &in, 104.99) == AD_NONE, "menos de 5 s: nada");
        CHECK(autodump_decide(&st, &in, 105.0) == AD_NEAR_COVER, "5 s: permitido");
        st.n = AD_MAX_N; CHECK(autodump_decide(&st, &in, 1e6) == AD_NONE, "tope por sesión");
        st.n = AD_MAX_N - 1; CHECK(autodump_decide(&st, &in, 1e6) == AD_NEAR_COVER, "el octavo se permite");
        /* la separación de 5 s rige también con n == 0 (un intento fallido, p. ej. ocupado, también cuenta) */
        st.n = 0; st.last_s = 100.0; memset(st.by, 0, sizeof st.by);
        CHECK(autodump_decide(&st, &in, 102.0) == AD_NONE, "n == 0 con intento reciente: respeta los 5 s");
        CHECK(autodump_decide(&st, &in, 105.0) == AD_NEAR_COVER, "n == 0, pasados 5 s: permitido");
        /* máximo AD_MAX_PER_WHY por motivo: el motivo agotado cede el turno al siguiente */
        st.last_s = -1.0; in.black_cover = 0; st.by[AD_NEAR_COVER] = AD_MAX_PER_WHY - 1;
        CHECK(autodump_decide(&st, &in, 1e6) == AD_NEAR_COVER, "el último de su motivo se permite");
        st.by[AD_NEAR_COVER] = AD_MAX_PER_WHY;
        CHECK(autodump_decide(&st, &in, 1e6) == AD_NONE, "motivo agotado: nada");
        in.n_hw_cull = AD_T_HWCULL;
        CHECK(autodump_decide(&st, &in, 1e6) == AD_HWCULL, "motivo agotado: pasa al siguiente");
        st.by[AD_HWCULL] = AD_MAX_PER_WHY;
        CHECK(autodump_decide(&st, &in, 1e6) == AD_NONE, "dos motivos agotados: nada");
        CHECK(!strcmp(autodump_name(AD_HWCULL), "hw_cull") && !strcmp(autodump_name(99), "ninguno"), "nombres");
    }
    fprintf(stderr, fails ? "test_dump_pick: %d FALLOS\n" : "test_dump_pick: OK\n", fails);
    return fails ? 1 : 0;
}
