#ifndef CPU_H
#define CPU_H

void cpu_init(void);
int cpu_write_combining_enabled(void);
void cpu_write_fence(void);

#endif /* CPU_H */
