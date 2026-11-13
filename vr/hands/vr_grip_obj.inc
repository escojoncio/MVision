/* Colocación del objeto del JUEGO en la mano para las granadas (ids 2 frag, 7 aturdidora, 8 señuelo): marco local del modelo del juego (piezas del actor, unidades del motor = mm) -> marco de la mano
 * (mm, el de vr_hand_skin), mano DERECHA canónica. Es EXACTAMENTE la relación con la que gripfit_gen.c ajustó las poses HP_W_FRAG/STUN/CHAFF_R y test_gripfit.c las comprueba: la palma sin mover (t = r = 0)
 * con Hm = [-1 0 0; 0 0 1; 0 1 0] y dR = (-47.7, 54.8, 15.4) mm de Snake contra la malla wload en marco HELD girado 180 grados sobre Z, p_obj = Hm * (p_mano + dR) * 0.001. Con
 * HELD = (x, -z, y) del motor y ese giro, Hm * giro * HELD = identidad: el modelo va SIN giro respecto a la mano y su origen queda en -dR de ella: e_motor = p_mano + dR  =>  p_mano = e_motor + (47.7, -54.8, -15.4).
 * Lo usa ns_wphide_carry_off (vrhost.c) en vez de la relación mano-objeto que da el juego, para que el objeto quede donde se diseñó la pose (dedos, pulgar y palma en contacto; test_gripfit.c). */
#define OBJ_SNAKE {1, 0, 0, 0, 1, 0, 0, 0, 1}, {47.7f, -54.8f, -15.4f}
