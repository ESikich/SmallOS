#ifndef USER_DLFCN_WRAPPER_H
#define USER_DLFCN_WRAPPER_H

#define RTLD_LAZY   0x001
#define RTLD_NOW    0x002
#define RTLD_GLOBAL 0x100

void* dlopen(const char* filename, int flag);
const char* dlerror(void);
void* dlsym(void* handle, char* symbol);
int dlclose(void* handle);

#endif
