#include "cpu.h"
#include "types.h"

#define CPUID_FEATURE_MSR  (1u << 5)
#define CPUID_FEATURE_PAT  (1u << 16)
#define CPUID_FEATURE_SSE  (1u << 25)

#define IA32_PAT_MSR       0x277u
#define PAT_TYPE_WC        0x01ull
#define PAT_ENTRY1_SHIFT   8u

typedef struct {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
} cpu_regs_t;

static int s_cpu_ready;
static int s_has_sse;
static int s_wc_enabled;
static volatile u32 s_fence_word;

static int cpu_has_cpuid(void) {
    u32 before;
    u32 after;

    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl"
        : "=&r"(before), "=&r"(after)
        :
        : "cc");

    return ((before ^ after) & 0x200000u) != 0u;
}

static void cpu_cpuid(u32 leaf, u32 subleaf, cpu_regs_t* out) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(out->eax), "=b"(out->ebx), "=c"(out->ecx), "=d"(out->edx)
        : "a"(leaf), "c"(subleaf)
        : "memory");
}

static u64 cpu_rdmsr(u32 msr) {
    u32 lo;
    u32 hi;

    __asm__ __volatile__(
        "rdmsr"
        : "=a"(lo), "=d"(hi)
        : "c"(msr)
        : "memory");
    return ((u64)hi << 32) | lo;
}

static void cpu_wrmsr(u32 msr, u64 value) {
    u32 lo = (u32)value;
    u32 hi = (u32)(value >> 32);

    __asm__ __volatile__(
        "wrmsr"
        :
        : "c"(msr), "a"(lo), "d"(hi)
        : "memory");
}

static void cpu_enable_pat_write_combining(void) {
    u64 pat = cpu_rdmsr(IA32_PAT_MSR);
    u64 mask = 0xFFull << PAT_ENTRY1_SHIFT;

    pat = (pat & ~mask) | (PAT_TYPE_WC << PAT_ENTRY1_SHIFT);
    cpu_wrmsr(IA32_PAT_MSR, pat);
    s_wc_enabled = 1;
}

void cpu_init(void) {
    cpu_regs_t leaf0;
    cpu_regs_t leaf1;

    if (s_cpu_ready) return;
    s_cpu_ready = 1;

    if (!cpu_has_cpuid()) {
        return;
    }

    cpu_cpuid(0, 0, &leaf0);
    if (leaf0.eax < 1u) {
        return;
    }

    cpu_cpuid(1, 0, &leaf1);
    s_has_sse = (leaf1.edx & CPUID_FEATURE_SSE) != 0u;
    if ((leaf1.edx & (CPUID_FEATURE_MSR | CPUID_FEATURE_PAT)) ==
        (CPUID_FEATURE_MSR | CPUID_FEATURE_PAT)) {
        cpu_enable_pat_write_combining();
    }
}

int cpu_write_combining_enabled(void) {
    return s_wc_enabled;
}

void cpu_write_fence(void) {
    if (!s_wc_enabled) {
        __asm__ __volatile__("" : : : "memory");
        return;
    }

    if (s_has_sse) {
        __asm__ __volatile__("sfence" : : : "memory");
    } else {
        __asm__ __volatile__(
            "lock; addl $0, %0"
            : "+m"(s_fence_word)
            :
            : "memory", "cc");
    }
}
