; switch.asm - Context switch assembly for Alteo OS
; Implements cooperative and preemptive context switching
bits 64

; ---------------------------------------------------------------------------
; void switch_context(uint64_t* old_rsp, uint64_t new_rsp)
;   RDI = pointer to save location for current RSP
;   RSI = new RSP to load (from next process's saved state)
;
; Saves callee-saved registers and RFLAGS on the current stack,
; swaps the stack pointer, then restores registers from the new stack.
; The 'ret' at the end pops the new process's saved return address,
; effectively resuming execution where that process last called
; switch_context (or at its entry point for new processes).
; ---------------------------------------------------------------------------
global switch_context
switch_context:
    ; Save callee-saved registers on the current stack
    pushfq                  ; Save RFLAGS
    push r15
    push r14
    push r13
    push r12
    push rbx
    push rbp

    ; Save current RSP to *old_rsp
    mov [rdi], rsp

    ; Load new RSP
    mov rsp, rsi

    ; Restore callee-saved registers from the new stack
    pop rbp
    pop rbx
    pop r12
    pop r13
    pop r14
    pop r15
    popfq                   ; Restore RFLAGS

    ; Return to the new process's saved return address
    ret

; ---------------------------------------------------------------------------
; Timer IRQ 0 handler with context switch support
; This is a special version of the IRQ0 handler that allows the C handler
; to switch the stack pointer for preemptive multitasking.
;
; The C function timer_irq_handler(uint64_t current_rsp) returns the
; (possibly different) RSP to use when restoring state.
; ---------------------------------------------------------------------------
extern timer_irq_handler

global irq0_switch
irq0_switch:
    cli

    ; Push dummy error code and interrupt number (matches registers_t layout)
    push qword 0           ; Dummy error code
    push qword 32          ; Interrupt number (IRQ0 = 32)

    ; Save all general-purpose registers
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

    ; Save DS (pushed as a qword to maintain alignment)
    xor rax, rax
    mov ax, ds
    push rax

    ; Call C handler: rdi = current RSP (pointer to saved register frame)
    mov rdi, rsp
    call timer_irq_handler
    ; RAX = new RSP (same if no switch, different if context switched)

    ; Switch stack if handler returned a different RSP
    mov rsp, rax

    ; Restore DS
    pop rax
    mov ds, ax
    mov es, ax

    ; Restore general-purpose registers
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

    ; Skip interrupt number and error code
    add rsp, 16

    sti
    iretq

; ---------------------------------------------------------------------------
; Syscall entry point (for SYSCALL/SYSRET mechanism)
; Called when user code executes the SYSCALL instruction.
;
; On entry:
;   RCX = user RIP (return address)
;   R11 = user RFLAGS
;   RAX = syscall number
;   RDI, RSI, RDX, R10 = syscall arguments (arg1-arg4)
;
; The SFMASK MSR clears IF in RFLAGS on SYSCALL entry.
; ---------------------------------------------------------------------------
extern syscall_dispatch

global syscall_entry
syscall_entry:
    ; We're now in Ring 0 with kernel CS/SS
    ; Swap to kernel stack (RSP0 from TSS)
    ; Save user RSP in a scratch register
    mov r15, rsp            ; Save user RSP temporarily

    ; Load kernel stack from TSS RSP0
    ; We store the kernel stack pointer in a known location
    extern kernel_syscall_stack_top
    mov rsp, [rel kernel_syscall_stack_top]

    ; Push user context for SYSRET
    push r15                ; User RSP
    push r11                ; User RFLAGS
    push rcx                ; User RIP

    ; Save callee-saved registers (in case syscall handler clobbers them)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Set up arguments for syscall_dispatch(syscall_num, arg1, arg2, arg3)
    ; RAX = syscall_num, RDI = arg1, RSI = arg2, RDX = arg3
    ; Move R10 to RCX for the C calling convention (arg4)
    mov rcx, r10            ; arg4 = r10 (SYSCALL clobbers RCX)
    ; Shift arguments: rdi=arg1, rsi=arg2, rdx=arg3, rcx=arg4 already set
    ; We need: rdi=syscall_num, rsi=arg1, rdx=arg2, rcx=arg3
    mov r10, rdx            ; Save arg3
    mov rdx, rsi            ; arg3 (for dispatch) = original arg2
    mov rsi, rdi            ; arg2 (for dispatch) = original arg1
    mov rdi, rax            ; arg1 (for dispatch) = syscall number
    mov rcx, r10            ; arg4 (for dispatch) = original arg3

    ; Enable interrupts during syscall processing
    sti

    ; Call the C syscall dispatcher
    call syscall_dispatch

    ; Disable interrupts for SYSRET
    cli

    ; Return value is in RAX (already set by syscall_dispatch)

    ; Restore callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Restore user context
    pop rcx                 ; User RIP (for SYSRET)
    pop r11                 ; User RFLAGS (for SYSRET)
    pop rsp                 ; User RSP

    ; Return to user mode
    ; SYSRET sets CS = STAR[63:48]+16, SS = STAR[63:48]+8 with RPL=3
    o64 sysret
