ASM=nasm
CC=i686-elf-gcc
LD=i686-elf-ld
OBJCOPY=i686-elf-objcopy
QEMU_IMG?=qemu-img
MAKEFLAGS += --no-print-directory

SRC_DIR=src
BOOT_DIR=$(SRC_DIR)/boot
KERNEL_DIR=$(SRC_DIR)/kernel
DRIVERS_DIR=$(SRC_DIR)/drivers
SHELL_DIR=$(SRC_DIR)/shell
EXEC_DIR=$(SRC_DIR)/exec
USER_DIR=$(SRC_DIR)/user

BUILD_DIR=build
DISPLAY_BACKEND ?= auto
SERIAL_CONSOLE ?= 0
ifneq ($(filter $(DISPLAY_BACKEND),auto vga),$(DISPLAY_BACKEND))
$(error DISPLAY_BACKEND must be one of: auto vga)
endif
ifneq ($(filter $(SERIAL_CONSOLE),0 1),$(SERIAL_CONSOLE))
$(error SERIAL_CONSOLE must be one of: 0 1)
endif
SERIAL_SUFFIX=$(if $(filter 1,$(SERIAL_CONSOLE)),-serial,)
OBJ_DIR=$(BUILD_DIR)/obj/$(DISPLAY_BACKEND)$(SERIAL_SUFFIX)
BIN_ROOT=$(BUILD_DIR)/bin
BIN_DIR=$(BIN_ROOT)/$(DISPLAY_BACKEND)$(SERIAL_SUFFIX)
GEN_DIR=$(BUILD_DIR)/gen/$(DISPLAY_BACKEND)$(SERIAL_SUFFIX)
IMG_DIR=$(BUILD_DIR)/img
ifeq ($(DISPLAY_BACKEND),auto)
IMG_FILE=$(IMG_DIR)$(SERIAL_SUFFIX)/os-image.bin
else
IMG_FILE=$(IMG_DIR)/$(DISPLAY_BACKEND)$(SERIAL_SUFFIX)/os-image.bin
endif
ESXI_VMDK_SIZE ?=
ESXI_VMDK_DIR=$(IMG_DIR)/esxi/$(DISPLAY_BACKEND)$(SERIAL_SUFFIX)
ESXI_RAW_FILE=$(ESXI_VMDK_DIR)/smallos-esxi.raw
ESXI_VMDK_FILE=$(ESXI_VMDK_DIR)/smallos-esxi.vmdk
TOOLS_DIR=$(BUILD_DIR)/tools
TINYCC_DIR=$(BUILD_DIR)/tinycc-host
TINYCC_CONFIG_STAMP=$(TINYCC_DIR)/.configured
TINYCC_SMALOS_OBJ_DIR=$(OBJ_DIR)/tinycc-smalos
TINYCC_SMALOS_OBJ=$(TINYCC_SMALOS_OBJ_DIR)/tcc.o
TINYCC_SMALOS_BIN=$(BIN_DIR)/tcc-smalos.elf
TINYCC_SMALOS_SRC_DIR=$(BUILD_DIR)/tinycc-smalos-src
TINYCC_SMALOS_PATCH_STAMP=$(TINYCC_SMALOS_SRC_DIR)/.smallos-patched
TINYCC_SMALOS_SRC=$(TINYCC_SMALOS_SRC_DIR)/tcc.c
CSERVER_DIR=$(CURDIR)/third_party/cserver
CSERVER_OBJ_DIR=$(OBJ_DIR)/cserver
CSERVER_BIN=$(BIN_DIR)/cserve.elf
THIRD_PARTY_TINYCC_SENTINEL=$(CURDIR)/third_party/tinycc/tcc.c
THIRD_PARTY_CSERVER_SENTINEL=$(CSERVER_DIR)/src/main.c
THIRD_PARTY_FTP_CLIENT_SENTINEL=$(CURDIR)/third_party/ftp_client/include/ftp_client.h
THIRD_PARTY_FTP_SERVER_SENTINEL=$(CURDIR)/third_party/ftp_server/include/ftp_server.h
STATE_DIR=.state
STATE_EXT2_IMG=$(STATE_DIR)/ext2.img
STATE_EXT2_STAMP=$(STATE_DIR)/ext2.img.stamp

BOOT_SECTOR_SIZE := $(shell awk '/^BOOT_SECTOR_SIZE[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
BOOT_SECTOR_MASK := $(shell echo $$(( $(BOOT_SECTOR_SIZE) - 1 )))
KERNEL_OFFSET := 0x1000
LOADER2_SEGMENT := $(shell awk '/^LOADER2_SEGMENT[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_OFFSET := $(shell awk '/^LOADER2_OFFSET[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_LOAD_ADDR := $(shell echo $$(( ( $(LOADER2_SEGMENT) << 4 ) + $(LOADER2_OFFSET) )))
STAGE2_STACK_TOP := 0xFF00
STAGE2_STACK_TOP_PHYS := $(shell printf '0x%X' $$(( $(LOADER2_LOAD_ADDR) + $(STAGE2_STACK_TOP) )))
STAGE2_STACK_TOP_32 := 0x1FF000
EXT2_TOTAL_BLOCKS := $(shell awk '/^#define[[:space:]]+TOTAL_BLOCKS[[:space:]]+/ {print $$3}' tools/mkext2.c)
EXT2_TOTAL_SIZE_MB := $(shell awk '/^#define[[:space:]]+TOTAL_SIZE_MB[[:space:]]+/ {print $$3}' tools/mkext2.c)
BOOT_PARTITION_TABLE_OFFSET := $(shell awk '/^MBR_PARTITION_TABLE_OFFSET[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
BOOT_PARTITION_ENTRY_SIZE := $(shell awk '/^MBR_PARTITION_ENTRY_SIZE[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/boot.asm)
LOADER2_SIZE_BYTES := $(shell awk '/^LOADER2_SIZE_BYTES[[:space:]]+equ/ {print $$3}' $(BOOT_DIR)/loader2.asm)

CPPFLAGS=-I$(GEN_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(SHELL_DIR) -I$(EXEC_DIR) -I$(USER_DIR) -I$(CURDIR)/third_party/ftp_server/include
ifeq ($(DISPLAY_BACKEND),vga)
CPPFLAGS+=-DSMALLOS_FORCE_VGA_BACKEND=1
LOADER2_FORCE_VGA_BACKEND=1
else
LOADER2_FORCE_VGA_BACKEND=0
endif
ifeq ($(SERIAL_CONSOLE),1)
CPPFLAGS+=-DSMALLOS_SERIAL_CONSOLE=1
endif
CFLAGS=-ffreestanding -m32 -fno-pie -fno-stack-protector -nostdlib -nostartfiles -Wa,--noexecstack
KERNEL_CFLAGS ?=
DRIVER_CFLAGS ?=
DISPLAY_DRIVER_CFLAGS ?= -O2
USER_CFLAGS ?= -O2
DEPFLAGS=-MMD -MP
HOST_CC=gcc
HOST_32_LIBGCC := $(firstword $(wildcard /usr/lib/gcc/*/*/32/libgcc.a))
LIBGCC_FILE ?= $(if $(HOST_32_LIBGCC),$(HOST_32_LIBGCC),$(shell $(CC) -print-libgcc-file-name))
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
	$(DRIVERS_DIR)/mouse.c \
	$(SHELL_DIR)/shell.c \
	$(SHELL_DIR)/line_editor.c \
	$(DRIVERS_DIR)/terminal.c \
	$(DRIVERS_DIR)/unicode.c \
	$(DRIVERS_DIR)/display.c \
	$(DRIVERS_DIR)/screen.c \
	$(DRIVERS_DIR)/fb_console.c \
	$(KERNEL_DIR)/system.c \
	$(KERNEL_DIR)/timer.c \
	$(KERNEL_DIR)/klib.c \
	$(KERNEL_DIR)/memory.c \
	$(KERNEL_DIR)/boot_info.c \
	$(KERNEL_DIR)/pmm.c \
	$(KERNEL_DIR)/vfs.c \
	$(KERNEL_DIR)/input.c \
	$(KERNEL_DIR)/socket.c \
	$(KERNEL_DIR)/wait.c \
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
	$(DRIVERS_DIR)/net.c \
	$(DRIVERS_DIR)/dhcp.c \
	$(DRIVERS_DIR)/arp.c \
	$(DRIVERS_DIR)/ipv4.c \
	$(DRIVERS_DIR)/ntp.c \
	$(DRIVERS_DIR)/tcp.c \
	$(DRIVERS_DIR)/ext2.c \
	$(DRIVERS_DIR)/serial.c

USER_PROGS=echo about uptime halt reboot date pwd cat more fsread ls tree touch rm mkdir rmdir cp mv edit bmpview bootsplash diskview gui shell hello ticks args runelf_test readline exec_test waitprobe fileread compiler_demo heapprobe statprobe fileprobe cwdprobe stdioprobe dirprobe errnoprobe badptrprobe fault sleep_test timerfdprobe signalfdprobe connectprobe ptrguard spinwkr pgrpprobe preempt_test crtprobe displayprobe inputprobe pipeprobe dupprobe forkprobe execveprobe envprobe plasma mandel tcpecho sockeof ftpd
USER_SRCS=$(addprefix $(USER_DIR)/,$(addsuffix .c,$(USER_PROGS)))
USER_RUNTIME_SRCS=$(USER_DIR)/user_alloc.c $(USER_DIR)/user_stdio.c $(USER_DIR)/user_posix.c $(USER_DIR)/user_time.c $(USER_DIR)/user_dirent.c $(USER_DIR)/user_crypt.c $(USER_DIR)/setjmp.asm
CSERVER_SRCS=\
	$(CSERVER_DIR)/src/main.c \
	$(CSERVER_DIR)/src/server.c \
	$(CSERVER_DIR)/src/conn.c \
	$(CSERVER_DIR)/src/parser.c \
	$(CSERVER_DIR)/src/router.c \
	$(CSERVER_DIR)/src/static.c \
	$(CSERVER_DIR)/src/response.c \
	$(CSERVER_DIR)/src/log.c \
	$(CSERVER_DIR)/src/config.c \
	$(CSERVER_DIR)/src/util.c
CSERVER_OBJS=$(patsubst $(CSERVER_DIR)/src/%.c,$(CSERVER_OBJ_DIR)/%.o,$(CSERVER_SRCS))
EXT2_BIN_ENTRIES=bin/echo.elf=$(BIN_DIR)/echo.elf bin/about.elf=$(BIN_DIR)/about.elf bin/uptime.elf=$(BIN_DIR)/uptime.elf bin/halt.elf=$(BIN_DIR)/halt.elf bin/reboot.elf=$(BIN_DIR)/reboot.elf bin/date.elf=$(BIN_DIR)/date.elf bin/pwd.elf=$(BIN_DIR)/pwd.elf bin/cat.elf=$(BIN_DIR)/cat.elf bin/more.elf=$(BIN_DIR)/more.elf bin/fsread.elf=$(BIN_DIR)/fsread.elf bin/ls.elf=$(BIN_DIR)/ls.elf bin/tree.elf=$(BIN_DIR)/tree.elf bin/touch.elf=$(BIN_DIR)/touch.elf bin/rm.elf=$(BIN_DIR)/rm.elf bin/mkdir.elf=$(BIN_DIR)/mkdir.elf bin/rmdir.elf=$(BIN_DIR)/rmdir.elf bin/cp.elf=$(BIN_DIR)/cp.elf bin/mv.elf=$(BIN_DIR)/mv.elf bin/edit.elf=$(BIN_DIR)/edit.elf bin/bmpview.elf=$(BIN_DIR)/bmpview.elf bin/bootsplash.elf=$(BIN_DIR)/bootsplash.elf bin/diskview.elf=$(BIN_DIR)/diskview.elf bin/gui.elf=$(BIN_DIR)/gui.elf bin/shell.elf=$(BIN_DIR)/shell.elf
EXT2_DEMO_ENTRIES=usr/bin/hello.elf=$(BIN_DIR)/hello.elf usr/bin/plasma.elf=$(BIN_DIR)/plasma.elf usr/bin/mandel.elf=$(BIN_DIR)/mandel.elf
EXT2_TEST_ENTRIES=usr/libexec/tests/ticks.elf=$(BIN_DIR)/ticks.elf usr/libexec/tests/args.elf=$(BIN_DIR)/args.elf usr/libexec/tests/runelf_test.elf=$(BIN_DIR)/runelf_test.elf usr/libexec/tests/readline.elf=$(BIN_DIR)/readline.elf usr/libexec/tests/exec_test.elf=$(BIN_DIR)/exec_test.elf usr/libexec/tests/waitprobe.elf=$(BIN_DIR)/waitprobe.elf usr/libexec/tests/fileread.elf=$(BIN_DIR)/fileread.elf usr/libexec/tests/compiler_demo.elf=$(BIN_DIR)/compiler_demo.elf usr/libexec/tests/heapprobe.elf=$(BIN_DIR)/heapprobe.elf usr/libexec/tests/statprobe.elf=$(BIN_DIR)/statprobe.elf usr/libexec/tests/fileprobe.elf=$(BIN_DIR)/fileprobe.elf usr/libexec/tests/cwdprobe.elf=$(BIN_DIR)/cwdprobe.elf usr/libexec/tests/stdioprobe.elf=$(BIN_DIR)/stdioprobe.elf usr/libexec/tests/dirprobe.elf=$(BIN_DIR)/dirprobe.elf usr/libexec/tests/errnoprobe.elf=$(BIN_DIR)/errnoprobe.elf usr/libexec/tests/badptrprobe.elf=$(BIN_DIR)/badptrprobe.elf usr/libexec/tests/fault.elf=$(BIN_DIR)/fault.elf usr/libexec/tests/sleep_test.elf=$(BIN_DIR)/sleep_test.elf usr/libexec/tests/timerfdprobe.elf=$(BIN_DIR)/timerfdprobe.elf usr/libexec/tests/signalfdprobe.elf=$(BIN_DIR)/signalfdprobe.elf usr/libexec/tests/connectprobe.elf=$(BIN_DIR)/connectprobe.elf usr/libexec/tests/ptrguard.elf=$(BIN_DIR)/ptrguard.elf usr/libexec/tests/spinwkr.elf=$(BIN_DIR)/spinwkr.elf usr/libexec/tests/pgrpprobe.elf=$(BIN_DIR)/pgrpprobe.elf usr/libexec/tests/preempt_test.elf=$(BIN_DIR)/preempt_test.elf usr/libexec/tests/crtprobe.elf=$(BIN_DIR)/crtprobe.elf usr/libexec/tests/displayprobe.elf=$(BIN_DIR)/displayprobe.elf usr/libexec/tests/inputprobe.elf=$(BIN_DIR)/inputprobe.elf usr/libexec/tests/pipeprobe.elf=$(BIN_DIR)/pipeprobe.elf usr/libexec/tests/dupprobe.elf=$(BIN_DIR)/dupprobe.elf usr/libexec/tests/forkprobe.elf=$(BIN_DIR)/forkprobe.elf usr/libexec/tests/execveprobe.elf=$(BIN_DIR)/execveprobe.elf usr/libexec/tests/envprobe.elf=$(BIN_DIR)/envprobe.elf
EXT2_APP_ENTRIES=$(EXT2_BIN_ENTRIES) $(EXT2_DEMO_ENTRIES) $(EXT2_TEST_ENTRIES)
EXT2_APP_ENTRIES+= usr/sbin/tcpecho.elf=$(BIN_DIR)/tcpecho.elf usr/sbin/sockeof.elf=$(BIN_DIR)/sockeof.elf usr/sbin/ftpd.elf=$(BIN_DIR)/ftpd.elf
EXT2_APP_ENTRIES+= usr/sbin/cserve.elf=$(CSERVER_BIN)
EXT2_EXTRA_DIRS=tmp/ var/log/
EXT2_EXTRA_ENTRIES=usr/bin/tcc.elf=$(TINYCC_SMALOS_BIN) usr/share/examples/tinycc/tccmath.c=$(CURDIR)/samples/tccmath.c usr/share/examples/tinycc/tccagg.c=$(CURDIR)/samples/tccagg.c usr/share/examples/tinycc/tcctree.c=$(CURDIR)/samples/tcctree.c usr/share/examples/tinycc/tccmini.c=$(CURDIR)/samples/tccmini.c etc/cserve.ini=$(CURDIR)/samples/cserve.ini var/www/index.html=$(CURDIR)/samples/cserve_index.html var/log/boot.log=$(CURDIR)/samples/boot.log boot/splash.bmp=$(CURDIR)/assets/boot_splash.bmp
EXT2_EXTRA_FILES=$(foreach entry,$(EXT2_EXTRA_ENTRIES),$(word 2,$(subst =, ,$(entry))))

KERNEL_OBJS=$(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASM_SRCS)) \
            $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(KERNEL_C_SRCS))

USER_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(USER_SRCS))
GUI_OBJS=$(OBJ_DIR)/user/gui/app.o $(OBJ_DIR)/user/gui/shell_window.o
USER_SHELL_OBJS=$(OBJ_DIR)/user/shell/app.o
USER_RUNTIME_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.c,$(USER_RUNTIME_SRCS))) \
                 $(patsubst $(USER_DIR)/%.asm,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.asm,$(USER_RUNTIME_SRCS)))
USER_CRT0_OBJ=$(OBJ_DIR)/user/user_crt0.o
USER_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_PROGS)))

OBJ_SUBDIRS=$(sort \
	$(dir $(KERNEL_OBJS)) \
	$(dir $(USER_OBJS)) \
	$(dir $(GUI_OBJS)) \
	$(dir $(USER_SHELL_OBJS)) \
)

BUILD_SUBDIRS=$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(GEN_DIR) $(IMG_DIR) $(dir $(IMG_FILE)) $(ESXI_VMDK_DIR) $(TOOLS_DIR) $(OBJ_SUBDIRS) $(STATE_DIR)
BUILD_SUBDIRS+=$(TINYCC_SMALOS_OBJ_DIR) $(CSERVER_OBJ_DIR)

all: check-third-party $(IMG_FILE)

dirs:
	mkdir -p $(BUILD_SUBDIRS)

deps:
	git submodule update --init --recursive

check-third-party:
	@if [ ! -f "$(THIRD_PARTY_TINYCC_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_CSERVER_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_FTP_CLIENT_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_FTP_SERVER_SENTINEL)" ]; then \
		echo "Missing third-party dependencies."; \
		echo "Run: git submodule update --init --recursive"; \
		echo "Or clone with: git clone --recurse-submodules <repo-url>"; \
		exit 1; \
	fi

$(OBJ_DIR)/boot/%.o: $(BOOT_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/%.o: $(KERNEL_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/drivers/%.o: $(DRIVERS_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DRIVER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/drivers/display.o \
$(OBJ_DIR)/drivers/fb_console.o \
$(OBJ_DIR)/drivers/screen.o \
$(OBJ_DIR)/drivers/terminal.o: DRIVER_CFLAGS += $(DISPLAY_DRIVER_CFLAGS)

$(OBJ_DIR)/shell/%.o: $(SHELL_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/exec/%.o: $(EXEC_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/%.o: $(USER_DIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/gui/%.o: $(USER_DIR)/gui/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/shell/%.o: $(USER_DIR)/shell/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/%.o: $(USER_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(TINYCC_SMALOS_PATCH_STAMP): check-third-party patches/tinycc/smallos.patch | dirs
	rm -rf $(TINYCC_SMALOS_SRC_DIR)
	mkdir -p $(TINYCC_SMALOS_SRC_DIR)
	(cd $(CURDIR)/third_party/tinycc && tar cf - --exclude=.git .) | (cd $(TINYCC_SMALOS_SRC_DIR) && tar xf -)
	patch -d $(TINYCC_SMALOS_SRC_DIR) -p1 < patches/tinycc/smallos.patch
	touch $@

$(TINYCC_SMALOS_OBJ): $(TINYCC_SMALOS_PATCH_STAMP) $(TINYCC_CONFIG_STAMP) | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) -Os -ffunction-sections -fdata-sections -I$(TINYCC_DIR) -I$(TINYCC_SMALOS_SRC_DIR) -DTCC_TARGET_I386 -DTCC_TARGET_SMALLOS -c $(TINYCC_SMALOS_SRC) -o $@

$(TINYCC_SMALOS_BIN): $(TINYCC_SMALOS_OBJ) $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) -s --gc-sections $^ $(LIBGCC_FILE) -o $@

$(CSERVER_OBJ_DIR)/%.o: $(CSERVER_DIR)/src/%.c check-third-party | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -std=c2x -D_GNU_SOURCE -I$(CSERVER_DIR)/include -c $< -o $@

$(CSERVER_BIN): $(CSERVER_OBJS) $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC_FILE) -o $@

$(BIN_DIR)/crtprobe.elf: $(OBJ_DIR)/user/crtprobe.o $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/pipeprobe.elf: $(OBJ_DIR)/user/pipeprobe.o $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/dupprobe.elf: $(OBJ_DIR)/user/dupprobe.o $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/forkprobe.elf: $(OBJ_DIR)/user/forkprobe.o $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/execveprobe.elf: $(OBJ_DIR)/user/execveprobe.o $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/envprobe.elf: $(OBJ_DIR)/user/envprobe.o $(USER_CRT0_OBJ) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/bmpview.elf: $(OBJ_DIR)/user/bmpview.o $(OBJ_DIR)/user/image_bmp.o $(OBJ_DIR)/user/gfx.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/bootsplash.elf: $(OBJ_DIR)/user/bootsplash.o $(OBJ_DIR)/user/image_bmp.o $(OBJ_DIR)/user/gfx.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/diskview.elf: $(OBJ_DIR)/user/diskview.o $(OBJ_DIR)/user/gfx.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/gui.elf: $(OBJ_DIR)/user/gui.o $(GUI_OBJS) $(OBJ_DIR)/user/gfx.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/shell.elf: $(OBJ_DIR)/user/shell.o $(USER_SHELL_OBJS) $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/plasma.elf: $(OBJ_DIR)/user/plasma.o $(OBJ_DIR)/user/gfx.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/mandel.elf: $(OBJ_DIR)/user/mandel.o $(OBJ_DIR)/user/gfx.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(BIN_DIR)/kernel.elf: $(KERNEL_OBJS) linker.ld | dirs
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BIN_DIR)/kernel.bin: $(BIN_DIR)/kernel.elf | dirs
	$(OBJCOPY) -O binary $< $@

$(BIN_DIR)/%.elf: $(OBJ_DIR)/user/%.o $(USER_RUNTIME_OBJS) | dirs
	$(LD) $(USER_LDFLAGS) $^ -o $@

$(TOOLS_DIR)/mkext2: tools/mkext2.c | dirs
	$(HOST_CC) -o $@ $<

$(TOOLS_DIR)/mkimage: tools/mkimage.c | dirs
	$(HOST_CC) -o $@ $<

# ext2 seed image (generated from the current tree)
#
$(BIN_DIR)/ext2.seed.img: $(USER_ELFS) $(CSERVER_BIN) $(TOOLS_DIR)/mkext2 $(EXT2_EXTRA_FILES) Makefile | dirs
	$(TOOLS_DIR)/mkext2 $@ $(EXT2_APP_ENTRIES) $(EXT2_EXTRA_ENTRIES) $(EXT2_EXTRA_DIRS)

$(STATE_EXT2_STAMP): $(BIN_DIR)/ext2.seed.img | dirs
	cp $< $(STATE_EXT2_IMG)
	touch $@

$(STATE_EXT2_IMG): $(STATE_EXT2_STAMP) | dirs
	@if [ ! -f $@ ]; then cp $(BIN_DIR)/ext2.seed.img $@; fi

$(TINYCC_CONFIG_STAMP): check-third-party tools/build_tinycc.sh | dirs
	./tools/build_tinycc.sh $(CURDIR) $(TINYCC_DIR) $(CURDIR)/third_party/tinycc
	touch $@

reset-disk: $(BIN_DIR)/ext2.seed.img | dirs
	cp $< $(STATE_EXT2_IMG)
	touch $(STATE_EXT2_STAMP)

tinycc-host: $(TINYCC_CONFIG_STAMP)

tinycc-host-clean:
	rm -rf $(TINYCC_DIR)

tinycc-smalos: $(TINYCC_SMALOS_BIN)

$(GEN_DIR)/loader2.gen.asm: $(BOOT_DIR)/loader2.asm | dirs
	sed \
		-e "s/__STAGE2_STACK_TOP__/$(STAGE2_STACK_TOP)/" \
		-e "s/__STAGE2_STACK_TOP_32__/$(STAGE2_STACK_TOP_32)/" \
		-e "s/__FORCE_VGA_BACKEND__/$(LOADER2_FORCE_VGA_BACKEND)/" \
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
#   .state/ext2.img      mutable ext2 partition copy, after the padded kernel
#
# Sector 0 is now an MBR-style boot sector with partition table entries for
# the kernel image and ext2 partition, so stage 2 and the kernel can
# discover disk locations directly from the image itself.
#
$(IMG_FILE): boot-layout-check $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(STATE_EXT2_IMG) $(STATE_EXT2_STAMP) $(TOOLS_DIR)/mkimage | dirs
	$(TOOLS_DIR)/mkimage \
		--boot $(BIN_DIR)/boot.bin \
		--loader $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fs $(STATE_EXT2_IMG) \
		--out $@ \
		--sector-size $(BOOT_SECTOR_SIZE) \
		--loader-size $(LOADER2_SIZE_BYTES) \
		--boot-partition-table-offset $(BOOT_PARTITION_TABLE_OFFSET) \
		--boot-partition-entry-size $(BOOT_PARTITION_ENTRY_SIZE)

image-layout-check: $(IMG_FILE)
	$(PYTHON3) tools/verify_image_layout.py \
		--image $(IMG_FILE) \
		--boot $(BIN_DIR)/boot.bin \
		--loader2 $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fs $(STATE_DIR)/ext2.img \
		--sector-size $(BOOT_SECTOR_SIZE) \
		--loader-size $(LOADER2_SIZE_BYTES) \
		--boot-partition-table-offset $(BOOT_PARTITION_TABLE_OFFSET) \
		--boot-partition-entry-size $(BOOT_PARTITION_ENTRY_SIZE)

qemu-image:
	$(MAKE) image-layout-check SERIAL_CONSOLE=0 DISPLAY_BACKEND=$(DISPLAY_BACKEND)

esxi-vmdk: qemu-image
	$(MAKE) esxi-vmdk-build SERIAL_CONSOLE=1 DISPLAY_BACKEND=$(DISPLAY_BACKEND)

esxi-vmdk-build: $(ESXI_VMDK_FILE)

$(ESXI_VMDK_FILE): image-layout-check | dirs
	cp $(IMG_FILE) $(ESXI_RAW_FILE)
	@if [ -n "$(ESXI_VMDK_SIZE)" ]; then \
		truncate -s "$(ESXI_VMDK_SIZE)" "$(ESXI_RAW_FILE)"; \
	fi
	$(QEMU_IMG) convert -f raw -O vmdk -o adapter_type=ide,subformat=monolithicSparse $(ESXI_RAW_FILE) $@
	@printf 'ESXi VMDK: %s\n' "$@"

esxi-deploy:
	tools/deploy_esxi.sh $(ESXI_DEPLOY_FLAGS)

esxi-serial-log:
	tools/esxi_serial_log.sh $(ESXI_SERIAL_FLAGS)

esxi-smoke:
	tools/esxi_smoke.sh $(ESXI_SMOKE_FLAGS)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/*/*.d)
-include $(wildcard $(OBJ_DIR)/*/*/*.d)

QEMU=qemu-system-i386
SERIAL_LOG=/tmp/smallos-serial.log
MONITOR_SOCK=/tmp/smallos-monitor.sock
PIDFILE=/tmp/smallos.pid
SMOKE_TIMEOUT=120.0
CSERVE_SMOKE_PORT?=8080
CSERVE_SMOKE_CLIENTS?=32
SOCKET_PARALLEL_PORT?=2323
SOCKET_PARALLEL_CLIENTS?=8
SOCKET_PARALLEL_ROUNDS?=3
FTP_LOOP_ITERATIONS?=5
PYTHON3=python3
TEST_SETUP_LOG=$(BUILD_DIR)/test-setup.log
QEMU_SELFTEST_FLAGS?=--summary
QEMU_NET_MODE?=user
QEMU_NET_IFACE?=tap0
QEMU_NET_MAC?=52:54:00:12:34:56
QEMU_MEMORY_MB?=32
QEMU_DISPLAY?=curses
QEMU_HEADLESS_DISPLAY?=none
SMOKE_DIR=$(BUILD_DIR)/smoke
FRAMEBUFFER_SMOKE_PPM=$(SMOKE_DIR)/framebuffer.ppm
VGA_SMOKE_PPM=$(SMOKE_DIR)/vga.ppm
DISPLAY_SMOKE_QEMU_DISPLAY?=vnc=unix:/tmp/smallos-vnc.sock
DISPLAY_SMOKE_VNC_SOCK=/tmp/smallos-vnc.sock
QEMU_NET_HOSTFWD?=
QEMU_NET_GUESTFWD?=
QEMU_NETFLAGS_USER=-nic user,model=e1000,mac=$(QEMU_NET_MAC)$(QEMU_NET_HOSTFWD)$(QEMU_NET_GUESTFWD)
QEMU_NETFLAGS_TAP=-netdev tap,id=net0,ifname=$(QEMU_NET_IFACE),script=no,downscript=no -device e1000,netdev=net0,mac=$(QEMU_NET_MAC)
QEMU_NETFLAGS=$(if $(filter tap,$(QEMU_NET_MODE)),$(QEMU_NETFLAGS_TAP),$(QEMU_NETFLAGS_USER))
QEMUFLAGS=-drive format=raw,file=$(IMG_FILE) -boot c -m $(QEMU_MEMORY_MB) \
          -serial file:$(SERIAL_LOG) \
          $(QEMU_NETFLAGS)

.PHONY: all dirs deps check-third-party run run-gtk run-sdl run-tap run-headless run-headless-tap test framebuffer-smoke vga-smoke display-smoke display-smoke-one socket-eof-smoke socket-parallel-smoke ftp-smoke ftp-loop-smoke cserve-smoke smoke smoke-reboot smoke-halt clean boot-layout-check image-layout-check qemu-image esxi-vmdk esxi-vmdk-build esxi-deploy esxi-serial-log esxi-smoke verify verify-display verify-network verify-full reset-disk tinycc-host tinycc-host-clean

run: image-layout-check
	$(QEMU) $(QEMUFLAGS) -display $(QEMU_DISPLAY)

run-gtk:
	$(MAKE) run QEMU_DISPLAY=gtk

run-sdl:
	$(MAKE) run QEMU_DISPLAY=sdl

run-tap: image-layout-check
	$(MAKE) run QEMU_NET_MODE=tap

run-headless: image-layout-check
	$(QEMU) $(QEMUFLAGS) -display $(QEMU_HEADLESS_DISPLAY) \
	    -monitor unix:/tmp/smallos-monitor.sock,server,nowait \
	    -daemonize -pidfile /tmp/smallos.pid

run-headless-tap: image-layout-check
	$(MAKE) run-headless QEMU_NET_MODE=tap

test:
	@mkdir -p $(BUILD_DIR)
	@printf '%-38s ' '[test] reset disk'
	@if $(MAKE) --silent reset-disk SERIAL_CONSOLE=1 >$(TEST_SETUP_LOG) 2>&1; then \
		printf 'PASS\n'; \
	else \
		printf 'FAIL\n'; \
		cat $(TEST_SETUP_LOG); \
		exit 1; \
	fi
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	@rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	@printf '%-38s ' '[test] boot qemu'
	@if $(MAKE) --silent run-headless SERIAL_CONSOLE=1 >>$(TEST_SETUP_LOG) 2>&1; then \
		printf 'PASS\n'; \
	else \
		printf 'FAIL\n'; \
		cat $(TEST_SETUP_LOG); \
		exit 1; \
	fi
	@$(PYTHON3) tools/qemu_selftest.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout 600 \
		$(QEMU_SELFTEST_FLAGS)

ftp-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1 QEMU_NET_HOSTFWD=',hostfwd=tcp::2121-:2121,hostfwd=tcp::30000-:30000'
	$(PYTHON3) tools/ftp_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout 120

socket-eof-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1 QEMU_NET_HOSTFWD=',hostfwd=tcp::2463-:2463'
	$(PYTHON3) tools/socket_eof_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout 120

socket-parallel-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1 QEMU_NET_HOSTFWD=',hostfwd=tcp::$(SOCKET_PARALLEL_PORT)-:2323'
	$(PYTHON3) tools/socket_parallel_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--port $(SOCKET_PARALLEL_PORT) \
		--clients $(SOCKET_PARALLEL_CLIENTS) \
		--rounds $(SOCKET_PARALLEL_ROUNDS) \
		--timeout 120

ftp-loop-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1 QEMU_NET_HOSTFWD=',hostfwd=tcp::2121-:2121,hostfwd=tcp::30000-:30000'
	$(PYTHON3) tools/ftp_loop_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--iterations $(FTP_LOOP_ITERATIONS) \
		--timeout 120

cserve-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1 QEMU_NET_HOSTFWD=',hostfwd=tcp::$(CSERVE_SMOKE_PORT)-:8080'
	$(PYTHON3) tools/cserve_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--port $(CSERVE_SMOKE_PORT) \
		--clients $(CSERVE_SMOKE_CLIENTS) \
		--timeout 120

framebuffer-smoke:
	$(MAKE) display-smoke-one DISPLAY_BACKEND=auto DISPLAY_SMOKE_MODE=framebuffer DISPLAY_SMOKE_PPM=$(FRAMEBUFFER_SMOKE_PPM)

vga-smoke:
	$(MAKE) display-smoke-one DISPLAY_BACKEND=vga DISPLAY_SMOKE_MODE=vga DISPLAY_SMOKE_PPM=$(VGA_SMOKE_PPM)

display-smoke-one:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1 DISPLAY_BACKEND=$(DISPLAY_BACKEND)
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE) $(DISPLAY_SMOKE_VNC_SOCK) $(DISPLAY_SMOKE_PPM)
	mkdir -p $(SMOKE_DIR)
	$(MAKE) run-headless SERIAL_CONSOLE=1 DISPLAY_BACKEND=$(DISPLAY_BACKEND) QEMU_HEADLESS_DISPLAY=$(DISPLAY_SMOKE_QEMU_DISPLAY)
	$(PYTHON3) tools/display_smoke.py \
		--mode $(DISPLAY_SMOKE_MODE) \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--screenshot $(DISPLAY_SMOKE_PPM) \
		--timeout $(SMOKE_TIMEOUT)

display-smoke: framebuffer-smoke vga-smoke

smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	$(MAKE) smoke-reboot
	$(MAKE) smoke-halt

smoke-reboot:
	$(MAKE) image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1
	$(PYTHON3) tools/qemu_smoke.py \
		--command reboot \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

smoke-halt:
	$(MAKE) image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1
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

verify-display:
	$(MAKE) display-smoke

verify-network:
	$(MAKE) socket-eof-smoke
	$(MAKE) socket-parallel-smoke
	$(MAKE) ftp-smoke
	$(MAKE) ftp-loop-smoke
	$(MAKE) cserve-smoke

verify-full:
	$(MAKE) verify
	$(MAKE) verify-display
	$(MAKE) verify-network

clean:
	rm -rf $(BUILD_DIR)
