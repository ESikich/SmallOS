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
KERNEL_OFFSET := 0x1000
STAGE2_STACK_TOP := 0xF000
STAGE2_STACK_TOP_32 := 0xF0000
# Build-time guard: keep stage 2's stack safely above the loaded kernel image.
STAGE2_SAFE_KERNEL_SECTORS := $(shell echo $$(( ( $(STAGE2_STACK_TOP) - $(KERNEL_OFFSET) ) / $(BOOT_SECTOR_SIZE) )))
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
	$(DRIVERS_DIR)/fat16.c \
	$(DRIVERS_DIR)/serial.c

USER_PROGS=hello ticks args runelf_test readline exec_test fileread fault
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

$(OBJ_DIR)/boot/%.o: $(BOOT_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/%.o: $(KERNEL_DIR)/%.asm | dirs
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

$(TOOLS_DIR)/mkimage: tools/mkimage.c | dirs
	$(HOST_CC) -o $@ $<

#
# FAT16 partition image (fixed size, no external dependencies)
#
$(BIN_DIR)/fat16.img: $(USER_ELFS) $(TOOLS_DIR)/mkfat16 | dirs
	$(TOOLS_DIR)/mkfat16 $@ $(USER_ELFS)

$(GEN_DIR)/loader2.gen.asm: $(BOOT_DIR)/loader2.asm $(BIN_DIR)/kernel.bin | dirs
	@kernel_sectors=$$(( ($$(wc -c < $(BIN_DIR)/kernel.bin) + $(BOOT_SECTOR_MASK)) / $(BOOT_SECTOR_SIZE) )); \
	if [ $$kernel_sectors -gt $(STAGE2_SAFE_KERNEL_SECTORS) ]; then \
		echo "ERROR: kernel needs $$kernel_sectors sectors, but stage2 only allows $(STAGE2_SAFE_KERNEL_SECTORS)"; \
		echo "       move the stage-2 stack higher or shrink the kernel"; \
		exit 1; \
	fi; \
	echo "kernel:  $$(wc -c < $(BIN_DIR)/kernel.bin) bytes ($$kernel_sectors sectors, LBA $(KERNEL_LBA))"; \
	sed \
		-e "s/__KERNEL_SECTORS__/$$kernel_sectors/" \
		-e "s/__STAGE2_STACK_TOP__/$(STAGE2_STACK_TOP)/" \
		-e "s/__STAGE2_STACK_TOP_32__/$(STAGE2_STACK_TOP_32)/" \
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
# The stage-2 stack top and kernel-size ceiling come from the same constants,
# so a growth that would overlap the loader becomes a build error instead of a
# boot-time hang.
#
# The FAT16 partition start LBA is patched as a little-endian u32 into the
# boot-sector field declared by FAT16_LBA_PATCH_OFFSET in boot.asm so the kernel can read
# it at runtime without any compile-time dependency.
#
$(IMG_DIR)/os-image.bin: $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(BIN_DIR)/fat16.img $(TOOLS_DIR)/mkimage | dirs
	$(TOOLS_DIR)/mkimage \
		--boot $(BIN_DIR)/boot.bin \
		--loader $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fat16 $(BIN_DIR)/fat16.img \
		--out $@ \
		--sector-size $(BOOT_SECTOR_SIZE) \
		--kernel-lba $(KERNEL_LBA) \
		--loader-size $(LOADER2_SIZE_BYTES) \
		--boot-fat16-lba-patch-offset $(BOOT_FAT16_LBA_PATCH_OFFSET)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/*/*.d)

QEMU=qemu-system-i386
SERIAL_LOG=/tmp/smallos-serial.log
MONITOR_SOCK=/tmp/smallos-monitor.sock
PIDFILE=/tmp/smallos.pid
SMOKE_TIMEOUT=120.0
PYTHON3=python3
QEMUFLAGS=-drive format=raw,file=$(IMG_DIR)/os-image.bin -m 32 \
          -serial file:$(SERIAL_LOG)

.PHONY: all dirs run run-headless test smoke smoke-reboot smoke-halt clean

run: $(IMG_DIR)/os-image.bin
	$(QEMU) $(QEMUFLAGS) -display curses

run-headless: $(IMG_DIR)/os-image.bin
	$(QEMU) $(QEMUFLAGS) -display none \
	    -monitor unix:/tmp/smallos-monitor.sock,server,nowait \
	    -daemonize -pidfile /tmp/smallos.pid

test: $(IMG_DIR)/os-image.bin
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless
	$(PYTHON3) tools/qemu_selftest.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE)

smoke: $(IMG_DIR)/os-image.bin
	$(MAKE) smoke-reboot
	$(MAKE) smoke-halt

smoke-reboot: $(IMG_DIR)/os-image.bin
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless
	$(PYTHON3) tools/qemu_smoke.py \
		--command reboot \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

smoke-halt: $(IMG_DIR)/os-image.bin
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless
	$(PYTHON3) tools/qemu_smoke.py \
		--command halt \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

clean:
	rm -rf $(BUILD_DIR)
