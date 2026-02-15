; gdt_asm.asm - GDT and TSS assembly helpers (64-bit)
bits 64

; void gdt_flush(uint64_t gdt_ptr)
; Loads the new GDT and reloads all segment registers
global gdt_flush
gdt_flush:
    lgdt [rdi]              ; Load GDT (rdi = pointer to gdt_ptr structure)

    ; Reload data segment registers with kernel data selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS via far return (push new CS, push return address, retfq)
    ; This is the canonical way to reload CS in 64-bit mode
    pop rdi                 ; Save return address
    push qword 0x08         ; New CS = kernel code segment
    push rdi                ; Return address
    retfq                   ; Far return reloads CS and jumps to return address

; void tss_flush(uint16_t tss_selector)
; Loads the Task Register with the TSS selector
global tss_flush
tss_flush:
    ltr di                  ; Load Task Register (di = TSS selector, 0x28)
    ret
