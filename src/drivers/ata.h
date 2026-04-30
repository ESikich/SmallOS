#ifndef ATA_H
#define ATA_H

typedef unsigned int  u32;
typedef unsigned short u16;

/*
 * ata.h — ATA PIO driver (primary channel, master drive)
 *
 * Provides 28-bit LBA sector reads via polling (no DMA, no IRQ).
 * Suitable for reading a FAT16 filesystem in 32-bit protected mode.
 *
 * Hardware:
 *   Primary ATA channel  I/O base  0x1F0
 *   Control register              0x3F6
 *   Drive: master (0xE0)
 *
 * QEMU emulates this as a standard IDE controller — no detection needed.
 */

/*
 * ata_init()
 *
 * Issue a software reset and wait for the drive to become ready.
 * Must be called once before any read.
 */
void ata_init(void);

/*
 * ata_read_sectors(lba, count, buf)
 *
 * Read `count` 512-byte sectors starting at 28-bit LBA address `lba`
 * into the buffer at `buf`.  `buf` must be at least count * 512 bytes.
 *
 * Returns 1 on success, 0 on error (BSY timeout or ERR/DF set).
 *
 * Reads are blocking (polling BSY/DRQ).  Interrupts do not need to be
 * enabled.  count must be 1–255; passing 0 reads 256 sectors per ATA
 * convention — avoid 0 here.
 */
int ata_read_sectors(u32 lba, unsigned char count, void* buf);

/*
 * ata_write_sectors(lba, count, buf)
 *
 * Write `count` 512-byte sectors starting at 28-bit LBA address `lba`
 * from the buffer at `buf`.
 *
 * Returns 1 on success, 0 on error.  Like reads, this is blocking PIO
 * and only supports the primary channel master drive.
 */
int ata_write_sectors(u32 lba, unsigned char count, const void* buf);

#endif /* ATA_H */
