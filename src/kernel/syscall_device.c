#include "syscall_internal.h"
#include "input.h"
#include "keyboard.h"
#include "klib.h"
#include "paging.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "uapi_errno.h"
#include "../drivers/display.h"
#include "../drivers/mouse.h"
#include "../drivers/sound.h"
#include "../drivers/usb.h"

#define INPUT_READ_MAX_EVENTS 64u

int sys_display_info_impl(sys_display_info_t* out_info) {
    display_info_t info;

    if (!out_info) return -EFAULT;
    if (!user_buf_ok((unsigned int)out_info, sizeof(*out_info))) return -EFAULT;
    if (!display_get_info(&info)) return -EIO;

    {
        sys_display_info_t user_info;
        user_info.width = info.width;
        user_info.height = info.height;
        user_info.pitch = info.pitch;
        user_info.bpp = info.bpp;
        user_info.format = info.format;
        if (copy_to_user(out_info, &user_info, sizeof(user_info)) < 0) return -EFAULT;
    }
    return 0;
}

int sys_display_acquire_impl(void) {
    process_t* proc = (process_t*)sched_current();
    if (!display_acquire(proc)) {
        return -EIO;
    }
    process_set_display_input_owner(proc, 1);
    keyboard_buf_clear();
    input_clear_events();
    return 0;
}

int sys_display_release_impl(void) {
    process_t* proc = (process_t*)sched_current();
    process_set_display_input_owner(proc, 0);
    keyboard_buf_clear();
    input_clear_events();
    display_release(proc);
    return 0;
}

int sys_display_fill_impl(const sys_display_fill_rect_t* user_req) {
    sys_display_fill_rect_t req;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!display_fill((process_t*)sched_current(), req.x, req.y, req.w, req.h, req.color)) return -EIO;
    return 0;
}

int sys_display_blit_impl(const sys_display_blit_rect_t* user_req) {
    sys_display_blit_rect_t req;
    unsigned int bytes;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!req.pixels) return -EFAULT;
    if (req.h > 0xFFFFFFFFu / req.w) return -EOVERFLOW;
    if (req.w * req.h > 0xFFFFFFFFu / sizeof(unsigned int)) return -EOVERFLOW;
    bytes = req.w * req.h * sizeof(unsigned int);
    if (!user_buf_ok((unsigned int)req.pixels, bytes)) return -EFAULT;
    if (!display_blit((process_t*)sched_current(), req.x, req.y, req.w, req.h, req.pixels)) return -EIO;
    return 0;
}

int sys_display_blit_stride_impl(const sys_display_blit_stride_rect_t* user_req) {
    sys_display_blit_stride_rect_t req;
    unsigned int span_pixels;
    unsigned int bytes;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!req.pixels) return -EFAULT;
    if (req.pitch_pixels < req.w) return -EINVAL;
    if (req.h - 1u > 0xFFFFFFFFu / req.pitch_pixels) return -EOVERFLOW;
    span_pixels = (req.h - 1u) * req.pitch_pixels;
    if (span_pixels > 0xFFFFFFFFu - req.w) return -EOVERFLOW;
    span_pixels += req.w;
    if (span_pixels > 0xFFFFFFFFu / sizeof(unsigned int)) return -EOVERFLOW;
    bytes = span_pixels * sizeof(unsigned int);
    if (!user_buf_ok((unsigned int)req.pixels, bytes)) return -EFAULT;
    if (!display_blit_stride((process_t*)sched_current(),
                             req.x, req.y, req.w, req.h,
                             req.pitch_pixels, req.pixels)) {
        return -EIO;
    }
    return 0;
}

int sys_display_map_impl(sys_display_map_info_t* out_info) {
    sys_display_map_info_t info;

    if (!out_info) return -EFAULT;
    if (!user_buf_ok((unsigned int)out_info, sizeof(*out_info))) return -EFAULT;
    if (!display_map((process_t*)sched_current(), &info)) return -EIO;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

int sys_display_present_page_impl(sys_display_present_page_t* user_req) {
    sys_display_present_page_t req;

    if (!user_req) return -EFAULT;
    if (!user_buf_ok((unsigned int)user_req, sizeof(*user_req))) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (!display_present_page((process_t*)sched_current(), &req)) return -EIO;
    if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
    return 0;
}

int sys_mouse_read_impl(sys_mouse_state_t* out_state) {
    sys_mouse_state_t state;

    if (!out_state) return -EFAULT;
    if (!mouse_read_state(&state)) return -EIO;
    if (copy_to_user(out_state, &state, sizeof(state)) < 0) return -EFAULT;
    return 0;
}

int sys_mouse_debug_impl(sys_mousedebug_t* out_info) {
    mouse_debug_state_t debug;
    sys_mousedebug_t info;

    if (!out_info) return -EFAULT;
    mouse_debug_snapshot(&debug);
    info.irq_count = debug.irq_count;
    info.byte_count = debug.byte_count;
    info.aux_status_count = debug.aux_status_count;
    info.packet_count = debug.packet_count;
    info.vmware_packet_count = debug.vmware_packet_count;
    info.sync_drop_count = debug.sync_drop_count;
    info.overflow_drop_count = debug.overflow_drop_count;
    info.vmware_enabled = debug.vmware_enabled;
    info.packet_size = debug.packet_size;
    info.device_id = debug.device_id;
    info.ready = debug.ready;
    info.init_step = debug.init_step;
    info.init_fail = debug.init_fail;
    info.config_before = debug.config_before;
    info.config_after = debug.config_after;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

int sys_usbinfo_impl(sys_usbinfo_t* out_info) {
    usb_debug_state_t debug;
    sys_usbinfo_t info;

    if (!out_info) return -EFAULT;
    usb_debug_snapshot(&debug);
    info.controller_count = debug.controller_count;
    info.uhci_count = debug.uhci_count;
    info.ohci_count = debug.ohci_count;
    info.ehci_count = debug.ehci_count;
    info.xhci_count = debug.xhci_count;
    info.powered_port_count = debug.powered_port_count;
    info.keyboard_active = debug.keyboard_active;
    info.keyboard_port = debug.keyboard_port;
    info.keyboard_endpoint = debug.keyboard_endpoint;
    info.keyboard_packet_size = debug.keyboard_packet_size;
    info.keyboard_interval = debug.keyboard_interval;
    info.keyboard_poll_count = debug.keyboard_poll_count;
    info.keyboard_report_count = debug.keyboard_report_count;
    info.keyboard_fail_count = debug.keyboard_fail_count;
    info.keyboard_last_cc = debug.keyboard_last_cc;
    info.mouse_active = debug.mouse_active;
    info.mouse_port = debug.mouse_port;
    info.mouse_endpoint = debug.mouse_endpoint;
    info.mouse_packet_size = debug.mouse_packet_size;
    info.mouse_interval = debug.mouse_interval;
    info.mouse_poll_count = debug.mouse_poll_count;
    info.mouse_report_count = debug.mouse_report_count;
    info.mouse_fail_count = debug.mouse_fail_count;
    info.mouse_last_cc = debug.mouse_last_cc;
    info.service_active = debug.service_active;
    info.storage_active = debug.storage_active;
    info.storage_port = debug.storage_port;
    info.last_bar = debug.last_bar;
    info.last_ports = debug.last_ports;
    info.last_port_status0 = debug.last_port_status0;
    info.last_port_status1 = debug.last_port_status1;
    info.last_bus = debug.last_bus;
    info.last_slot = debug.last_slot;
    info.last_func = debug.last_func;
    info.last_prog_if = debug.last_prog_if;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

int sys_usb_diag_op_impl(unsigned int op, unsigned int arg) {
    switch (op) {
        case SYS_USB_DIAG_OP_PORT_SNAPSHOT: {
            sys_usb_port_snapshot_t snapshot;
            if (!arg) return -EFAULT;
            usb_port_snapshot(&snapshot);
            if (copy_to_user((sys_usb_port_snapshot_t*)arg,
                             &snapshot,
                             sizeof(snapshot)) < 0) {
                return -EFAULT;
            }
            return 0;
        }
        case SYS_USB_DIAG_OP_PORTS:
            usb_dump_ports();
            return 0;
        case SYS_USB_DIAG_OP_DIAG:
            usb_diag();
            return 0;
        case SYS_USB_DIAG_OP_PEEK:
            usb_peek_port(arg);
            return 0;
        case SYS_USB_DIAG_OP_POWER:
            return (int)usb_power_ohci_ports();
        default:
            return -EINVAL;
    }
}

int sys_usb_mouse_op_impl(unsigned int op, unsigned int port) {
    switch (op) {
        case SYS_USB_MOUSE_OP_OPEN:
            return usb_mouse_open_port_quiet(port) ? 1 : 0;
        case SYS_USB_MOUSE_OP_POLL:
            return usb_mouse_poll_once();
        case SYS_USB_MOUSE_OP_CLOSE:
            usb_mouse_close();
            return 0;
        default:
            return -EINVAL;
    }
}

int sys_input_read_impl(syscall_regs_t* regs,
                               sys_input_event_t* out_events,
                               unsigned int max_events,
                               unsigned int flags) {
    unsigned int bytes;
    unsigned int copied = 0;
    process_t* proc;

    (void)regs;

    if ((flags & ~SYS_INPUT_FLAG_NONBLOCK) != 0u) return -EINVAL;
    if (max_events == 0u) return 0;
    if (max_events > INPUT_READ_MAX_EVENTS) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)out_events,
                             max_events,
                             sizeof(sys_input_event_t),
                             &bytes)) {
        return -EFAULT;
    }

    proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    while (1) {
        __asm__ volatile ("cli");
        if (input_available()) {
            break;
        }
        if (flags & SYS_INPUT_FLAG_NONBLOCK) {
            return 0;
        }
        proc->state = PROCESS_STATE_WAITING;
        input_set_waiting_process(proc);
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("sti");

    while (copied < max_events) {
        sys_input_event_t ev;
        if (!input_pop_event(&ev)) {
            break;
        }
        k_memcpy(&out_events[copied], &ev, sizeof(ev));
        copied++;
    }

    return (int)copied;
}

int sys_input_wait_until_impl(syscall_regs_t* regs,
                                     unsigned int deadline) {
    process_t* proc = (process_t*)sched_current();

    (void)regs;

    if (!proc) return -EINVAL;

    __asm__ volatile ("cli");
    if (input_available()) {
        __asm__ volatile ("sti");
        return 1;
    }
    if ((int)(timer_get_ticks() - deadline) >= 0) {
        __asm__ volatile ("sti");
        return 0;
    }

    proc->sleep_until = deadline;
    proc->state = PROCESS_STATE_SLEEPING;
    input_set_waiting_process(proc);
    sys_wait_until_current_running(proc);
    input_forget_waiting_process(proc);
    __asm__ volatile ("sti");

    return input_available() ? 1 : 0;
}

int sys_sound_op_impl(unsigned int op, unsigned int arg1,
                             unsigned int arg2) {
    switch (op) {
    case SYS_SOUND_OP_PCM_U8:
    case SYS_SOUND_OP_PCM_U8_LEGACY:
    case SYS_SOUND_OP_PCM_U8_SB16_8: {
        sys_sound_pcm_u8_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u || req.count > SYS_SOUND_PCM_MAX_SAMPLES ||
            req.sample_hz < SYS_SOUND_PCM_MIN_HZ ||
            req.sample_hz > SYS_SOUND_PCM_MAX_HZ) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.samples, req.count,
                                 sizeof(req.samples[0]), &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        if (op == SYS_SOUND_OP_PCM_U8_LEGACY) {
            return sound_pcm_u8_legacy(req.samples, req.count, req.sample_hz);
        }
        if (op == SYS_SOUND_OP_PCM_U8_SB16_8) {
            return sound_pcm_u8_sb16_8(req.samples, req.count, req.sample_hz);
        }
        return sound_pcm_u8(req.samples, req.count, req.sample_hz);
    }
    case SYS_SOUND_OP_PIT_SEQUENCE: {
        sys_sound_pit_sequence_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u || req.count > SYS_SOUND_SEQUENCE_MAX_SAMPLES ||
            req.sample_hz == 0u || req.divisor_scale == 0u) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.samples, req.count,
                                 sizeof(req.samples[0]), &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        return sound_pit_sequence(req.samples, req.count, req.sample_hz,
                                  req.divisor_scale);
    }
    case SYS_SOUND_OP_OPL_SEQUENCE: {
        sys_sound_opl_sequence_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u ||
            req.count > SYS_SOUND_OPL_SEQUENCE_MAX_EVENTS ||
            req.timer_hz == 0u ||
            (req.flags & ~SYS_SOUND_OPL_SEQUENCE_FLAG_LOOP) != 0u) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.events,
                                 req.count,
                                 sizeof(req.events[0]),
                                 &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        return sound_opl_sequence(req.events, req.count,
                                  req.timer_hz, req.flags);
    }
    case SYS_SOUND_OP_OPL_EFFECT: {
        sys_sound_opl_effect_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u ||
            req.count > SYS_SOUND_OPL_EFFECT_MAX_SAMPLES ||
            req.sample_hz == 0u ||
            req.sample_hz > SYS_SOUND_MAX_HZ ||
            req.channel >= 9u) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.samples,
                                 req.count,
                                 sizeof(req.samples[0]),
                                 &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        return sound_opl_effect(&req);
    }
    case SYS_SOUND_OP_TONE:
        return sound_tone(arg1, arg2);
    case SYS_SOUND_OP_STOP:
        sound_stop();
        return 0;
    case SYS_SOUND_OP_PIT_DIVISOR:
        return sound_pit_divisor(arg1, arg2);
    case SYS_SOUND_OP_OPL_WRITE:
        return sound_opl_write(arg1, arg2);
    case SYS_SOUND_OP_OPL_RESET:
        return sound_opl_reset();
    case SYS_SOUND_OP_OPL_SEQUENCE_STOP:
        sound_opl_sequence_stop();
        return 0;
    case SYS_SOUND_OP_OPL_EFFECT_STOP:
        sound_opl_effect_stop();
        return 0;
    case SYS_SOUND_OP_CAPS:
        return (int)sound_caps();
    case SYS_SOUND_OP_STATUS: {
        sys_sound_status_t status;

        if (!user_buf_ok(arg1, sizeof(status))) {
            return -EFAULT;
        }
        sound_status(&status);
        return copy_to_user((void*)arg1, &status, sizeof(status));
    }
    default:
        return -EINVAL;
    }
}
