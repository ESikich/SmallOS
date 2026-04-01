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

CFLAGS=-ffreestanding -m32 -fno-pie -fno-stack-protector -nostdlib -nostartfiles \
	-I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(SHELL_DIR) -I$(EXEC_DIR) -I$(USER_DIR)
HOST_CC=gcc
LDFLAGS=-T linker.ld -m elf_i386

KERNEL_OBJS=\
	$(OBJ_DIR)/kernel_entry.o \
	$(OBJ_DIR)/interrupts.o \
	$(OBJ_DIR)/setjmp.o \
	$(OBJ_DIR)/sched_switch.o \
	$(OBJ_DIR)/kernel.o \
	$(OBJ_DIR)/idt.o \
	$(OBJ_DIR)/keyboard.o \
	$(OBJ_DIR)/shell.o \
	$(OBJ_DIR)/line_editor.o \
	$(OBJ_DIR)/terminal.o \
	$(OBJ_DIR)/screen.o \
	$(OBJ_DIR)/system.o \
	$(OBJ_DIR)/timer.o \
	$(OBJ_DIR)/memory.o \
	$(OBJ_DIR)/pmm.o \
	$(OBJ_DIR)/process.o \
	$(OBJ_DIR)/scheduler.o \
	$(OBJ_DIR)/parse.o \
	$(OBJ_DIR)/commands.o \
	$(OBJ_DIR)/programs.o \
	$(OBJ_DIR)/exec.o \
	$(OBJ_DIR)/images.o \
	$(OBJ_DIR)/image_programs.o \
	$(OBJ_DIR)/elf_loader.o \
	$(OBJ_DIR)/syscall.o \
	$(OBJ_DIR)/gdt.o \
	$(OBJ_DIR)/paging.o \
	$(OBJ_DIR)/ramdisk.o

USER_PROGS=hello ticks args runelf_test readline exec_test

USER_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_PROGS)))
USER_OBJS=$(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(USER_PROGS)))
RAMDISK_ENTRIES=$(foreach p,$(USER_PROGS),$(p):$(BIN_DIR)/$(p).elf)

all: $(IMG_DIR)/os-image.bin

dirs:
	mkdir -p $(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(GEN_DIR) $(IMG_DIR) $(TOOLS_DIR)

$(OBJ_DIR)/setjmp.o: $(KERNEL_DIR)/setjmp.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/sched_switch.o: $(KERNEL_DIR)/sched_switch.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel_entry.o: $(BOOT_DIR)/kernel_entry.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/interrupts.o: $(KERNEL_DIR)/interrupts.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel.o: $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/memory.h $(KERNEL_DIR)/scheduler.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/idt.o: $(KERNEL_DIR)/idt.c $(KERNEL_DIR)/idt.h $(KERNEL_DIR)/ports.h $(DRIVERS_DIR)/keyboard.h $(KERNEL_DIR)/scheduler.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/keyboard.o: $(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/keyboard.h $(KERNEL_DIR)/ports.h $(SHELL_DIR)/shell.h $(DRIVERS_DIR)/screen.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/shell.o: $(SHELL_DIR)/shell.c $(SHELL_DIR)/shell.h $(DRIVERS_DIR)/screen.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/screen.o: $(DRIVERS_DIR)/screen.c $(DRIVERS_DIR)/screen.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/terminal.o: $(DRIVERS_DIR)/terminal.c $(DRIVERS_DIR)/terminal.h $(DRIVERS_DIR)/screen.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/line_editor.o: $(SHELL_DIR)/line_editor.c $(SHELL_DIR)/line_editor.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/system.o: $(KERNEL_DIR)/system.c $(KERNEL_DIR)/system.h $(KERNEL_DIR)/ports.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/timer.o: $(KERNEL_DIR)/timer.c $(KERNEL_DIR)/timer.h $(KERNEL_DIR)/ports.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/memory.o: $(KERNEL_DIR)/memory.c $(KERNEL_DIR)/memory.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/pmm.o: $(KERNEL_DIR)/pmm.c $(KERNEL_DIR)/pmm.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/process.o: $(KERNEL_DIR)/process.c $(KERNEL_DIR)/process.h $(KERNEL_DIR)/paging.h $(KERNEL_DIR)/pmm.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/scheduler.o: $(KERNEL_DIR)/scheduler.c $(KERNEL_DIR)/scheduler.h $(KERNEL_DIR)/process.h $(KERNEL_DIR)/paging.h $(KERNEL_DIR)/gdt.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/parse.o: $(SHELL_DIR)/parse.c $(SHELL_DIR)/parse.h $(KERNEL_DIR)/memory.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/commands.o: $(SHELL_DIR)/commands.c $(SHELL_DIR)/commands.h $(SHELL_DIR)/parse.h $(DRIVERS_DIR)/terminal.h $(KERNEL_DIR)/system.h $(KERNEL_DIR)/timer.h $(KERNEL_DIR)/memory.h $(KERNEL_DIR)/pmm.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/programs.o: $(EXEC_DIR)/programs.c $(EXEC_DIR)/programs.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/exec.o: $(EXEC_DIR)/exec.c $(EXEC_DIR)/exec.h $(KERNEL_DIR)/memory.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/images.o: $(EXEC_DIR)/images.c $(EXEC_DIR)/images.h $(EXEC_DIR)/exec.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/image_programs.o: $(EXEC_DIR)/image_programs.c $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/elf_loader.o: $(EXEC_DIR)/elf_loader.c $(EXEC_DIR)/elf_loader.h $(KERNEL_DIR)/elf.h $(KERNEL_DIR)/paging.h $(KERNEL_DIR)/process.h $(KERNEL_DIR)/scheduler.h $(KERNEL_DIR)/memory.h $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/ramdisk.h $(DRIVERS_DIR)/terminal.h $(DRIVERS_DIR)/keyboard.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/syscall.o: $(KERNEL_DIR)/syscall.c $(KERNEL_DIR)/syscall.h $(DRIVERS_DIR)/terminal.h $(KERNEL_DIR)/timer.h $(KERNEL_DIR)/uapi_syscall.h $(DRIVERS_DIR)/keyboard.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/gdt.o: $(KERNEL_DIR)/gdt.c $(KERNEL_DIR)/gdt.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/paging.o: $(KERNEL_DIR)/paging.c $(KERNEL_DIR)/paging.h $(KERNEL_DIR)/memory.h $(KERNEL_DIR)/pmm.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/ramdisk.o: $(KERNEL_DIR)/ramdisk.c $(KERNEL_DIR)/ramdisk.h $(DRIVERS_DIR)/terminal.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/kernel.elf: $(KERNEL_OBJS) linker.ld | dirs
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BIN_DIR)/kernel.bin: $(BIN_DIR)/kernel.elf | dirs
	$(OBJCOPY) -O binary $< $@

$(OBJ_DIR)/%.o: $(USER_DIR)/%.c $(USER_DIR)/user_lib.h $(USER_DIR)/user_syscall.h $(KERNEL_DIR)/uapi_syscall.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/%.elf: $(OBJ_DIR)/%.o | dirs
	$(LD) -m elf_i386 -Ttext-segment 0x400000 -e _start $< -o $@

$(TOOLS_DIR)/mkramdisk: tools/mkramdisk.c | dirs
	$(HOST_CC) -o $@ $<

$(BIN_DIR)/ramdisk.rd: $(USER_ELFS) $(TOOLS_DIR)/mkramdisk | dirs
	$(TOOLS_DIR)/mkramdisk $@ $(RAMDISK_ENTRIES)

$(GEN_DIR)/loader2.gen.asm: $(BOOT_DIR)/loader2.asm $(BIN_DIR)/kernel.bin $(BIN_DIR)/ramdisk.rd | dirs
	@kernel_sectors=$$(( ($$(wc -c < $(BIN_DIR)/kernel.bin) + 511) / 512 )); \
	ramdisk_sectors=$$(( ($$(wc -c < $(BIN_DIR)/ramdisk.rd) + 511) / 512 )); \
	ramdisk_lba=$$(( 5 + $$kernel_sectors )); \
	echo "kernel:  $$(wc -c < $(BIN_DIR)/kernel.bin) bytes ($$kernel_sectors sectors, LBA 5)"; \
	echo "ramdisk: $$(wc -c < $(BIN_DIR)/ramdisk.rd) bytes ($$ramdisk_sectors sectors, LBA $$ramdisk_lba)"; \
	sed \
		-e "s/__KERNEL_SECTORS__/$$kernel_sectors/" \
		-e "s/__RAMDISK_SECTORS__/$$ramdisk_sectors/" \
		-e "s/__RAMDISK_LBA__/$$ramdisk_lba/" \
		$< > $@

$(BIN_DIR)/loader2.bin: $(GEN_DIR)/loader2.gen.asm | dirs
	$(ASM) -f bin $< -o $@
	@size=$$(wc -c < $@); \
	if [ $$size -ne 2048 ]; then \
		echo "ERROR: loader2.bin must be 2048 bytes, got $$size"; \
		exit 1; \
	fi

$(BIN_DIR)/boot.bin: $(BOOT_DIR)/boot.asm | dirs
	$(ASM) -f bin $< -o $@

$(IMG_DIR)/os-image.bin: $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(BIN_DIR)/ramdisk.rd | dirs
	@kernel_size=$$(wc -c < $(BIN_DIR)/kernel.bin); \
	padded=$$(( ($$kernel_size + 511) & ~511 )); \
	pad=$$(( $$padded - $$kernel_size )); \
	cp $(BIN_DIR)/kernel.bin $(BIN_DIR)/kernel_padded.bin; \
	dd if=/dev/zero bs=1 count=$$pad >> $(BIN_DIR)/kernel_padded.bin 2>/dev/null; \
	cat $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel_padded.bin $(BIN_DIR)/ramdisk.rd > $@

clean:
	rm -rf $(BUILD_DIR)
