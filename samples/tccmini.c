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

static int add(int a, int b) {
    return a + b;
}

static int fib(int n) {
    int a = 0;
    int b = 1;
    while (n-- > 0) {
        int next = a + b;
        a = b;
        b = next;
    }
    return a;
}

static int checksum(const int* values, int len) {
    int sum = 0;
    int i = 0;
    while (i < len) {
        sum += values[i] * (i + 1);
        i++;
    }
    return sum;
}

static int muladd(int a, int b, int c) {
    return a * b + c;
}

static int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

static int bonus_pick(int mode) {
    switch (mode) {
        case 0: return 3;
        case 1: return 5;
        case 2: return 7;
        default: return 11;
    }
}

struct pair {
    int x;
    int y;
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

typedef int (*binop_t)(int, int);

struct task {
    const char* name;
    binop_t fn;
    int lhs;
    int rhs;
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

struct tree_node {
    struct wrapper item;
    struct tree_node* left;
    struct tree_node* right;
};

static int sub(int a, int b) {
    return a - b;
}

static int mul(int a, int b) {
    return a * b;
}

static int run_tasks(const struct task* tasks, int len) {
    int sum = 0;
    int i = 0;
    while (i < len) {
        sum += tasks[i].fn(tasks[i].lhs, tasks[i].rhs);
        i++;
    }
    return sum;
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
    int values[4];
    struct pair pairs[2];
    struct pair returned;
    struct task tasks[3];
    struct wrapper wrappers[2];
    struct node nodes[3];
    struct bundle bundle;
    struct matrixbox matrix;
    struct tree_node tree[3];
    int scratch[6];
    int a = add(2, 5);
    int b = fib(6);
    int c;
    int s;
    int r;
    int nested;
    int listed;
    int bundled;
    int matrixed;
    int treed;
    int f;
    int bonus;
    int dispatch;
    int total;
    char buf[256];
    char* p = buf;

    values[0] = 3;
    values[1] = 1;
    values[2] = 4;
    values[3] = 1;
    pairs[0].x = 2;
    pairs[0].y = 3;
    pairs[1].x = 5;
    pairs[1].y = 7;
    returned = combine_pairs(make_pair(8, 9), pairs[0]);
    tasks[0].name = "add";
    tasks[0].fn = add;
    tasks[0].lhs = 2;
    tasks[0].rhs = 5;
    tasks[1].name = "sub";
    tasks[1].fn = sub;
    tasks[1].lhs = 9;
    tasks[1].rhs = 4;
    tasks[2].name = "mul";
    tasks[2].fn = mul;
    tasks[2].lhs = 3;
    tasks[2].rhs = 4;
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
    tree[0].item.primary = make_pair(2, 2);
    tree[0].item.aux.left = 1;
    tree[0].item.aux.right = 1;
    tree[0].item.fn = add;
    tree[0].left = &tree[1];
    tree[0].right = &tree[2];
    tree[1].item.primary = make_pair(1, 0);
    tree[1].item.aux.left = 1;
    tree[1].item.aux.right = 0;
    tree[1].item.fn = mul;
    tree[1].left = 0;
    tree[1].right = 0;
    tree[2].item.primary = make_pair(0, 1);
    tree[2].item.aux.left = 2;
    tree[2].item.aux.right = 3;
    tree[2].item.fn = add;
    tree[2].left = 0;
    tree[2].right = 0;
    memset(scratch, 0, sizeof(scratch));
    memcpy(&scratch[0], values, sizeof(values));
    memmove(&scratch[1], &scratch[0], sizeof(values));
    c = checksum(values, 4);
    s = pair_score(&pairs[0]) + pair_chain_score(pairs, 2);
    r = pair_score(&returned);
    nested = wrap_total(wrappers, 2);
    listed = list_total(nodes);
    bundled = bundle_total(&bundle);
    matrixed = matrixbox_score(matrix);
    treed = tree_total(&tree[0]);
    f = factorial(5);
    bonus = bonus_pick(2);
    dispatch = run_tasks(tasks, 3);
    total = muladd(a, b, c);
    total += s;
    total += r;
    total += nested;
    total += listed;
    total += bundled;
    total += matrixed;
    total += treed;
    total += f;
    total += bonus;
    total += dispatch;

    p = append_str(p, "tinycc ramp ok: add=");
    p = append_dec(p, a);
    p = append_str(p, " fib=");
    p = append_dec(p, b);
    p = append_str(p, " checksum=");
    p = append_dec(p, c);
    p = append_str(p, " scratch=");
    p = append_dec(p, scratch[3]);
    p = append_str(p, " struct=");
    p = append_dec(p, s);
    p = append_str(p, " pairret=");
    p = append_dec(p, r);
    p = append_str(p, " nested=");
    p = append_dec(p, nested);
    p = append_str(p, " list=");
    p = append_dec(p, listed);
    p = append_str(p, " bundle=");
    p = append_dec(p, bundled);
    p = append_str(p, " matrix=");
    p = append_dec(p, matrixed);
    p = append_str(p, " tree=");
    p = append_dec(p, treed);
    p = append_str(p, " fact=");
    p = append_dec(p, f);
    p = append_str(p, " bonus=");
    p = append_dec(p, bonus);
    p = append_str(p, " dispatch=");
    p = append_dec(p, dispatch);
    p = append_str(p, " total=");
    p = append_dec(p, total);
    p = append_str(p, "\n");

    sys_write(buf, (int)(p - buf));
    sys_exit(0);
}
