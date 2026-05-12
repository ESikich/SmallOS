ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R + X */
    data PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = 0x1000;

    .text : ALIGN(4096)
    {
        *(.text)
    } :text

    .rodata : ALIGN(4096)
    {
        *(.rodata)
    } :text

    .data : ALIGN(4096)
    {
        *(.data)
    } :data

    . = 0x100000;

    .bss (NOLOAD) : ALIGN(4096)
    {
        bss_start = .;
        *(.bss)
        *(COMMON)
        bss_end = .;
    } :data
}
