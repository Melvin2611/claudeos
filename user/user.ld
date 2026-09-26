ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 0x400000;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : { *(COMMON) *(.bss .bss.*) }
    _end = .;
    .note.claudeos 0 : { KEEP(*(.note.claudeos)) }
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}
