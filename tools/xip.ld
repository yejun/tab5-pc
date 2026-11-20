/* The emulator's code and constants run from PSRAM, not flash: when the caches don't have a line, PSRAM delivers it
   in about 160 CPU cycles, the flash in about 690 (measured on the Tab5). The section is linked for a fixed address in
   PSRAM's address range that nothing else uses; tools/xip_patch.py moves its bytes into t386_xip_store (flash) after
   the link, and the firmware copies them into PSRAM and maps that at the address when it starts (xip.h).
   tools/xip_patch.py ld puts this output section into the core's sections.ld, before .flash.text. */
  .t386_xip 0x4B000000 :
  {
    _t386_xip_start = ABSOLUTE(.);
    *t386_i386.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_fpu.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_vga.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_ide.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_pc.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_i8042.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_i8254.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_i8257.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_i8259.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_sb16.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_ne2000.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_misc.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *t386_pci.c.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *tab5_386.ino.cpp.o(.text.t386_* .rodata.t386_*)   /* the VGA fast paths and the like, built with the sketch */
    . = ALIGN(16);
    _t386_xip_end = ABSOLUTE(.);
  }
