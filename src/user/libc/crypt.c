#include "crypt.h"

char* crypt(const char* key, const char* salt) {
    (void)salt;
    return (char*)key;
}
