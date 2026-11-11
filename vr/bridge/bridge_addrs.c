/* Tabla documentada de direcciones del puente RAM (EXE principal, edición europea). Ver bridge_addrs.h.
 *
 * Método general (para repetirlo): las variables de la sección de datos pequeños se acceden en el EXE con
 * `lw/sw rX, desplazamiento($gp)` y las grandes con `lui rX,0x800b ; addiu/lw rX,imm(rX)`. El registro gp lo fija
 * el arranque del EXE (lui/addiu gp -> 0x800AC488). Cada dirección se halló desarrollando una función conocida
 * del juego (localizada con la tabla de equivalencias de funciones del proyecto) y leyendo qué direcciones toca y
 * en qué orden; después se validó DINÁMICAMENTE ejecutando el runtime con el guion de mando que entra en partida
 * (~fotograma 3000-4000) y comparando el valor leído con lo que ocurre en pantalla (pulsando cruceta, triángulo,
 * cruz, Start, etc.). Confianza: 3 = alta (código + comportamiento verificado), 2 = media (código + estructura
 * coherente, sin ejercitar en la ejecución de prueba), 1 = baja.
 *
 * Hallazgo de estructura: las variables escalares de un mismo módulo fuente conservan su orden relativo entre
 * regiones; así, hallado un bloque de ellas por una función de inicialización (que las pone a cero/-1 en un orden
 * fijo), el resto del bloque sale por el desplazamiento relativo (+gp). Todas las de ese bloque fueron validadas.
 */
#include "bridge_addrs.h"

#define T3 3
#define T2 2

const BridgeAddr bridge_addr_table[] = {
    {"vars_partida", BA_VARS, 192, T3, "s16[96]",
     "Función de puesta a cero de las variables del script: memset de 190 bytes que conserva dos halfwords vecinos "
     "(dificultad y configuración) -> el búfer empieza 4 bytes antes de la primera. La función de guardado de una "
     "variable (resta la base del búfer) confirma la base 0x800B5F28. En ejecución: [11]=[12]=256 (vida/máx. al "
     "empezar), [14]=[15]=-1 (sin arma ni objeto), [8..10] cambia con la cruceta, [16] pasa a 1 al agacharse y a 2 al "
     "tumbarse, [95]=primera persona. Los bucles de añadir/quitar arma y objeto del módulo de objetos usan "
     "base+0x22 (munición), +0x36 (máximos), +0x4a (objetos) y +0x7a (máx. consumibles); sus tamaños 10/24 coinciden. "
     "Valores -1 = no lo tiene; el bit 0x8000 marca 'bloqueado' (el juego lo pone con |= al retirar el equipo)."},
    {"estado_global", BA_GAME_STATUS, 4, T3, "u32 (datos pequeños, gp+232)",
     "Función de inicialización del sistema de juego: pone a cero, en este orden, varias variables (gp+232, gp+240, "
     "gp+1880...). La primera es el estado global. Se observa 0x80020 (comunicador con voz), 0x104a0000 (escena con mando "
     "retirado: menú+vida+radar anulados) y 0 en juego."},
    {"codigo_zona", BA_STAGE_CODE, 8, T3, "char[8]",
     "La función que fija la zona actual copia una cadena a esta dirección (strcpy) y guarda el id en BA_AREA_ID. "
     "Se lee el texto de 4 caracteres de la zona."},
    {"id_zona", BA_AREA_ID, 2, T3, "s16", "Misma función que codigo_zona: store halfword gp+1736. Cambia con la zona."},
    {"historial_zonas", BA_AREA_HISTORY, 16, T2, "s16[8]",
     "Funciones de copia del historial (memcpy 16 bytes desde/hacia esta tabla) y la de fijar zona desplaza la tabla 1 "
     "posición. Se observan 3 ids no nulos."},
    {"ctrl_jugador_ptr", BA_PLAYER_CTRL, 4, T3, "u32 puntero",
     "Bloque sbss de ~30 variables del módulo principal (orden: ... magazine, id, ctrl, ruido, modo alerta, ...). "
     "El puntero apunta a un bloque cuyo campo +0x30 vale siempre 0x21CA (hash del actor del jugador) y cuyos "
     "+0/+8 coinciden con la posición/rotación del jugador y con la entrada 0 de la lista de controles."},
    {"pos_jugador", BA_PLAYER_POS, 8, T3, "VEC16",
     "Función de inicialización del sistema de juego: copia 3 halfwords de las variables de partida [8..10] a "
     "gp+1816/1818/1820. En ejecución sigue a la posición del bloque de control (diferencia <= 1 unidad)."},
    {"estado_jugador", BA_PLAYER_STATUS, 4, T3, "u32 (gp+1880)",
     "Se pone a cero en la inicialización del sistema de juego; el bloque de vecinas confirma el orden. "
     "Verificado: 0x20 al agacharse (con stance=1), 0x40 al tumbarse (stance=2), 0x10 al caminar, 0x10000 al "
     "chocar contra la pared (pegado), 0xC009 con triángulo (primera persona), 0x01000000 dentro del agua."},
    {"accion_jugador", BA_PLAYER_ACTION, 4, T2, "s32 (gp+1864)",
     "Vecina por orden del bloque (tras 'cargador', 'fallos de lectura', 'vibración'). Se observan 0x10, 0x4, 0x23 "
     "según la animación; el significado de cada valor no se ha catalogado."},
    {"alerta", BA_ALERT_MODE, 4, T2, "s32 (gp+1800)",
     "Inicialización del sistema de juego (se pone a 0) + función que fija el modo de alerta (almacena el modo 0..3). "
     "No se vio alerta en la ejecución de prueba (siempre 0); lectura validada solo por rango."},
    {"nivel_alerta", BA_ALERT_LEVEL, 4, T2, "s32 (gp+1824)", "Inicialización + función de ruido que lo copia del máximo del fotograma. Sin ejercitar."},
    {"alerta_max", BA_ALERT_MAX, 4, T2, "s32 (gp+1768)", "Inicialización (se pone a cero). Sin ejercitar."},
    {"ruido", BA_NOISE_POS, 8, T2, "VEC16 + s32 potencia/duración",
     "Bloque de ruido (posición +0x00 desde gp+1792, potencia gp+1836, duración gp+1848): inicializados por la "
     "función que decrementa la duración. Sin ejercitar."},
    {"mapa_jugador", BA_PLAYER_MAP, 4, T3, "s32 (gp+1812)", "Vecina de la posición del jugador; vale 1 en la zona de prueba y coincide con el mapa de los blancos del jugador."},
    {"cargador", BA_MAGAZINE, 2, T2, "s16 (gp+1780) y capacidad s16 (gp+1844)", "Inicialización (a cero) de ambos. Sin arma en la prueba: siempre 0."},
    {"oxigeno", BA_O2, 2, T3, "s16 (gp+1852)", "La inicialización lo pone a 1024; se observa 1024."},
    {"bloqueo_equipo", BA_DISABLE_WEAPON, 4, T2, "u32 + u32 (gp+1772, gp+1840)", "Inicialización a cero. Sin ejercitar."},
    {"nivel_pausa", BA_PAUSE_LEVEL, 4, T3, "s32 (gp+1584)",
     "Se pone a cero al final de la función que inicializa las listas de actores (gp+1584). Se observa 1 con el "
     "comunicador abierto, 2 en la pantalla de pausa y 0 en juego."},
    {"estado_menu", BA_MENU_WORK + 0x2A, 1, T3, "u8 dentro del bloque del gestor de menús",
     "El actor del gestor de menús se crea sobre 0x800BE4E0; su función de actualización de radio/comunicador lee "
     "byte +0x2A (bit0 menú de objetos, bit1 armas, bit2 comunicador). Se observa 4 durante el comunicador."},
    {"n_controles", BA_N_CONTROLS, 4, T3, "s32 (gp+1724)", "Funciones de alta/baja en la lista de controles (comparan con 96 y lo incrementan)."},
    {"lista_controles", BA_CONTROL_LIST, 384, T3, "u32[96]",
     "La función de dibujo del radar toma esta lista (0x800B6860) como array de punteros a controles, entrada 0 = "
     "jugador. Las funciones de alta/baja la modifican. Offsets del control comprobados en ejecución: +0 pos, +8 rot, "
     "+0x30 hash, +0x3A atributos de radar (0xD = visible+cono+todo el mapa en soldados; el estado = bits 12-15)."},
    {"blancos", BA_TARGET_ARRAY, 64 * 72, T3, "72 B x 64",
     "La función de reserva de blanco indexa un array de 64 elementos de 72 bytes con contador en gp+1904 (BA_N_TARGETS). "
     "En ejecución: el blanco del jugador (lado 1) trae vida 256 = vida del jugador; los soldados (lado 2) traen "
     "vida 192 y su centro coincide con la posición de su control."},
    {"n_blancos", BA_N_TARGETS, 4, T3, "s32 (gp+1904)", "Ver blancos."},
    {"camara_juego", BA_CAMERA, 0x80, T3, "estructura",
     "La función que fija los límites de la cámara escribe en 0x800B89AC (+68) y 0x800B899C (+52) -> base 0x800B8968. "
     "El halfword +0x22 (primera persona) coincide con el usado por el puente de la carcasa interior (0x800B898A). "
     "En ejecución: posición/objetivo coherentes con la vista, zoom 320. "
     "Cámara de evento: la función 0x8003098C guarda una retrollamada por ranura en +0x74 (4 ranuras) y el despacho de la cámara "
     "(0x8002FD78), con el bit 0x2 de las banderas (+0x18), llama a la de la ranura 1 (+0x78) para que coloque la cámara. Solo la "
     "instalan actores de zona: el panel del ascensor (al entrar el jugador en su zona pone banderas = 2, retira el mando al jugador "
     "con el bit 0x20000000 de su estado y lo oculta; al salir lo deshace) y command.c (que a la vez pone el bit 0x10000000 del estado global: escena). "
     "En volcados: 0x2 + retrollamada solo ante el panel; 0 en juego normal, conductos y gateo; 0x10 pegado a la pared."},
    {"canal_3d", BA_VIEW_CHANNEL, 0x1E0, T3, "estructura",
     "La función que proyecta un punto con la cámara carga la matriz desde 0x800B2B8C (= canal + 0x10, mundo->vista). "
     "La matriz vista->mundo está 0x20 detrás. En ejecución: columnas de módulo ~4096 y traslación = posición de la "
     "cámara de juego en partida; en escenas difiere (cámara propia de la escena). Foco en +0x50 = 320."},
    {"listas_actores", BA_ACTOR_LISTS, 9 * 68, T3, "9 x {nodo inicio 32 B, nodo fin 32 B, pausa s16, muerte s16}",
     "La función de inicialización de listas de actores recorre 9 entradas de 68 bytes desde 0x800ADDA8 y la de "
     "volcado imprime nivel/pausa/muerte. Cada nodo: prev,next,act,die,free,nombre(char*),runtime,count. El puntero "
     "de nombre apunta a una cadena de depuración con el fichero fuente del actor; el control de un actor vive en "
     "actor+0x20 (comprobado con el jugador y los soldados)."},
    {"gestor_menu", BA_MENU_WORK, 0x220, T3, "estructura", "Ver estado_menu."},
};
const int bridge_addr_count = (int)(sizeof bridge_addr_table / sizeof bridge_addr_table[0]);
