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

struct node {
    struct wrapper item;
    struct node* next;
};

struct bundle {
    struct wrapper items[2];
    struct node chain[2];
};

struct matrixbox {
    int cells[2][2];
    struct bundle payload;
};

static struct pair make_pair(int x, int y) {
    struct pair p;
    p.x = x;
    p.y = y;
    return p;
}

static struct pair combine_pairs(struct pair a, struct pair b) {
    struct pair p;
    p.x = a.x + b.x;
    p.y = a.y + b.y;
    return p;
}

static int pair_score(struct pair* p) {
    return p->x + p->y;
}

static int pair_chain_score(struct pair* p, int len) {
    int sum = 0;
    int i = 0;
    while (i < len) {
        sum += (p + i)->x + (p + i)->y;
        i++;
    }
    return sum;
}

static int add(int a, int b) {
    return a + b;
}

static int mul(int a, int b) {
    return a * b;
}

static int wrap_score(const struct wrapper* w) {
    return w->fn(w->primary.x + w->aux.left, w->primary.y + w->aux.right);
}

static int wrap_total(const struct wrapper* ws, int len) {
    int sum = 0;
    int i = 0;
    while (i < len) {
        sum += wrap_score(&ws[i]);
        i++;
    }
    return sum;
}

static int list_total(const struct node* n) {
    int sum = 0;
    while (n) {
        sum += wrap_score(&n->item);
        n = n->next;
    }
    return sum;
}

static int bundle_total(const struct bundle* b) {
    return wrap_total(b->items, 2) + list_total(b->chain);
}

static int matrixbox_score(struct matrixbox box) {
    return box.cells[0][0] + box.cells[0][1] + box.cells[1][0] + box.cells[1][1] + bundle_total(&box.payload);
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
    struct pair pairs[2];
    struct pair returned;
    struct wrapper wrappers[2];
    struct node nodes[3];
    struct bundle bundle;
    struct matrixbox matrix;
    int struct_total;
    int pair_total;
    int nested_total;
    int list_total_value;
    int bundle_total_value;
    int matrix_total;
    int total;
    char buf[128];
    char* p = buf;

    pairs[0].x = 2;
    pairs[0].y = 3;
    pairs[1].x = 5;
    pairs[1].y = 7;
    returned = combine_pairs(make_pair(8, 9), pairs[0]);
    wrappers[0].primary = make_pair(2, 3);
    wrappers[0].aux.left = 4;
    wrappers[0].aux.right = 5;
    wrappers[0].fn = add;
    wrappers[1].primary = make_pair(1, 6);
    wrappers[1].aux.left = 2;
    wrappers[1].aux.right = 1;
    wrappers[1].fn = mul;
    nodes[0].item.primary = make_pair(1, 2);
    nodes[0].item.aux.left = 3;
    nodes[0].item.aux.right = 4;
    nodes[0].item.fn = add;
    nodes[0].next = &nodes[1];
    nodes[1].item.primary = make_pair(2, 2);
    nodes[1].item.aux.left = 1;
    nodes[1].item.aux.right = 1;
    nodes[1].item.fn = mul;
    nodes[1].next = &nodes[2];
    nodes[2].item.primary = make_pair(0, 5);
    nodes[2].item.aux.left = 5;
    nodes[2].item.aux.right = 0;
    nodes[2].item.fn = add;
    nodes[2].next = 0;
    bundle.items[0].primary = make_pair(3, 1);
    bundle.items[0].aux.left = 2;
    bundle.items[0].aux.right = 2;
    bundle.items[0].fn = add;
    bundle.items[1].primary = make_pair(2, 4);
    bundle.items[1].aux.left = 1;
    bundle.items[1].aux.right = 3;
    bundle.items[1].fn = mul;
    bundle.chain[0].item.primary = make_pair(1, 1);
    bundle.chain[0].item.aux.left = 1;
    bundle.chain[0].item.aux.right = 2;
    bundle.chain[0].item.fn = add;
    bundle.chain[0].next = &bundle.chain[1];
    bundle.chain[1].item.primary = make_pair(2, 1);
    bundle.chain[1].item.aux.left = 0;
    bundle.chain[1].item.aux.right = 5;
    bundle.chain[1].item.fn = mul;
    bundle.chain[1].next = 0;
    matrix.cells[0][0] = 1;
    matrix.cells[0][1] = 2;
    matrix.cells[1][0] = 3;
    matrix.cells[1][1] = 4;
    matrix.payload = bundle;

    struct_total = pair_score(&pairs[0]) + pair_chain_score(pairs, 2);
    pair_total = pair_score(&returned);
    nested_total = wrap_total(wrappers, 2);
    list_total_value = list_total(nodes);
    bundle_total_value = bundle_total(&bundle);
    matrix_total = matrixbox_score(matrix);
    total = struct_total + pair_total + nested_total + list_total_value + bundle_total_value + matrix_total;

    p = append_str(p, "tcc agg ok: struct=");
    p = append_dec(p, struct_total);
    p = append_str(p, " pairret=");
    p = append_dec(p, pair_total);
    p = append_str(p, " nested=");
    p = append_dec(p, nested_total);
    p = append_str(p, " list=");
    p = append_dec(p, list_total_value);
    p = append_str(p, " bundle=");
    p = append_dec(p, bundle_total_value);
    p = append_str(p, " matrix=");
    p = append_dec(p, matrix_total);
    p = append_str(p, " total=");
    p = append_dec(p, total);
    p = append_str(p, "\n");

    sys_write(buf, (int)(p - buf));
    sys_exit(0);
}
