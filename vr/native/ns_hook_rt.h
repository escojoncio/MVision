#pragma once
/* ns_hook_rt: enganche de ns_capture a la etapa «pantalla» del motor con el parcheo en ejecución del runtime (rtpatch.h). Solo intérprete (MV_NOGEN). */
#include <stdint.h>
#include "cpu.h"

#define NS_STAGE_ADDR 0x8001D324u      /* etapa «pantalla» (docs/escena_nativa.md §10) */
#define NS_STAGE_TABLE 0x8009E534u     /* tabla de las siete etapas; la entrada 0 apunta a NS_STAGE_ADDR */
#define NS_MAIN_CHANNEL 0x800B2B7Cu    /* a0 de la etapa para el canal principal; los demás canales no se capturan */

enum { NS_HOOK_OK = 0, NS_HOOK_BAD_CODE = -1, NS_HOOK_BAD_TABLE = -2, NS_HOOK_PATCH_FAIL = -3 };

/* Se llama tras la captura (misma llamada de la etapa, hilo de emulación) con la CPU: lo usa el volcado en el punto de enganche. NULL = nada. */
typedef void (*NsStageCb)(Cpu *c);
void ns_hook_set_stage_cb(NsStageCb cb);

/* Valida la forma de la etapa (prólogo de pila y entrada de la tabla) y la sustituye por la envoltura. Devuelve NS_HOOK_*. Idempotente. */
int ns_hook_install(void);
void ns_hook_uninstall(void);
int ns_hook_installed(void);

/* Para llamar cada cierto tiempo (p. ej. cada medio segundo) desde el hilo de emulación: reintenta una instalación fallida y reinstala si el parche quedó caducado
 * (otra overlay sobre la etapa). Devuelve 1 si queda instalado. */
int ns_hook_poll(void);
unsigned ns_hook_stale_count(void);   /* veces que se retiró un parche caducado */
int ns_hook_last_rc(void);            /* último NS_HOOK_* de ns_hook_install (para registrar solo los cambios) */
