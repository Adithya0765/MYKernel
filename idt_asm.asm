; idt_asm.asm - Assembly functions for IDT (64-bit)
bits 64

global idt_flush

idt_flush:
    lidt [rdi]          ; Load IDT (first parameter in rdi for x64 calling convention)
    ret