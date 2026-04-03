ASM=nasm
CC=i686-elf-gcc
LD=i686-elf-ld
OBJCOPY=i686-elf-objcopy

SRC_DIR=src
BOOT_DIR=$(SRC_DIR)/boot
KERNEL_DIR=$(SRC_DIR)/kernel
DRIVERS_DIR=$(SRC_DIR)/drivers
SHELL_DIR=$(SRC_DIR)/shell
EXEC_DIR=$(SRC_DIR)/exec
USER_DIR=$(SRC_DIR)/user

BUILD_DIR=build
OBJ_DIR=$(BUILD_DIR)/obj
BIN_DIR=$(BUILD_DIR)/bin
GEN_DIR=$(BUILD_DIR)/gen
IMG_DIR=$(BUILD_DIR)/img
TOOLS_DIR=$(BUILD_DIR)/tools

BOOT_SECTOR_SIZE := $(shell awk '/^BOOT_SECTOR_SIZE[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
BOOT_SECTOR_MASK := $(shell echo $$(( $(BOOT_SECTOR_SIZE) - 1 )))
KERNEL_LBA := $(shell awk '/^KERNEL_LBA[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/loader2.asm)
FAT16_TOTAL_SECTORS := $(shell awk '/^#define[[:space:]]+TOTAL_SECTORS[[:space:]]+/ {print $$3}' tools/mkfat16.c)
FAT16_TOTAL_SIZE_MB := $(shell awk '/^#define[[:space:]]+TOTAL_SIZE_MB[[:space:]]+/ {print $$3}' tools/mkfat16.c)
BOOT_FAT16_LBA_PATCH_OFFSET := $(shell awk '/^FAT16_LBA_PATCH_OFFSET[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_SIZE_BYTES := $(shell awk '/^LOADER2_SIZE_BYTES[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/loader2.asm)

CPPFLAGS=-I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(SHELL_DIR) -I$(EXEC_DIR) -I$(USER_DIR)
CFLAGS=-ffreestanding -m32 -fno-pie -fno-stack-protector -nostdlib -nostartfiles
DEPFLAGS=-MMD -MP
HOST_CC=gcc
LDFLAGS=-T linker.ld -m elf_i386
USER_LDFLAGS=-m elf_i386 -Ttext-segment 0x400000 -e _start

KERNEL_ASM_SRCS=\
	$(BOOT_DIR)/kernel_entry.asm \
	$(KERNEL_DIR)/interrupts.asm \
	$(KERNEL_DIR)/setjmp.asm \
	$(KERNEL_DIR)/sched_switch.asm

KERNEL_C_SRCS=\
	$(KERNEL_DIR)/kernel.c \
	$(KERNEL_DIR)/idt.c \
	$(DRIVERS_DIR)/keyboard.c \
	$(SHELL_DIR)/shell.c \
	$(SHELL_DIR)/line_editor.c \
	$(DRIVERS_DIR)/terminal.c \
	$(DRIVERS_DIR)/screen.c \
	$(KERNEL_DIR)/system.c \
	$(KERNEL_DIR)/timer.c \
	$(KERNEL_DIR)/klib.c \
	$(KERNEL_DIR)/memory.c \
	$(KERNEL_DIR)/pmm.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/scheduler.c \
	$(SHELL_DIR)/parse.c \
	$(SHELL_DIR)/commands.c \
	$(EXEC_DIR)/elf_loader.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/gdt.c \
	$(KERNEL_DIR)/paging.c \
	$(DRIVERS_DIR)/ata.c \
	$(DRIVERS_DIR)/fat16.c

USER_PROGS=hello ticks args runelf_test readline exec_test fileread
USER_SRCS=$(addprefix $(USER_DIR)/,$(addsuffix .c,$(USER_PROGS)))

KERNEL_OBJS=$(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASM_SRCS)) \
            $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(KERNEL_C_SRCS))

USER_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(USER_SRCS))
USER_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_PROGS)))

OBJ_SUBDIRS=$(sort \
	$(dir $(KERNEL_OBJS)) \
	$(dir $(USER_OBJS)) \
)

BUILD_SUBDIRS=$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(GEN_DIR) $(IMG_DIR) $(TOOLS_DIR) $(OBJ_SUBDIRS)

all: $(IMG_DIR)/os-image.bin

dirs:
	mkdir -p $(BUILD_SUBDIRS)

$(OBJ_DIR)/kernel/setjmp.o: $(KERNEL_DIR)/setjmp.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/sched_switch.o: $(KERNEL_DIR)/sched_switch.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/boot/kernel_entry.o: $(BOOT_DIR)/kernel_entry.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/interrupts.o: $(KERNEL_DIR)/interrupts.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/drivers/%.o: $(DRIVERS_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/shell/%.o: $(SHELL_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/exec/%.o: $(EXEC_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/%.o: $(USER_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(BIN_DIR)/kernel.elf: $(KERNEL_OBJS) linker.ld | dirs
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BIN_DIR)/kernel.bin: $(BIN_DIR)/kernel.elf | dirs
	$(OBJCOPY) -O binary $< $@

$(BIN_DIR)/%.elf: $(OBJ_DIR)/user/%.o | dirs
	$(LD) $(USER_LDFLAGS) $< -o $@

$(TOOLS_DIR)/mkfat16: tools/mkfat16.c | dirs
	$(HOST_CC) -o $@ $<

#
# FAT16 partition image (fixed size, no external dependencies)
#
$(BIN_DIR)/fat16.img: $(USER_ELFS) $(TOOLS_DIR)/mkfat16 | dirs
	$(TOOLS_DIR)/mkfat16 $@ $(USER_ELFS)

$(GEN_DIR)/loader2.gen.asm: $(BOOT_DIR)/loader2.asm $(BIN_DIR)/kernel.bin | dirs
	@kernel_sectors=$$(( ($$(wc -c < $(BIN_DIR)/kernel.bin) + $(BOOT_SECTOR_MASK)) / $(BOOT_SECTOR_SIZE) )); \
	echo "kernel:  $$(wc -c < $(BIN_DIR)/kernel.bin) bytes ($$kernel_sectors sectors, LBA $(KERNEL_LBA))"; \
	sed \
		-e "s/__KERNEL_SECTORS__/$$kernel_sectors/" \
		$< > $@

$(BIN_DIR)/loader2.bin: $(GEN_DIR)/loader2.gen.asm | dirs
	$(ASM) -f bin $< -o $@
	@size=$$(wc -c < $@); \
	if [ $$size -ne $(LOADER2_SIZE_BYTES) ]; then \
		echo "ERROR: loader2.bin must be $(LOADER2_SIZE_BYTES) bytes, got $$size"; \
		exit 1; \
	fi

$(BIN_DIR)/boot.bin: $(BOOT_DIR)/boot.asm | dirs
	$(ASM) -f bin $< -o $@

#
# Final disk image
#
# Layout:
#   boot.bin             ($(BOOT_SECTOR_SIZE) bytes,   LBA 0)
#   loader2.bin          ($(LOADER2_SIZE_BYTES) bytes, LBA 1-($(KERNEL_LBA)-1))
#   kernel_padded.bin    (sector-aligned, LBA $(KERNEL_LBA)+)
#   fat16.img            ($(FAT16_TOTAL_SIZE_MB) MB FAT16 partition, LBA $(KERNEL_LBA)+kernel_sectors)
#
# The FAT16 partition start LBA is patched as a little-endian u32 into the
# boot-sector field declared by FAT16_LBA_PATCH_OFFSET in boot.asm so the kernel can read
# it at runtime without any compile-time dependency.
#
$(IMG_DIR)/os-image.bin: $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(BIN_DIR)/fat16.img | dirs
	@kernel_size=$$(wc -c < $(BIN_DIR)/kernel.bin); \
	padded=$$(( ($$kernel_size + $(BOOT_SECTOR_MASK)) & ~$(BOOT_SECTOR_MASK) )); \
	pad=$$(( $$padded - $$kernel_size )); \
	cp $(BIN_DIR)/kernel.bin $(BIN_DIR)/kernel_padded.bin; \
	dd if=/dev/zero bs=1 count=$$pad >> $(BIN_DIR)/kernel_padded.bin 2>/dev/null; \
	kernel_sectors=$$(( $$padded / $(BOOT_SECTOR_SIZE) )); \
	fat16_lba=$$(( $(KERNEL_LBA) + $$kernel_sectors )); \
	echo "kernel:  $$kernel_size bytes ($$kernel_sectors sectors, LBA $(KERNEL_LBA))"; \
	echo "fat16:   $(FAT16_TOTAL_SECTORS) sectors ($(FAT16_TOTAL_SIZE_MB) MB), LBA $$fat16_lba"; \
	cat $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel_padded.bin \
		$(BIN_DIR)/fat16.img > $@; \
	lba0=$$(( $$fat16_lba & 0xFF )); \
	lba1=$$(( ($$fat16_lba >> 8) & 0xFF )); \
	lba2=$$(( ($$fat16_lba >> 16) & 0xFF )); \
	lba3=$$(( ($$fat16_lba >> 24) & 0xFF )); \
	printf "$$(printf '\\%03o\\%03o\\%03o\\%03o' $$lba0 $$lba1 $$lba2 $$lba3)" | \
		dd of=$@ bs=1 seek=$(BOOT_FAT16_LBA_PATCH_OFFSET) count=4 conv=notrunc 2>/dev/null; \
	echo "fat16:   LBA $$fat16_lba patched into sector 0 offset $(BOOT_FAT16_LBA_PATCH_OFFSET)"

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/*/*.d)

clean:
	rm -rf $(BUILD_DIR)