#ifndef GDT_H
#define GDT_H

/*
 * GDT segment selectors.
 *
 * Selector = (index << 3) | RPL
 *
 *   index 0  null descriptor
 *   index 1  kernel code   DPL=0   selector 0x08
 *   index 2  kernel data   DPL=0   selector 0x10
 *   index 3  user code     DPL=3   selector 0x18 | 3 = 0x1B
 *   index 4  user data     DPL=3   selector 0x20 | 3 = 0x23
 *   index 5  TSS           DPL=0   selector 0x28
 */
#define SEG_KERNEL_CODE  0x08
#define SEG_KERNEL_DATA  0x10
#define SEG_USER_CODE    0x1B   /* 0x18 | RPL3 */
#define SEG_USER_DATA    0x23   /* 0x20 | RPL3 */
#define SEG_TSS          0x28

void gdt_init(void);

/*
 * tss_set_kernel_stack()
 *
 * Update the TSS ESP0 field with the kernel stack pointer that should
 * be loaded on the next privilege transition into ring 0.
 *
 * This is the supported interface for changing ESP0. We no longer
 * expose a pointer to tss.esp0, because it is a field inside a packed
 * TSS structure and taking its address can trigger
 * -Waddress-of-packed-member warnings and encourages writes through an
 * unaligned pointer.
 */
void tss_set_kernel_stack(unsigned int esp0);

/*
 * tss_get_kernel_stack()
 *
 * Return the current value of the TSS ESP0 field.
 *
 * This accessor is intended for inspection and debugging. Callers
 * should use tss_set_kernel_stack() to update ESP0 rather than writing
 * through a pointer into the packed TSS structure.
 */
unsigned int tss_get_kernel_stack(void);

#endif /* GDT_H */