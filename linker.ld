ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Start of kernel image in memory */
    _kernel_start = .;

    /* Multiboot header MUST be in first 8KiB and kept */
    .boot : ALIGN(4)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4)
    {
        *(.text*)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata*)
    }

    .data : ALIGN(4)
    {
        *(.data*)
    }

    /* End of loaded image (before .bss) */
    _kernel_loaded_end = .;

    .bss : ALIGN(16)
    {
        *(.bss*)
        *(COMMON)
    }

    /* End of kernel in memory (after .bss) */
    _kernel_end = .;
}