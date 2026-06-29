#ifndef USER_DLFCN_WRAPPER_H
#define USER_DLFCN_WRAPPER_H

#define RTLD_LAZY   0x001
#define RTLD_NOW    0x002
#define RTLD_LOCAL  0x000
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void*)0)

void* dlopen(const char* filename, int flag);
const char* dlerror(void);
void* dlsym(void* handle, const char* symbol);
int dlclose(void* handle);

#endif
