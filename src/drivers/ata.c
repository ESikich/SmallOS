#include "ata.h"
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

/* How many status reads to spin before declaring a timeout */
#define ATA_TIMEOUT   100000u

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void ata_init(void) {
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
        return;
    }

    terminal_puts("ata: ready\n");
}

int ata_read_sectors(u32 lba, unsigned char count, void* buf) {
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
    outb(ATA_DRIVE,    (unsigned char)(ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0Fu)));
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0,     (unsigned char)( lba        & 0xFFu));
    outb(ATA_LBA1,     (unsigned char)((lba >>  8) & 0xFFu));
    outb(ATA_LBA2,     (unsigned char)((lba >> 16) & 0xFFu));

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

        unsigned int w;
        for (w = 0; w < 256; w++) {
            /*
             * inw — 16-bit I/O read.  Not in ports.h, so inline here.
             */
            u16 word;
            __asm__ __volatile__("inw %1, %0" : "=a"(word) : "Nd"((u16)ATA_DATA));
            *dest++ = word;
        }
    }

    return 1;
}