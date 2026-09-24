/* ClaudeOS kernel linker script: 32-bit bootstrap at 1 MiB, kernel in the higher half. */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;
    _kernel_phys_start = .;

    .boot : {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
    }

    .boot.bss (NOLOAD) : ALIGN(4096) {
        *(.boot.bss)
    }

    . = ALIGN(4096);
    . += KERNEL_VMA;

    _kernel_start = .;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA) {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    }

    . = ALIGN(4096);
    _kernel_end = .;
    _kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
