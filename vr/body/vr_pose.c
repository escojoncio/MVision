/* Implementación débil de vrpose_get_hands: sin manos. runtime/host_vr.c la sustituye con la real. */
#include <string.h>
#include "vr_pose.h"

__attribute__((weak)) int vrpose_get_hands(VrHandPose out[2]) {
    memset(out, 0, 2 * sizeof *out);
    return 0;
}
