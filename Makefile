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
TINYCC_DIR=$(BUILD_DIR)/tinycc-host
TINYCC_CONFIG_STAMP=$(TINYCC_DIR)/.configured
TINYCC_SMALOS_OBJ_DIR=$(OBJ_DIR)/tinycc-smalos
TINYCC_SMALOS_OBJ=$(TINYCC_SMALOS_OBJ_DIR)/tcc.o
TINYCC_SMALOS_BIN=$(BIN_DIR)/tcc-smalos.elf
STATE_DIR=.state

BOOT_SECTOR_SIZE := $(shell awk '/^BOOT_SECTOR_SIZE[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
BOOT_SECTOR_MASK := $(shell echo $$(( $(BOOT_SECTOR_SIZE) - 1 )))
KERNEL_OFFSET := 0x1000
LOADER2_SEGMENT := $(shell awk '/^LOADER2_SEGMENT[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_OFFSET := $(shell awk '/^LOADER2_OFFSET[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_LOAD_ADDR := $(shell echo $$(( ( $(LOADER2_SEGMENT) << 4 ) + $(LOADER2_OFFSET) )))
STAGE2_STACK_TOP := 0xFF00
STAGE2_STACK_TOP_PHYS := $(shell printf '0x%X' $$(( $(LOADER2_LOAD_ADDR) + $(STAGE2_STACK_TOP) )))
STAGE2_STACK_TOP_32 := 0x1FF000
FAT16_TOTAL_SECTORS := $(shell awk '/^#define[[:space:]]+TOTAL_SECTORS[[:space:]]+/ {print $$3}' tools/mkfat16.c)
FAT16_TOTAL_SIZE_MB := $(shell awk '/^#define[[:space:]]+TOTAL_SIZE_MB[[:space:]]+/ {print $$3}' tools/mkfat16.c)
BOOT_PARTITION_TABLE_OFFSET := $(shell awk '/^MBR_PARTITION_TABLE_OFFSET[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
BOOT_PARTITION_ENTRY_SIZE := $(shell awk '/^MBR_PARTITION_ENTRY_SIZE[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_SIZE_BYTES := $(shell awk '/^LOADER2_SIZE_BYTES[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/loader2.asm)

CPPFLAGS=-I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(SHELL_DIR) -I$(EXEC_DIR) -I$(USER_DIR) -I$(CURDIR)/third_party/ftp_server/include
CFLAGS=-ffreestanding -m32 -fno-pie -fno-stack-protector -nostdlib -nostartfiles -Wa,--noexecstack
DEPFLAGS=-MMD -MP
HOST_CC=gcc
LIBGCC_FILE := $(shell $(CC) -print-libgcc-file-name)
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
	$(KERNEL_DIR)/vfs.c \
	$(KERNEL_DIR)/process.c \
	$(KERNEL_DIR)/scheduler.c \
	$(SHELL_DIR)/parse.c \
	$(SHELL_DIR)/commands.c \
	$(EXEC_DIR)/elf_loader.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/gdt.c \
	$(KERNEL_DIR)/paging.c \
	$(DRIVERS_DIR)/ata.c \
	$(DRIVERS_DIR)/pci.c \
	$(DRIVERS_DIR)/e1000.c \
	$(DRIVERS_DIR)/arp.c \
	$(DRIVERS_DIR)/ipv4.c \
	$(DRIVERS_DIR)/tcp.c \
	$(DRIVERS_DIR)/fat16.c \
	$(DRIVERS_DIR)/serial.c

USER_PROGS=echo about uptime halt reboot hello ticks args runelf_test readline exec_test fileread compiler_demo heapprobe statprobe fileprobe fault sleep_test ptrguard spinwkr preempt_test tcpecho ftpd
USER_SRCS=$(addprefix $(USER_DIR)/,$(addsuffix .c,$(USER_PROGS)))
USER_RUNTIME_SRCS=$(USER_DIR)/user_alloc.c $(USER_DIR)/user_stdio.c $(USER_DIR)/user_posix.c $(USER_DIR)/ftp_compat.c $(USER_DIR)/setjmp.asm
FAT16_ROOT_ENTRIES=echo.elf=$(BIN_DIR)/echo.elf about.elf=$(BIN_DIR)/about.elf uptime.elf=$(BIN_DIR)/uptime.elf halt.elf=$(BIN_DIR)/halt.elf reboot.elf=$(BIN_DIR)/reboot.elf
FAT16_DEMO_ENTRIES=apps/demo/hello.elf=$(BIN_DIR)/hello.elf
FAT16_TEST_ENTRIES=apps/tests/ticks.elf=$(BIN_DIR)/ticks.elf apps/tests/args.elf=$(BIN_DIR)/args.elf apps/tests/runelf_test.elf=$(BIN_DIR)/runelf_test.elf apps/tests/readline.elf=$(BIN_DIR)/readline.elf apps/tests/exec_test.elf=$(BIN_DIR)/exec_test.elf apps/tests/fileread.elf=$(BIN_DIR)/fileread.elf apps/tests/compiler_demo.elf=$(BIN_DIR)/compiler_demo.elf apps/tests/heapprobe.elf=$(BIN_DIR)/heapprobe.elf apps/tests/statprobe.elf=$(BIN_DIR)/statprobe.elf apps/tests/fileprobe.elf=$(BIN_DIR)/fileprobe.elf apps/tests/fault.elf=$(BIN_DIR)/fault.elf apps/tests/sleep_test.elf=$(BIN_DIR)/sleep_test.elf apps/tests/ptrguard.elf=$(BIN_DIR)/ptrguard.elf apps/tests/spinwkr.elf=$(BIN_DIR)/spinwkr.elf apps/tests/preempt_test.elf=$(BIN_DIR)/preempt_test.elf
FAT16_APP_ENTRIES=$(FAT16_DEMO_ENTRIES) $(FAT16_TEST_ENTRIES)
FAT16_APP_ENTRIES+= apps/services/tcpecho.elf=$(BIN_DIR)/tcpecho.elf apps/services/ftpd.elf=$(BIN_DIR)/ftpd.elf
FAT16_EXTRA_ENTRIES=tools/tcc.elf=$(TINYCC_SMALOS_BIN) tccmath.c=$(CURDIR)/samples/tccmath.c tccagg.c=$(CURDIR)/samples/tccagg.c tcctree.c=$(CURDIR)/samples/tcctree.c tccmini.c=$(CURDIR)/samples/tccmini.c
FAT16_EXTRA_FILES=$(foreach entry,$(FAT16_EXTRA_ENTRIES),$(word 2,$(subst =, ,$(entry))))

KERNEL_OBJS=$(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASM_SRCS)) \
            $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(KERNEL_C_SRCS))

USER_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(USER_SRCS))
USER_RUNTIME_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.c,$(USER_RUNTIME_SRCS))) \
                 $(patsubst $(USER_DIR)/%.asm,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.asm,$(USER_RUNTIME_SRCS)))
USER_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_PROGS)))

OBJ_SUBDIRS=$(sort \
	$(dir $(KERNEL_OBJS)) \
	$(dir $(USER_OBJS)) \
)

BUILD_SUBDIRS=$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(GEN_DIR) $(IMG_DIR) $(TOOLS_DIR) $(OBJ_SUBDIRS) $(STATE_DIR)
BUILD_SUBDIRS+=$(TINYCC_SMALOS_OBJ_DIR)

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

$(OBJ_DIR)/user/%.o: $(USER_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(TINYCC_SMALOS_OBJ): $(CURDIR)/third_party/tinycc/tcc.c $(TINYCC_CONFIG_STAMP) | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) -Os -ffunction-sections -fdata-sections -I$(TINYCC_DIR) -I$(CURDIR)/third_party/tinycc -DTCC_TARGET_I386 -DTCC_TARGET_SMALLOS -c $< -o $@

$(TINYCC_SMALOS_BIN): $(TINYCC_SMALOS_OBJ) $(OBJ_DIR)/user/tcc_entry.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) -s --gc-sections $^ $(LIBGCC_FILE) -o $@

$(BIN_DIR)/kernel.elf: $(KERNEL_OBJS) linker.ld | dirs
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BIN_DIR)/kernel.bin: $(BIN_DIR)/kernel.elf | dirs
	$(OBJCOPY) -O binary $< $@

$(BIN_DIR)/%.elf: $(OBJ_DIR)/user/%.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(TOOLS_DIR)/mkfat16: tools/mkfat16.c | dirs
	$(HOST_CC) -o $@ $<

$(TOOLS_DIR)/mkimage: tools/mkimage.c | dirs
	$(HOST_CC) -o $@ $<

# FAT16 seed image (generated from the current tree)
#
$(BIN_DIR)/fat16.seed.img: $(USER_ELFS) $(TOOLS_DIR)/mkfat16 $(FAT16_EXTRA_FILES) | dirs
	$(TOOLS_DIR)/mkfat16 $@ $(FAT16_ROOT_ENTRIES) $(FAT16_APP_ENTRIES) $(FAT16_EXTRA_ENTRIES)

$(STATE_DIR)/fat16.img: $(BIN_DIR)/fat16.seed.img | dirs
	cp $< $@

$(TINYCC_CONFIG_STAMP): tools/build_tinycc.sh | dirs
	./tools/build_tinycc.sh $(CURDIR) $(TINYCC_DIR) $(CURDIR)/third_party/tinycc
	touch $@

reset-disk: $(BIN_DIR)/fat16.seed.img | dirs
	cp $< $(STATE_DIR)/fat16.img

tinycc-host: $(TINYCC_CONFIG_STAMP)

tinycc-host-clean:
	rm -rf $(TINYCC_DIR)

tinycc-smalos: $(TINYCC_SMALOS_BIN)

$(GEN_DIR)/loader2.gen.asm: $(BOOT_DIR)/loader2.asm | dirs
	sed \
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

boot-layout-check: $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(GEN_DIR)/loader2.gen.asm
	$(PYTHON3) tools/verify_boot_layout.py \
		--boot-asm $(BOOT_DIR)/boot.asm \
		--loader2-asm $(BOOT_DIR)/loader2.asm \
		--memory-h $(KERNEL_DIR)/memory.h \
		--boot-bin $(BIN_DIR)/boot.bin \
		--loader2-bin $(BIN_DIR)/loader2.bin \
		--loader2-gen $(GEN_DIR)/loader2.gen.asm

#
# Final disk image
#
# Layout:
#   boot.bin             ($(BOOT_SECTOR_SIZE) bytes,   LBA 0)
#   loader2.bin          ($(LOADER2_SIZE_BYTES) bytes, LBA 1-$(shell echo $$(( $(LOADER2_SIZE_BYTES) / $(BOOT_SECTOR_SIZE) ))))
#   kernel_padded.bin    (sector-aligned, immediately after loader2.bin)
#   .state/fat16.img     mutable FAT16 partition copy, after the padded kernel
#
# Sector 0 is now an MBR-style boot sector with partition table entries for
# the kernel image and FAT16 partition, so stage 2 and the kernel can
# discover disk locations directly from the image itself.
#
$(IMG_DIR)/os-image.bin: boot-layout-check $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(STATE_DIR)/fat16.img $(TOOLS_DIR)/mkimage | dirs
	$(TOOLS_DIR)/mkimage \
		--boot $(BIN_DIR)/boot.bin \
		--loader $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fat16 $(STATE_DIR)/fat16.img \
		--out $@ \
		--sector-size $(BOOT_SECTOR_SIZE) \
		--loader-size $(LOADER2_SIZE_BYTES) \
		--boot-partition-table-offset $(BOOT_PARTITION_TABLE_OFFSET) \
		--boot-partition-entry-size $(BOOT_PARTITION_ENTRY_SIZE)

image-layout-check: $(IMG_DIR)/os-image.bin
	$(PYTHON3) tools/verify_image_layout.py \
		--image $(IMG_DIR)/os-image.bin \
		--boot $(BIN_DIR)/boot.bin \
		--loader2 $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fat16 $(STATE_DIR)/fat16.img \
		--sector-size $(BOOT_SECTOR_SIZE) \
		--boot-partition-table-offset $(BOOT_PARTITION_TABLE_OFFSET) \
		--boot-partition-entry-size $(BOOT_PARTITION_ENTRY_SIZE)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/*/*.d)

QEMU=qemu-system-i386
SERIAL_LOG=/tmp/smallos-serial.log
MONITOR_SOCK=/tmp/smallos-monitor.sock
PIDFILE=/tmp/smallos.pid
SMOKE_TIMEOUT=120.0
PYTHON3=python3
QEMU_NET_MODE?=user
QEMU_NET_IFACE?=tap0
QEMU_NET_MAC?=52:54:00:12:34:56
QEMU_NETFLAGS_USER=-nic user,model=e1000,mac=$(QEMU_NET_MAC)
QEMU_NETFLAGS_TAP=-netdev tap,id=net0,ifname=$(QEMU_NET_IFACE),script=no,downscript=no -device e1000,netdev=net0,mac=$(QEMU_NET_MAC)
QEMU_NETFLAGS=$(if $(filter tap,$(QEMU_NET_MODE)),$(QEMU_NETFLAGS_TAP),$(QEMU_NETFLAGS_USER))
QEMUFLAGS=-drive format=raw,file=$(IMG_DIR)/os-image.bin -boot c -m 32 \
          -serial file:$(SERIAL_LOG) \
          $(QEMU_NETFLAGS)

.PHONY: all dirs run run-tap run-headless run-headless-tap test smoke smoke-reboot smoke-halt clean boot-layout-check image-layout-check verify reset-disk tinycc-host tinycc-host-clean

run: image-layout-check
	$(QEMU) $(QEMUFLAGS) -display curses

run-tap: image-layout-check
	$(MAKE) run QEMU_NET_MODE=tap

run-headless: image-layout-check
	$(QEMU) $(QEMUFLAGS) -display none \
	    -monitor unix:/tmp/smallos-monitor.sock,server,nowait \
	    -daemonize -pidfile /tmp/smallos.pid

run-headless-tap: image-layout-check
	$(MAKE) run-headless QEMU_NET_MODE=tap

test:
	$(MAKE) reset-disk
	$(MAKE) image-layout-check
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless
	$(PYTHON3) tools/qemu_selftest.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout 600

smoke: reset-disk image-layout-check
	$(MAKE) smoke-reboot
	$(MAKE) smoke-halt

smoke-reboot: image-layout-check
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless
	$(PYTHON3) tools/qemu_smoke.py \
		--command reboot \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

smoke-halt: image-layout-check
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless
	$(PYTHON3) tools/qemu_smoke.py \
		--command halt \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

verify:
	$(MAKE) boot-layout-check
	$(MAKE) image-layout-check
	$(MAKE) test
	$(MAKE) smoke

clean:
	rm -rf $(BUILD_DIR)
