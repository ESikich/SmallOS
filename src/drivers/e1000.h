#ifndef E1000_H
#define E1000_H

#include "../kernel/types.h"
typedef unsigned short u16;

int e1000_init(void);
void e1000_print_info(void);
int e1000_link_up(void);
const u8* e1000_mac(void);
int e1000_send(const void* data, u32 len);

#endif /* E1000_H */
