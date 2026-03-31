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
 * tss_set_kernel_stack(esp0)
 *
 * Update the TSS ESP0 field to point at the top of the kernel stack.
 * Must be called before iret-ing into ring 3 so that the CPU knows
 * where to switch the stack on the next int 0x80.
 */
void tss_set_kernel_stack(unsigned int esp0);

/*
 * tss_get_esp0_ptr()
 *
 * Return a pointer to the tss.esp0 field.  Used by the scheduler to
 * update TSS ESP0 directly during a context switch without calling
 * tss_set_kernel_stack() (which would add a function-call overhead
 * inside the hot IRQ path).
 *
 * The returned pointer remains valid for the lifetime of the kernel.
 */
unsigned int* tss_get_esp0_ptr(void);

#endif /* GDT_H */