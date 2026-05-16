#include "usb_storage.h"

#include "terminal.h"
#include "usb.h"
#include "../kernel/klib.h"
#include "../kernel/types.h"

#define USB_BOT_CBW_SIGNATURE 0x43425355u
#define USB_BOT_CSW_SIGNATURE 0x53425355u
#define USB_BOT_CBW_LEN       31u
#define USB_BOT_CSW_LEN       13u

#define SCSI_TEST_UNIT_READY  0x00u
#define SCSI_REQUEST_SENSE    0x03u
#define SCSI_INQUIRY          0x12u
#define SCSI_READ_CAPACITY_10 0x25u
#define SCSI_READ_10          0x28u

#define USB_STORAGE_READ10_MAX_SECTORS 8u

static usb_mass_device_t s_mass_dev;
static int s_ready = 0;
static u32 s_sector_count = 0;
static u32 s_sector_size = 512u;
static u32 s_tag = 1u;
static u8 s_cbw[USB_BOT_CBW_LEN] __attribute__((aligned(16)));
static u8 s_csw[USB_BOT_CSW_LEN] __attribute__((aligned(16)));
static u8 s_sense[18] __attribute__((aligned(16)));
static u8 s_inquiry[36] __attribute__((aligned(16)));
static u8 s_capacity[8] __attribute__((aligned(16)));

static void write_u32_le(u8* buf, unsigned int off, u32 value) {
    buf[off] = (u8)(value & 0xFFu);
    buf[off + 1u] = (u8)((value >> 8) & 0xFFu);
    buf[off + 2u] = (u8)((value >> 16) & 0xFFu);
    buf[off + 3u] = (u8)((value >> 24) & 0xFFu);
}

static u32 read_u32_le(const u8* buf, unsigned int off) {
    return (u32)buf[off] |
           ((u32)buf[off + 1u] << 8) |
           ((u32)buf[off + 2u] << 16) |
           ((u32)buf[off + 3u] << 24);
}

static u32 read_u32_be(const u8* buf, unsigned int off) {
    return ((u32)buf[off] << 24) |
           ((u32)buf[off + 1u] << 16) |
           ((u32)buf[off + 2u] << 8) |
           (u32)buf[off + 3u];
}

static int bot_command(const u8* cdb,
                       unsigned int cdb_len,
                       void* data,
                       unsigned int data_len,
                       int data_in) {
    u32 tag = s_tag++;
    unsigned int actual = 0;

    if (!cdb || cdb_len == 0u || cdb_len > 16u) {
        return 0;
    }

    k_memset(s_cbw, 0, sizeof(s_cbw));
    write_u32_le(s_cbw, 0, USB_BOT_CBW_SIGNATURE);
    write_u32_le(s_cbw, 4, tag);
    write_u32_le(s_cbw, 8, data_len);
    s_cbw[12] = data_in ? 0x80u : 0x00u;
    s_cbw[13] = 0;
    s_cbw[14] = (u8)cdb_len;
    k_memcpy(s_cbw + 15, cdb, cdb_len);

    if (!usb_bulk_out(&s_mass_dev, s_cbw, sizeof(s_cbw))) {
        terminal_puts("usbms: CBW failed cmd=");
        terminal_put_hex(cdb[0]);
        terminal_putc('\n');
        return 0;
    }

    if (data_len != 0u) {
        if (!data) {
            return 0;
        }
        if (data_in) {
            if (!usb_bulk_in(&s_mass_dev, data, data_len, &actual)) {
                terminal_puts("usbms: data IN failed cmd=");
                terminal_put_hex(cdb[0]);
                terminal_putc('\n');
                return 0;
            }
        } else {
            if (!usb_bulk_out(&s_mass_dev, data, data_len)) {
                terminal_puts("usbms: data OUT failed cmd=");
                terminal_put_hex(cdb[0]);
                terminal_putc('\n');
                return 0;
            }
            actual = data_len;
        }
    }

    k_memset(s_csw, 0, sizeof(s_csw));
    if (!usb_bulk_in(&s_mass_dev, s_csw, sizeof(s_csw), &actual)) {
        terminal_puts("usbms: CSW read failed cmd=");
        terminal_put_hex(cdb[0]);
        terminal_putc('\n');
        return 0;
    }
    if (read_u32_le(s_csw, 0) != USB_BOT_CSW_SIGNATURE ||
        read_u32_le(s_csw, 4) != tag) {
        terminal_puts("usbms: bad CSW\n");
        return 0;
    }
    if (s_csw[12] != 0u) {
        terminal_puts("usbms: command status=");
        terminal_put_uint(s_csw[12]);
        terminal_putc('\n');
        return 0;
    }
    return 1;
}

static int scsi_request_sense(void) {
    u8 cdb[16];
    k_memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = sizeof(s_sense);
    k_memset(s_sense, 0, sizeof(s_sense));
    return bot_command(cdb, 6u, s_sense, sizeof(s_sense), 1);
}

static int scsi_test_unit_ready(void) {
    u8 cdb[16];
    k_memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    if (bot_command(cdb, 6u, 0, 0, 0)) {
        return 1;
    }
    (void)scsi_request_sense();
    return 0;
}

static int scsi_inquiry(void) {
    u8 cdb[16];
    k_memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = sizeof(s_inquiry);
    k_memset(s_inquiry, 0, sizeof(s_inquiry));
    return bot_command(cdb, 6u, s_inquiry, sizeof(s_inquiry), 1);
}

static int scsi_read_capacity(void) {
    u8 cdb[16];
    k_memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY_10;
    k_memset(s_capacity, 0, sizeof(s_capacity));
    if (!bot_command(cdb, 10u, s_capacity, sizeof(s_capacity), 1)) {
        return 0;
    }

    s_sector_count = read_u32_be(s_capacity, 0) + 1u;
    s_sector_size = read_u32_be(s_capacity, 4);
    return s_sector_count != 0u && s_sector_size != 0u;
}

static int scsi_read10(u32 lba, unsigned short count, void* buf) {
    u8 cdb[16];
    if (count == 0u) {
        return 1;
    }
    k_memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_10;
    cdb[2] = (u8)((lba >> 24) & 0xFFu);
    cdb[3] = (u8)((lba >> 16) & 0xFFu);
    cdb[4] = (u8)((lba >> 8) & 0xFFu);
    cdb[5] = (u8)(lba & 0xFFu);
    cdb[7] = (u8)((count >> 8) & 0xFFu);
    cdb[8] = (u8)(count & 0xFFu);
    return bot_command(cdb, 10u, buf, (u32)count * s_sector_size, 1);
}

static int usb_storage_block_read(block_device_t* dev, u32 lba, u32 count, void* buf) {
    u8* out = (u8*)buf;
    (void)dev;

    if (!s_ready || !buf || s_sector_size != 512u ||
        lba + count < lba || lba + count > s_sector_count) {
        return 0;
    }
    while (count != 0u) {
        unsigned short chunk = count > USB_STORAGE_READ10_MAX_SECTORS
                             ? USB_STORAGE_READ10_MAX_SECTORS
                             : (unsigned short)count;
        if (!scsi_read10(lba, chunk, out)) {
            return 0;
        }
        lba += chunk;
        count -= chunk;
        out += (u32)chunk * 512u;
    }
    return 1;
}

static int usb_storage_block_write(block_device_t* dev,
                                   u32 lba,
                                   u32 count,
                                   const void* buf) {
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return 0;
}

static block_device_t s_usb_storage_block = {
    "usb0",
    512u,
    0u,
    1,
    &s_mass_dev,
    usb_storage_block_read,
    usb_storage_block_write
};

int usb_storage_init(void) {
    s_ready = 0;
    if (!usb_find_mass_storage(&s_mass_dev)) {
        return 0;
    }
    if (!scsi_inquiry()) {
        terminal_puts("usbms: INQUIRY failed\n");
        return 0;
    }
    int unit_ready = 0;
    for (unsigned int i = 0; i < 5u; i++) {
        if (scsi_test_unit_ready()) {
            unit_ready = 1;
            break;
        }
    }
    if (!unit_ready) {
        terminal_puts("usbms: TEST UNIT READY failed\n");
        return 0;
    }
    if (!scsi_read_capacity()) {
        terminal_puts("usbms: READ CAPACITY failed\n");
        return 0;
    }
    if (s_sector_size != 512u) {
        terminal_puts("usbms: unsupported sector size=");
        terminal_put_uint(s_sector_size);
        terminal_putc('\n');
        return 0;
    }

    s_usb_storage_block.sector_size = s_sector_size;
    s_usb_storage_block.sector_count = s_sector_count;
    s_ready = 1;
    terminal_puts("usbms: ready sectors=");
    terminal_put_uint(s_sector_count);
    terminal_puts(" size=");
    terminal_put_uint(s_sector_size);
    terminal_puts(" read-only\n");
    return 1;
}

block_device_t* usb_storage_block_device(void) {
    return s_ready ? &s_usb_storage_block : 0;
}
