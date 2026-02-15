; boot.asm - Multiboot 1 with VBE Graphics
global _start
extern kernel_main
extern _kernel_start
extern _kernel_loaded_end
extern _kernel_end

; Constants
MB_MAGIC    equ 0x1BADB002
; Align + MemInfo + Video + AOUT kludge addresses
MB_FLAGS    equ 0x00010007
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
multiboot_header:
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    
    ; AOUT Kludge (Required for ELF/BIN mix)
    dd multiboot_header
    dd _kernel_start
    dd _kernel_loaded_end
    dd _kernel_end
    dd _start

    ; Graphics Request (Mode_type, Width, Height, Depth)
    dd 0    ; 0 = Linear Graphics
    dd 1024 ; Width
    dd 768  ; Height
    dd 32   ; Depth

section .text
bits 32
_start:
    ; 1. Stack
    mov esp, stack_top

    ; 2. Save Multiboot Pointer (EBX) - store as 32-bit, will zero-extend in 64-bit mode
    mov [multiboot_pointer], ebx

    ; 3. Check CPU
    call check_cpuid
    call check_long_mode

    ; 4. Paging
    call setup_page_tables
    call enable_paging

    ; 5. GDT
    lgdt [gdt64.pointer]
    
    ; 6. Jump
    jmp gdt64.code_seg:long_mode_start

; (Copy Helper Functions: check_cpuid, check_long_mode, setup_page_tables, enable_paging)
; ... keep existing helper functions ...
; If you don't have them, I can paste them again, but they are unchanged.

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    hlt

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    hlt

setup_page_tables:
    ; Setup L4: point to L3
    mov eax, page_table_l3
    or eax, 0b11
    mov [page_table_l4], eax
    
    ; Setup L3: Use 1GB huge pages to map first 4GB
    ; Entry 0: 0-1GB
    mov eax, 0x00000000
    or eax, 0b10000011  ; Present, Writable, Huge Page (1GB)
    mov [page_table_l3], eax
    
    ; Entry 1: 1-2GB
    mov eax, 0x40000000
    or eax, 0b10000011
    mov [page_table_l3 + 8], eax
    
    ; Entry 2: 2-3GB
    mov eax, 0x80000000
    or eax, 0b10000011
    mov [page_table_l3 + 16], eax
    
    ; Entry 3: 3-4GB (covers framebuffer at 0xFD000000)
    mov eax, 0xC0000000
    or eax, 0b10000011
    mov [page_table_l3 + 24], eax
    
    ret

enable_paging:
    mov eax, page_table_l4
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

bits 64
long_mode_start:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Pass multiboot pointer in rdi (first argument in x64 calling convention)
    ; Load 32-bit value and zero-extend to 64-bit
    xor rdi, rdi
    mov edi, [multiboot_pointer]
    xor rsi, rsi  ; Second argument (magic) - unused but set to 0
    call kernel_main
    hlt

section .data
align 4
multiboot_pointer: dd 0

section .bss
align 4096
page_table_l4: resb 4096
page_table_l3: resb 4096
stack_bottom:  resb 16384
stack_top:

section .rodata
gdt64:
    dq 0
.code_seg: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64