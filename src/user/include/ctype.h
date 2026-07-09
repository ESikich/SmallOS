#ifndef USER_CTYPE_WRAPPER_H
#define USER_CTYPE_WRAPPER_H

#ifdef isspace
#undef isspace
#endif
#ifdef isalpha
#undef isalpha
#endif
#ifdef isdigit
#undef isdigit
#endif
#ifdef isxdigit
#undef isxdigit
#endif
#ifdef isupper
#undef isupper
#endif
#ifdef islower
#undef islower
#endif
#ifdef isalnum
#undef isalnum
#endif
#ifdef iscntrl
#undef iscntrl
#endif
#ifdef isgraph
#undef isgraph
#endif
#ifdef isprint
#undef isprint
#endif
#ifdef ispunct
#undef ispunct
#endif
#ifdef tolower
#undef tolower
#endif
#ifdef toupper
#undef toupper
#endif

static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static inline int isalpha(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static inline int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

static inline int islower(int c) {
    return c >= 'a' && c <= 'z';
}

static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

static inline int iscntrl(int c) {
    return (c >= 0 && c < 32) || c == 127;
}

static inline int isgraph(int c) {
    return c > 32 && c < 127;
}

static inline int isprint(int c) {
    return c >= 32 && c < 127;
}

static inline int ispunct(int c) {
    return isgraph(c) && !isalnum(c);
}

static inline int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static inline int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

#endif /* USER_CTYPE_WRAPPER_H */
