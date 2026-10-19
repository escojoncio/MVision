#pragma once
#include "cpu.h"
uint32_t fiber_open(Cpu *c, uint32_t pc, uint32_t sp, uint32_t gp);
uint32_t fiber_close(Cpu *c, uint32_t h);
uint32_t fiber_change(Cpu *c, uint32_t h);
uint32_t fiber_current(void);

/* Kernel de hilos: PCB en *0x108 y tabla de TCB en *0x110 (layout del kernel de BIOS), reservados en
 * memoria de kernel. sys_conf reconfigura el número de TCB. */
void fiber_kinit(Cpu *c, uint32_t ntcb);
/* Tras una excepción: si el PCB apunta a otro TCB que el del hilo en ejecución, conmuta a él dejando el
 * actual suspendido con los registros de *c y los bits de SR saved_sr; si no, restaura SR. */
void fiber_return_from_exception(Cpu *c, uint32_t saved_sr);
/* Entrada de excepción: refleja el estado del hilo interrumpido en su TCB (registros, EPC, SR, CAUSE). */
void fiber_exception_enter(Cpu *c);
