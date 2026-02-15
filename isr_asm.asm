; isr_asm.asm - ISR stubs (64-bit)
bits 64

extern isr_handler

; Macro to create ISR stub without error code
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli
    push qword 0         ; Dummy error code
    push qword %1        ; Interrupt number
    jmp isr_common_stub
%endmacro

; Macro to create ISR stub with error code
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli
    push qword %1        ; Interrupt number
    jmp isr_common_stub
%endmacro

; CPU exception ISRs
ISR_NOERRCODE 0   ; Division by zero
ISR_NOERRCODE 1   ; Debug
ISR_NOERRCODE 2   ; Non-maskable interrupt
ISR_NOERRCODE 3   ; Breakpoint
ISR_NOERRCODE 4   ; Overflow
ISR_NOERRCODE 5   ; Bound range exceeded
ISR_NOERRCODE 6   ; Invalid opcode
ISR_NOERRCODE 7   ; Device not available
ISR_ERRCODE   8   ; Double fault
ISR_NOERRCODE 9   ; Coprocessor segment overrun
ISR_ERRCODE   10  ; Invalid TSS
ISR_ERRCODE   11  ; Segment not present
ISR_ERRCODE   12  ; Stack-segment fault
ISR_ERRCODE   13  ; General protection fault
ISR_ERRCODE   14  ; Page fault
ISR_NOERRCODE 15  ; Reserved
ISR_NOERRCODE 16  ; x87 floating-point exception
ISR_ERRCODE   17  ; Alignment check
ISR_NOERRCODE 18  ; Machine check
ISR_NOERRCODE 19  ; SIMD floating-point exception
ISR_NOERRCODE 20  ; Virtualization exception
ISR_NOERRCODE 21  ; Reserved
ISR_NOERRCODE 22  ; Reserved
ISR_NOERRCODE 23  ; Reserved
ISR_NOERRCODE 24  ; Reserved
ISR_NOERRCODE 25  ; Reserved
ISR_NOERRCODE 26  ; Reserved
ISR_NOERRCODE 27  ; Reserved
ISR_NOERRCODE 28  ; Reserved
ISR_NOERRCODE 29  ; Reserved
ISR_ERRCODE   30  ; Security exception
ISR_NOERRCODE 31  ; Reserved

; Common ISR stub - saves state and calls C handler (64-bit)
isr_common_stub:
    ; Push all general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Push data segment (DS is not available in 64-bit, use a dummy value)
    mov rax, 0
    push rax
    
    ; Call C handler (registers_t* is passed in rdi)
    mov rdi, rsp
    call isr_handler
    
    ; Pop data segment dummy
    pop rax
    
    ; Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    add rsp, 16        ; Clean up error code and ISR number
    sti
    iretq              ; Return from interrupt (64-bit)