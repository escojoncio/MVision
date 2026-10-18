/* Registro de armas. Referencias FUERTES a las diez definiciones: con símbolos débiles el enlazador no
 * extrae los miembros de un .a (el registro quedaría vacío) y en Mach-O un débil sin definir no enlaza.
 * Todas las entradas son no nulas; test_registry lo comprueba contra libweapons.a. */
#include "weapon.h"
#include "w_rifle.h"
#include "w_sniper.h"
#include "w_guided.h"
#include "w_aa.h"
#include "w_throw.h"

extern const WeaponDef w_pist;

const WeaponDef *const w_registry[] = {
    &w_pist, &w_rifle, &w_sniper, &w_guided, &w_aa,
    &w_throw_frag, &w_throw_b, &w_throw_c, &w_placed_a, &w_placed_b,
};
const int w_registry_count = (int)(sizeof w_registry / sizeof w_registry[0]);
