static void sys_write(const char* s, int len) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"(1), "b"(s), "c"(len)
        : "memory"
    );
}

static void sys_exit(int code) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"(2), "b"(code)
        : "memory"
    );
}

void* memcpy(void* dst, const void* src, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned int i = 0;
    while (i < len) {
        d[i] = s[i];
        i++;
    }
    return dst;
}

void* memmove(void* dst, const void* src, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned int i;
    if (d == s || len == 0) {
        return dst;
    }
    if (d < s) {
        return memcpy(dst, src, len);
    }
    i = len;
    while (i > 0) {
        i--;
        d[i] = s[i];
    }
    return dst;
}

void* memset(void* dst, int value, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char c = (unsigned char)value;
    unsigned int i = 0;
    while (i < len) {
        d[i] = c;
        i++;
    }
    return dst;
}

typedef int (*binop_t)(int, int);

struct pair {
    int x;
    int y;
};

struct inner {
    int left;
    int right;
};

struct wrapper {
    struct pair primary;
    struct inner aux;
    binop_t fn;
};

struct tree_node {
    struct wrapper item;
    struct tree_node* left;
    struct tree_node* right;
};

static int add(int a, int b) {
    return a + b;
}

static int mul(int a, int b) {
    return a * b;
}

static int wrap_score(const struct wrapper* w) {
    return w->fn(w->primary.x + w->aux.left, w->primary.y + w->aux.right);
}

static int tree_total(const struct tree_node* n) {
    if (!n) {
        return 0;
    }
    return wrap_score(&n->item) + tree_total(n->left) + tree_total(n->right);
}

static char* append_str(char* out, const char* s) {
    while (*s) {
        *out++ = *s++;
    }
    return out;
}

static char* append_dec(char* out, int value) {
    char tmp[16];
    int pos = 0;
    unsigned int n;

    if (value < 0) {
        *out++ = '-';
        n = (unsigned int)(-value);
    } else {
        n = (unsigned int)value;
    }

    if (n == 0) {
        *out++ = '0';
        return out;
    }

    while (n > 0) {
        tmp[pos++] = (char)('0' + (n % 10u));
        n /= 10u;
    }

    while (pos > 0) {
        *out++ = tmp[--pos];
    }

    return out;
}

void _start(void) {
    struct tree_node tree[3];
    int tree_value;
    int total;
    char buf[96];
    char* p = buf;

    tree[0].item.primary.x = 2;
    tree[0].item.primary.y = 2;
    tree[0].item.aux.left = 1;
    tree[0].item.aux.right = 1;
    tree[0].item.fn = add;
    tree[0].left = &tree[1];
    tree[0].right = &tree[2];
    tree[1].item.primary.x = 1;
    tree[1].item.primary.y = 0;
    tree[1].item.aux.left = 1;
    tree[1].item.aux.right = 0;
    tree[1].item.fn = mul;
    tree[1].left = 0;
    tree[1].right = 0;
    tree[2].item.primary.x = 0;
    tree[2].item.primary.y = 1;
    tree[2].item.aux.left = 2;
    tree[2].item.aux.right = 3;
    tree[2].item.fn = add;
    tree[2].left = 0;
    tree[2].right = 0;

    tree_value = tree_total(&tree[0]);
    total = tree_value;

    p = append_str(p, "tcc tree ok: tree=");
    p = append_dec(p, tree_value);
    p = append_str(p, " total=");
    p = append_dec(p, total);
    p = append_str(p, "\n");

    sys_write(buf, (int)(p - buf));
    sys_exit(0);
}
