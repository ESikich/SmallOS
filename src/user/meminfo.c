#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_meminfo_t info;
    unsigned int used_frames;

    (void)argc;
    (void)argv;

    if (sys_meminfo(&info) < 0) {
        u_puts("meminfo: unavailable\n");
        sys_exit(1);
    }

    used_frames = info.pmm_total_frames - info.pmm_free_frames;
    u_puts("heap:   base ");
    diag_put_hex32(info.heap_base);
    u_puts("  top ");
    diag_put_hex32(info.heap_top);
    u_puts("  used ");
    u_put_uint((info.heap_top - info.heap_base) / 1024u);
    u_puts(" KB\n");

    u_puts("frames: ");
    u_put_uint(info.pmm_free_frames);
    u_puts(" free / ");
    u_put_uint(info.pmm_total_frames);
    u_puts(" total  (");
    u_put_uint(info.pmm_free_frames * 4u);
    u_puts(" KB / ");
    u_put_uint(info.pmm_total_frames * 4u);
    u_puts(" KB)\n");

    u_puts("used:   ");
    u_put_uint(used_frames);
    u_puts(" frames (");
    u_put_uint(used_frames * 4u);
    u_puts(" KB)\n");

    u_puts("pmm:    used ");
    u_put_uint(info.pmm_used_frames);
    u_puts(" ref ");
    u_put_uint(info.pmm_refcounted_frames);
    u_puts(" shared ");
    u_put_uint(info.pmm_shared_frames);
    u_puts("\n");

    u_puts("proc:   ");
    u_put_uint(info.process_count);
    u_puts(" / ");
    u_put_uint(info.process_capacity);
    u_puts(" tasks  pages p=");
    u_put_uint(info.process_pages);
    u_puts(" kstk=");
    u_put_uint(info.kernel_stack_pages);
    u_puts(" fd=");
    u_put_uint(info.fd_table_pages);
    u_puts(" vm=");
    u_put_uint(info.vm_area_pages);
    u_puts("\n");

    u_puts("kalloc: ");
    u_put_uint(info.kalloc_pages);
    u_puts(" pages  used ");
    u_put_uint(info.kalloc_used_bytes);
    u_puts(" B  free ");
    u_put_uint(info.kalloc_free_bytes);
    u_puts(" B\n");

    u_puts("rofile: ");
    u_put_uint(info.ro_file_cache_pages);
    u_puts(" cache pages / ");
    u_put_uint(info.ro_file_cache_mapped_refs);
    u_puts(" mapped refs\n");

    u_puts("e820:   ");
    if (info.e820_valid) {
        u_put_uint(info.e820_count);
        u_puts(" entries\n");
    } else {
        u_puts("unavailable\n");
    }

    sys_exit(0);
}
