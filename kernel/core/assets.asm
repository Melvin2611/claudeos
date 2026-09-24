; Fonts embedded into the kernel image (window server + boot console + panic screen)
section .rodata
align 16

%macro EMBED 2
global %1_start
global %1_end
%1_start:
    incbin %2
%1_end:
align 16
%endmacro

EMBED font_ui, "assets/fonts/ui.fnt"
EMBED font_ui_bold, "assets/fonts/ui-bold.fnt"
EMBED font_ui_large, "assets/fonts/ui-large.fnt"
EMBED font_title, "assets/fonts/title.fnt"
EMBED font_display, "assets/fonts/display.fnt"
EMBED font_mono, "assets/fonts/mono.fnt"
EMBED font_mono_bold, "assets/fonts/mono-bold.fnt"
