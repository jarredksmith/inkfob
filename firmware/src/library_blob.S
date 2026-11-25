/* Embeds data/library.bin (built by tools/build_library.py) in the firmware. */
    .section .rodata.pager_library, "a"
    .balign 4
    .global pager_library_start
pager_library_start:
    .incbin "data/library.bin"
    .global pager_library_end
pager_library_end:
    .byte 0
