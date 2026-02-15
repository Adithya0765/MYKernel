// isr.h - Interrupt Service Routines (64-bit)
#ifndef ISR_H
#define ISR_H

#include "stdint.h"

// Registers pushed by our ISR stub (64-bit)
typedef struct {
    uint64_t ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

// ISR handler type
typedef void (*isr_handler_t)(registers_t*);

// Initialize ISRs
void isr_init();

// Register interrupt handler
void isr_register_handler(uint8_t n, isr_handler_t handler);

// ISR declarations (0-31 are CPU exceptions)
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

#endif