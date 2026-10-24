ENTRY(loader_entry)
MEMORY { SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 2048 }
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) } > SRAM
    .data : { *(.data*) } > SRAM
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) }
    ASSERT(SIZEOF(.data) == 0, "Loader must not require initialized data")
    ASSERT(SIZEOF(.bss) == 0, "Loader must not require BSS initialization")
}
