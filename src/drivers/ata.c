#include "ata.h"
#include "pci.h"
#include "../kernel/klib.h"
#include "../kernel/paging.h"
#include "../kernel/pmm.h"
#include "../kernel/ports.h"
#include "../drivers/terminal.h"

/* ------------------------------------------------------------------ */
/* ATA primary channel register map                                    */
/* ------------------------------------------------------------------ */

#define ATA_BASE        0x1F0u

#define ATA_DATA        (ATA_BASE + 0)   /* 16-bit data register          */
#define ATA_ERROR       (ATA_BASE + 1)   /* error register (read)         */
#define ATA_FEATURES    (ATA_BASE + 1)   /* features register (write)     */
#define ATA_SECCOUNT    (ATA_BASE + 2)   /* sector count                  */
#define ATA_LBA0        (ATA_BASE + 3)   /* LBA bits  0– 7                */
#define ATA_LBA1        (ATA_BASE + 4)   /* LBA bits  8–15                */
#define ATA_LBA2        (ATA_BASE + 5)   /* LBA bits 16–23                */
#define ATA_DRIVE       (ATA_BASE + 6)   /* drive / LBA bits 24–27        */
#define ATA_STATUS      (ATA_BASE + 7)   /* status register (read)        */
#define ATA_COMMAND     (ATA_BASE + 7)   /* command register (write)      */

#define ATA_CTRL        0x3F6u           /* device control / alt status   */

/* Status register bits */
#define ATA_SR_BSY      0x80u   /* drive busy                            */
#define ATA_SR_DRDY     0x40u   /* drive ready                           */
#define ATA_SR_DF       0x20u   /* drive write fault                     */
#define ATA_SR_ERR      0x01u   /* error occurred                        */
#define ATA_SR_DRQ      0x08u   /* data request — PIO data ready         */

/* Drive register: master + LBA mode */
#define ATA_DRIVE_MASTER_LBA  0xE0u   /* 1110 xxxx — master, LBA mode    */

/* ATA commands */
#define ATA_CMD_READ_SECTORS  0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_READ_DMA      0xC8u
#define ATA_CMD_WRITE_DMA     0xCAu

/* How many status reads to spin before declaring a timeout */
#define ATA_TIMEOUT   100000u
#define ATA_DMA_TIMEOUT 10000000u

/* PCI IDE / bus-master DMA constants */
#define PCI_CLASS_STORAGE 0x01u
#define PCI_SUBCLASS_IDE  0x01u
#define PCI_COMMAND_IO    0x0001u
#define PCI_COMMAND_BM    0x0004u

#define ATA_BM_PRIMARY_OFF 0x00u
#define ATA_BM_CMD         0x00u
#define ATA_BM_STATUS      0x02u
#define ATA_BM_PRDT        0x04u
#define ATA_BM_CMD_START   0x01u
#define ATA_BM_CMD_READ    0x08u
#define ATA_BM_STATUS_ACTIVE 0x01u
#define ATA_BM_STATUS_ERROR  0x02u
#define ATA_BM_STATUS_IRQ    0x04u

#define ATA_DMA_BUFFER_FRAMES 32u
#define ATA_DMA_BUFFER_BYTES  (ATA_DMA_BUFFER_FRAMES * PMM_FRAME_SIZE)
#define ATA_DMA_PRD_MAX       8u

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 base;
    u16 byte_count;
    u16 flags;
} __attribute__((packed)) ata_prd_t;

static int s_dma_enabled = 0;
static unsigned short s_bm_base = 0;
static u32 s_dma_buf_phys = 0;
static u8* s_dma_buf = 0;
static u32 s_prdt_phys = 0;
static ata_prd_t* s_prdt = 0;

/*
 * ata_poll_bsy()
 *
 * Spin until BSY clears or timeout.  Returns 1 if BSY cleared in time,
 * 0 on timeout.
 */
static int ata_poll_bsy(void) {
    unsigned int i;
    for (i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY)) return 1;
    }
    return 0;
}

/*
 * ata_poll_drq()
 *
 * After issuing a read command, spin until DRQ is set (data ready) or
 * an error/fault bit appears.  Returns 1 if DRQ set, 0 on error/timeout.
 */
static int ata_poll_drq(void) {
    unsigned int i;
    for (i = 0; i < ATA_TIMEOUT; i++) {
        unsigned char status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) { terminal_puts("ata: ERR\n"); return 0; }
        if (status & ATA_SR_DF)  { terminal_puts("ata: DF\n");  return 0; }
        if (status & ATA_SR_DRQ) return 1;
    }
    terminal_puts("ata: DRQ timeout\n");
    return 0;
}

/*
 * ata_poll_complete()
 *
 * Wait for the drive to finish a write or flush command.
 */
static int ata_poll_complete(void) {
    unsigned int i;
    for (i = 0; i < ATA_TIMEOUT; i++) {
        unsigned char status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) { terminal_puts("ata: ERR\n"); return 0; }
        if (status & ATA_SR_DF)  { terminal_puts("ata: DF\n");  return 0; }
        if (!(status & ATA_SR_BSY)) return 1;
    }
    terminal_puts("ata: complete timeout\n");
    return 0;
}

/*
 * ata_400ns_delay()
 *
 * Read the alternate status register four times (~400 ns) to give the
 * drive time to update its status after a command or reset.
 */
static void ata_400ns_delay(void) {
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
}

static void ata_select_lba28(u32 lba, unsigned char count) {
    outb(ATA_DRIVE,    (unsigned char)(ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0Fu)));
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0,     (unsigned char)( lba        & 0xFFu));
    outb(ATA_LBA1,     (unsigned char)((lba >>  8) & 0xFFu));
    outb(ATA_LBA2,     (unsigned char)((lba >> 16) & 0xFFu));
}

static int ata_find_ide_controller(pci_device_t* out) {
    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int slot = 0; slot < 32; slot++) {
            unsigned short vendor;
            unsigned char header_type;
            unsigned int function_count;

            vendor = pci_read_config_word((unsigned char)bus,
                                          (unsigned char)slot, 0, 0x00);
            if (vendor == 0xFFFFu) continue;

            header_type = pci_read_config_byte((unsigned char)bus,
                                               (unsigned char)slot, 0, 0x0E);
            function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu) continue;
                if (dev.class_code == PCI_CLASS_STORAGE &&
                    dev.subclass == PCI_SUBCLASS_IDE) {
                    if (out) *out = dev;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int ata_dma_build_prdt(u32 phys, u32 bytes) {
    unsigned int i = 0;

    if (!s_prdt || bytes == 0u) return 0;

    while (bytes > 0u) {
        u32 boundary = 0x10000u - (phys & 0xFFFFu);
        u32 chunk = bytes;

        if (i >= ATA_DMA_PRD_MAX) return 0;
        if (chunk > boundary) chunk = boundary;
        if (chunk > 0x10000u) chunk = 0x10000u;

        s_prdt[i].base = phys;
        s_prdt[i].byte_count = (u16)(chunk == 0x10000u ? 0u : chunk);
        s_prdt[i].flags = 0u;

        phys += chunk;
        bytes -= chunk;
        i++;
    }

    s_prdt[i - 1u].flags = 0x8000u;
    return 1;
}

static int ata_dma_wait(void) {
    unsigned int i;

    for (i = 0; i < ATA_DMA_TIMEOUT; i++) {
        unsigned char bm_status = inb((unsigned short)(s_bm_base + ATA_BM_PRIMARY_OFF + ATA_BM_STATUS));
        unsigned char ata_status;

        if ((bm_status & ATA_BM_STATUS_ERROR) != 0u) {
            return 0;
        }
        if ((bm_status & ATA_BM_STATUS_ACTIVE) != 0u) {
            continue;
        }

        ata_status = inb(ATA_STATUS);
        if ((ata_status & ATA_SR_BSY) != 0u) {
            continue;
        }
        if ((ata_status & (ATA_SR_ERR | ATA_SR_DF)) != 0u) {
            return 0;
        }
        return 1;
    }
    return 0;
}

static int ata_dma_transfer(u32 lba, unsigned char count, void* buf, int is_read) {
    u32 bytes = (u32)count * 512u;
    unsigned short bm_cmd_port;
    unsigned short bm_status_port;
    unsigned short bm_prdt_port;
    unsigned char direction;
    int ok;

    if (!s_dma_enabled || !buf || count == 0u || bytes > ATA_DMA_BUFFER_BYTES) {
        return 0;
    }
    if (!is_read) {
        k_memcpy(s_dma_buf, buf, bytes);
    }
    if (!ata_dma_build_prdt(s_dma_buf_phys, bytes)) {
        return 0;
    }
    if (!ata_poll_bsy()) {
        return 0;
    }

    bm_cmd_port = (unsigned short)(s_bm_base + ATA_BM_PRIMARY_OFF + ATA_BM_CMD);
    bm_status_port = (unsigned short)(s_bm_base + ATA_BM_PRIMARY_OFF + ATA_BM_STATUS);
    bm_prdt_port = (unsigned short)(s_bm_base + ATA_BM_PRIMARY_OFF + ATA_BM_PRDT);
    direction = is_read ? ATA_BM_CMD_READ : 0u;

    outb(bm_cmd_port, 0u);
    outb(bm_status_port, (unsigned char)(ATA_BM_STATUS_IRQ | ATA_BM_STATUS_ERROR));
    outl(bm_prdt_port, s_prdt_phys);
    outb(bm_cmd_port, direction);

    ata_select_lba28(lba, count);
    outb(ATA_COMMAND, is_read ? ATA_CMD_READ_DMA : ATA_CMD_WRITE_DMA);
    outb(bm_cmd_port, (unsigned char)(direction | ATA_BM_CMD_START));

    ok = ata_dma_wait();
    outb(bm_cmd_port, 0u);
    outb(bm_status_port, (unsigned char)(ATA_BM_STATUS_IRQ | ATA_BM_STATUS_ERROR));

    if (!ok) {
        return 0;
    }
    if (is_read) {
        k_memcpy(buf, s_dma_buf, bytes);
    }
    return 1;
}

static void ata_dma_try_init(void) {
    pci_device_t dev;
    unsigned int bar4;
    unsigned short command;

    if (!ata_find_ide_controller(&dev)) {
        return;
    }

    bar4 = pci_read_config_dword(dev.bus, dev.slot, dev.func, 0x20);
    if ((bar4 & 0x1u) == 0u) {
        return;
    }
    s_bm_base = (unsigned short)(bar4 & ~0xFu);
    if (s_bm_base == 0u) {
        return;
    }

    s_dma_buf_phys = pmm_alloc_contiguous_frames(ATA_DMA_BUFFER_FRAMES);
    s_prdt_phys = pmm_alloc_frame();
    if (!s_dma_buf_phys || !s_prdt_phys) {
        if (s_dma_buf_phys) pmm_free_contiguous_frames(s_dma_buf_phys, ATA_DMA_BUFFER_FRAMES);
        if (s_prdt_phys) pmm_free_frame(s_prdt_phys);
        s_dma_buf_phys = 0;
        s_prdt_phys = 0;
        return;
    }

    s_dma_buf = (u8*)paging_phys_to_kernel_virt(s_dma_buf_phys);
    s_prdt = (ata_prd_t*)paging_phys_to_kernel_virt(s_prdt_phys);
    k_memset(s_prdt, 0, PMM_FRAME_SIZE);

    command = pci_read_config_word(dev.bus, dev.slot, dev.func, 0x04);
    command |= (unsigned short)(PCI_COMMAND_IO | PCI_COMMAND_BM);
    pci_write_config_word(dev.bus, dev.slot, dev.func, 0x04, command);

    s_dma_enabled = 1;
    terminal_puts("ata: dma bm=");
    terminal_put_hex(s_bm_base);
    terminal_putc('\n');
}

static int ata_pio_read_sectors(u32 lba, unsigned char count, void* buf);
static int ata_pio_write_sectors(u32 lba, unsigned char count, const void* buf);

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int ata_init(void) {
    /*
     * Software reset: set SRST bit in device control, then clear it.
     * nIEN (bit 1) stays 0, so device interrupts are not disabled at
     * the drive level.  This driver still uses polling and does not rely
     * on ATA IRQ delivery.
     */
    outb(ATA_CTRL, 0x04);   /* SRST = 1 */
    ata_400ns_delay();
    outb(ATA_CTRL, 0x00);   /* SRST = 0 */
    ata_400ns_delay();

    if (!ata_poll_bsy()) {
        terminal_puts("ata: reset timeout\n");
        return 0;
    }

    terminal_puts("ata: ready\n");
    ata_dma_try_init();
    return 1;
}

static int ata_pio_read_sectors(u32 lba, unsigned char count, void* buf) {
    u16* dest = (u16*)buf;

    /*
     * Wait for drive to be idle before issuing a command.
     */
    if (!ata_poll_bsy()) {
        terminal_puts("ata: busy before command\n");
        return 0;
    }

    /*
     * Load LBA and sector count into the task file registers.
     *
     * Drive register: 1110 xxxx
     *   bit 7   = 1 (legacy, always set)
     *   bit 6   = 1 (LBA mode)
     *   bit 5   = 1 (legacy, always set)
     *   bit 4   = 0 (master)
     *   bits 3-0 = LBA bits 24–27
     */
    ata_select_lba28(lba, count);

    /* Issue READ SECTORS command */
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    /*
     * For each requested sector, wait for DRQ then read 256 16-bit
     * words (= 512 bytes) from the data register.
     */
    unsigned char s;
    for (s = 0; s < count; s++) {
        ata_400ns_delay();

        if (!ata_poll_drq()) {
            return 0;
        }

        {
            unsigned int words = 256u;
            __asm__ __volatile__(
                "cld; rep insw"
                : "+D"(dest), "+c"(words)
                : "d"((u16)ATA_DATA)
                : "memory");
        }
    }

    return 1;
}

static int ata_pio_write_sectors(u32 lba, unsigned char count, const void* buf) {
    const u16* src = (const u16*)buf;

    if (!ata_poll_bsy()) {
        terminal_puts("ata: busy before command\n");
        return 0;
    }

    ata_select_lba28(lba, count);

    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    unsigned char s;
    for (s = 0; s < count; s++) {
        ata_400ns_delay();

        if (!ata_poll_drq()) {
            return 0;
        }

        {
            unsigned int words = 256u;
            __asm__ __volatile__(
                "cld; rep outsw"
                : "+S"(src), "+c"(words)
                : "d"((u16)ATA_DATA)
                : "memory");
        }
    }

    return ata_poll_complete();
}

int ata_read_sectors(u32 lba, unsigned char count, void* buf) {
    if (s_dma_enabled) {
        if (ata_dma_transfer(lba, count, buf, 1)) {
            return 1;
        }
        s_dma_enabled = 0;
        terminal_puts("ata: dma read failed; using pio\n");
    }
    return ata_pio_read_sectors(lba, count, buf);
}

int ata_write_sectors(u32 lba, unsigned char count, const void* buf) {
    if (s_dma_enabled) {
        if (ata_dma_transfer(lba, count, (void*)buf, 0)) {
            return 1;
        }
        s_dma_enabled = 0;
        terminal_puts("ata: dma write failed; using pio\n");
    }
    return ata_pio_write_sectors(lba, count, buf);
}

static int ata_block_read(block_device_t* dev, u32 lba, u32 count, void* buf) {
    u8* out = (u8*)buf;
    (void)dev;

    while (count != 0u) {
        unsigned char chunk = count > 255u ? 255u : (unsigned char)count;
        if (!ata_read_sectors(lba, chunk, out)) {
            return 0;
        }
        lba += chunk;
        count -= chunk;
        out += (u32)chunk * 512u;
    }
    return 1;
}

static int ata_block_write(block_device_t* dev, u32 lba, u32 count, const void* buf) {
    const u8* in = (const u8*)buf;
    (void)dev;

    while (count != 0u) {
        unsigned char chunk = count > 255u ? 255u : (unsigned char)count;
        if (!ata_write_sectors(lba, chunk, in)) {
            return 0;
        }
        lba += chunk;
        count -= chunk;
        in += (u32)chunk * 512u;
    }
    return 1;
}

static block_device_t s_ata_block_device = {
    "ata0",
    512u,
    0u,
    0,
    0,
    ata_block_read,
    ata_block_write
};

block_device_t* ata_block_device(void) {
    return &s_ata_block_device;
}
