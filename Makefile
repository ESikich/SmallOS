ASM=nasm
CC=i686-elf-gcc
LD=i686-elf-ld
AR?=$(if $(shell command -v i686-elf-ar 2>/dev/null),i686-elf-ar,ar)
OBJCOPY=i686-elf-objcopy
QEMU_IMG?=qemu-img
MAKEFLAGS += --no-print-directory

SRC_DIR=src
BOOT_DIR=$(SRC_DIR)/boot
KERNEL_DIR=$(SRC_DIR)/kernel
DRIVERS_DIR=$(SRC_DIR)/drivers
EXEC_DIR=$(SRC_DIR)/exec
USER_DIR=$(SRC_DIR)/user
USER_INCLUDE_DIR=$(USER_DIR)/include
USER_INTERNAL_DIR=$(USER_DIR)/internal

BUILD_DIR=build
DISPLAY_BACKEND ?= auto
SERIAL_CONSOLE ?= 1
NIC_DRIVER ?= e1000
BOOT_FORCE_CHS ?= 0
BOOT_VBE_DIAG ?= 1
BOOT_VBE_RELAXED ?= 0
BOOT_RAMDISK_FALLBACK ?= never
BOOT_SKIP_USB_STORAGE ?= 0
ifneq ($(filter $(DISPLAY_BACKEND),auto vga),$(DISPLAY_BACKEND))
$(error DISPLAY_BACKEND must be one of: auto vga)
endif
ifneq ($(filter $(SERIAL_CONSOLE),0 1),$(SERIAL_CONSOLE))
$(error SERIAL_CONSOLE must be one of: 0 1)
endif
ifneq ($(filter $(NIC_DRIVER),e1000 rtl8139 all none),$(NIC_DRIVER))
$(error NIC_DRIVER must be one of: e1000 rtl8139 all none)
endif
ifneq ($(filter $(BOOT_FORCE_CHS),0 1),$(BOOT_FORCE_CHS))
$(error BOOT_FORCE_CHS must be one of: 0 1)
endif
ifneq ($(filter $(BOOT_VBE_DIAG),0 1),$(BOOT_VBE_DIAG))
$(error BOOT_VBE_DIAG must be one of: 0 1)
endif
ifneq ($(filter $(BOOT_VBE_RELAXED),0 1),$(BOOT_VBE_RELAXED))
$(error BOOT_VBE_RELAXED must be one of: 0 1)
endif
ifneq ($(filter $(BOOT_RAMDISK_FALLBACK),auto always never 0 1),$(BOOT_RAMDISK_FALLBACK))
$(error BOOT_RAMDISK_FALLBACK must be one of: auto always never 0 1)
endif
ifneq ($(filter $(BOOT_SKIP_USB_STORAGE),0 1),$(BOOT_SKIP_USB_STORAGE))
$(error BOOT_SKIP_USB_STORAGE must be one of: 0 1)
endif
LOADER2_RAMDISK_FALLBACK_POLICY=$(if $(filter always 1,$(BOOT_RAMDISK_FALLBACK)),1,$(if $(filter never 0,$(BOOT_RAMDISK_FALLBACK)),0,2))
SERIAL_SUFFIX=$(if $(filter 1,$(SERIAL_CONSOLE)),-serial,)
BUILD_PROFILE=$(DISPLAY_BACKEND)$(SERIAL_SUFFIX)-$(NIC_DRIVER)
OBJ_DIR=$(BUILD_DIR)/obj/$(BUILD_PROFILE)
BIN_ROOT=$(BUILD_DIR)/bin
BIN_DIR=$(BIN_ROOT)/$(BUILD_PROFILE)
GEN_DIR=$(BUILD_DIR)/gen/$(BUILD_PROFILE)
IMG_DIR=$(BUILD_DIR)/img
IMG_FILE=$(IMG_DIR)/smallos.img
USB_IMG_FILE=$(IMG_DIR)/smallos-wyse-s10-direct-usb.img
ESXI_VMDK_SIZE ?=
ESXI_RAW_FILE=$(IMG_DIR)/smallos-vmdk.raw
ESXI_VMDK_FILE=$(IMG_DIR)/smallos.vmdk
TOOLS_DIR=$(BUILD_DIR)/tools
KERNEL_CONFIG_STAMP=$(OBJ_DIR)/kernel.config
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
FRACTINT_SVN_URL=https://svn.fractint.net/tags/fractint-20-04p17
FRACTINT_SVN_REV=1804
FRACTINT_DIR=$(CURDIR)/third_party/fractint
FRACTINT_SVN_STAMP=$(FRACTINT_DIR)/.smallos-svn-r$(FRACTINT_SVN_REV)
FRACTINT_HELP_CC ?= $(HOST_CC) -m32
FRACTINT_PORT_DIR=$(USER_DIR)/ports/fractint
FRACTINT_OBJ_DIR=$(OBJ_DIR)/fractint
WOLF3D_SHAREWARE_URL=https://beta.wolf3d.net/file-download/download/private/4457
WOLF3D_SHAREWARE_ZIP=$(STATE_DIR)/downloads/wolf3d-shareware.zip
WOLF3D_DATA_DIR ?= $(STATE_DIR)/wolf3d
WOLF3D_STAGE_DATA ?= 1
WOLF3D_PALETTE_SRC=$(CURDIR)/third_party/wolf3d/WOLFSRC/WOLF3D.EXE
WOLF3D_PALETTE_H=$(GEN_DIR)/wolf3d_palette.h
WOLF3D_SIGNON_SRC=$(CURDIR)/third_party/wolf3d/WOLFSRC/OBJ/SIGNON.OBJ
WOLF3D_SIGNON_H=$(GEN_DIR)/wolf3d_signon.h
WOLF3D_PORT_DIR=$(USER_DIR)/ports/wolf3d
WOLF3D_PORT_INCLUDE_DIR=$(WOLF3D_PORT_DIR)/include
WOLF3D_SRC_DIR=$(CURDIR)/third_party/wolf3d/WOLFSRC
WOLF3D_SRC_MIRROR=$(GEN_DIR)/wolf3d_src
WOLF3D_SRC_STAMP=$(WOLF3D_SRC_MIRROR)/.stamp
WOLF3D_NATIVE_CPPFLAGS=$(USER_CPPFLAGS) -I$(WOLF3D_PORT_INCLUDE_DIR) -I$(WOLF3D_SRC_MIRROR) -DUPLOAD -include wolf3d_port.h
WOLF3D_UPSTREAM_PROBE_SRCS=ID_CA.C ID_IN.C ID_PM.C ID_US_1.C WL_ACT1.C WL_ACT2.C WL_AGENT.C WL_DEBUG.C WL_DRAW.C WL_GAME.C WL_INTER.C WL_MAIN.C WL_MENU.C WL_PLAY.C WL_SCALE.C WL_STATE.C WL_TEXT.C
WOLF3D_UPSTREAM_PROBE_OBJS=$(addprefix $(OBJ_DIR)/user/ports/wolf3d/upstream/,$(WOLF3D_UPSTREAM_PROBE_SRCS:.C=.o))
WOLF3D_UPSTREAM_EXTRA_COMPILE_SRCS=ID_MM.C ID_SD.C ID_VH.C ID_VL.C MUNGE.C OLDSCALE.C WOLFHACK.C
WOLF3D_UPSTREAM_EXTRA_COMPILE_OBJS=$(addprefix $(OBJ_DIR)/user/ports/wolf3d/upstream/,$(WOLF3D_UPSTREAM_EXTRA_COMPILE_SRCS:.C=.o))
WOLF3D_NATIVE_SHIM_OBJ=$(OBJ_DIR)/user/ports/wolf3d/wolf3d_dos_shim.o
WOLF3D_PLATFORM_SHIM_OBJ=$(OBJ_DIR)/user/ports/wolf3d/wolf3d_platform_shim.o
WOLF3D_CONFIG_SHIM_OBJ=$(OBJ_DIR)/user/ports/wolf3d/wolf3d_config_shim.o
WOLF3D_ASSETS_OBJ=$(OBJ_DIR)/user/ports/wolf3d/wolf3d_assets.o
WOLF3D_SOURCE_PROBE_OBJ=$(OBJ_DIR)/user/ports/wolf3d/wolf3d_source_probe.o
WOLF3D_SOURCE_PROBE_BIN=$(BIN_DIR)/wolf3d-srcprobe.elf
THIRD_PARTY_TINYCC_SENTINEL=$(CURDIR)/third_party/tinycc/tcc.c
THIRD_PARTY_CSERVER_SENTINEL=$(CSERVER_DIR)/src/main.c
THIRD_PARTY_FRACTINT_SENTINEL=$(FRACTINT_SVN_STAMP)
THIRD_PARTY_FTP_CLIENT_SENTINEL=$(CURDIR)/third_party/ftp_client/include/ftp_client.h
THIRD_PARTY_FTP_SERVER_SENTINEL=$(CURDIR)/third_party/ftp_server/include/ftp_server.h
THIRD_PARTY_WOLF3D_SENTINEL=$(CURDIR)/third_party/wolf3d/WOLFSRC/WL_MAIN.C
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

CPPFLAGS=-I$(GEN_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(EXEC_DIR) -I$(CURDIR)/third_party/ftp_server/include
USER_CPPFLAGS=$(CPPFLAGS) -I$(USER_INCLUDE_DIR) -I$(USER_INTERNAL_DIR)
USER_PUBLIC_CPPFLAGS=$(CPPFLAGS) -I$(USER_INCLUDE_DIR)
ifeq ($(DISPLAY_BACKEND),vga)
CPPFLAGS+=-DSMALLOS_FORCE_VGA_BACKEND=1
LOADER2_FORCE_VGA_BACKEND=1
else
LOADER2_FORCE_VGA_BACKEND=0
endif
ifeq ($(SERIAL_CONSOLE),1)
CPPFLAGS+=-DSMALLOS_SERIAL_CONSOLE=1
endif
ifeq ($(BOOT_SKIP_USB_STORAGE),1)
CPPFLAGS+=-DSMALLOS_BOOT_SKIP_USB_STORAGE=1
endif
ifneq ($(filter e1000 all,$(NIC_DRIVER)),)
CPPFLAGS+=-DSMALLOS_NIC_E1000=1
KERNEL_NIC_SRCS+=$(DRIVERS_DIR)/e1000.c
endif
ifneq ($(filter rtl8139 all,$(NIC_DRIVER)),)
CPPFLAGS+=-DSMALLOS_NIC_RTL8139=1
KERNEL_NIC_SRCS+=$(DRIVERS_DIR)/rtl8139.c
endif
CFLAGS=-ffreestanding -m32 -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -nostartfiles -Wa,--noexecstack
KERNEL_CFLAGS ?=
DRIVER_CFLAGS ?=
DISPLAY_DRIVER_CFLAGS ?= -O2
USER_CFLAGS ?= -O2
DEPFLAGS=-MMD -MP
HOST_CC=gcc
LIBGCC_FILE ?= $(shell $(CC) -m32 -print-libgcc-file-name)
LDFLAGS=-T linker.ld -m elf_i386
USER_LDFLAGS=-m elf_i386 -Ttext-segment 0x400000 -e _start --gc-sections
USER_DYN_LDFLAGS=-m elf_i386 -Ttext-segment 0x400000 -e _start --dynamic-linker /lib/ld-smallos.so --hash-style=sysv -z now
USER_PIE_LDFLAGS=-m elf_i386 -pie -e _start --dynamic-linker /lib/ld-smallos.so --hash-style=sysv -z now --gc-sections
LD_SMALLOS_LDFLAGS=-m elf_i386 -shared -e _start --gc-sections --hash-style=sysv -z now -z notext

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
	$(DRIVERS_DIR)/terminal.c \
	$(DRIVERS_DIR)/unicode.c \
	$(DRIVERS_DIR)/display.c \
	$(DRIVERS_DIR)/sound.c \
	$(DRIVERS_DIR)/screen.c \
	$(DRIVERS_DIR)/fb_console.c \
	$(KERNEL_DIR)/system.c \
		$(KERNEL_DIR)/timer.c \
		$(KERNEL_DIR)/cpu.c \
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
	$(EXEC_DIR)/elf_loader.c \
	$(KERNEL_DIR)/syscall.c \
	$(KERNEL_DIR)/gdt.c \
	$(KERNEL_DIR)/paging.c \
	$(DRIVERS_DIR)/ata.c \
	$(DRIVERS_DIR)/block.c \
	$(DRIVERS_DIR)/pci.c \
	$(DRIVERS_DIR)/usb.c \
	$(DRIVERS_DIR)/usb_storage.c \
	$(KERNEL_NIC_SRCS) \
	$(DRIVERS_DIR)/nic.c \
	$(DRIVERS_DIR)/net.c \
	$(DRIVERS_DIR)/dhcp.c \
	$(DRIVERS_DIR)/arp.c \
	$(DRIVERS_DIR)/ipv4.c \
	$(DRIVERS_DIR)/ntp.c \
	$(DRIVERS_DIR)/tcp.c \
	$(DRIVERS_DIR)/ext2.c \
	$(DRIVERS_DIR)/serial.c

USER_PROGS=echo about uptime halt reboot date pwd cat more man fsread ls tree touch rm mkdir rmdir cp mv edit bmpview bootsplash diskview gui shell ip ipconfig meminfo memmap cpuz top netinfo dhcp netsend netrecv arpgw ping pinggw pingpublic netcheck ataread usbinfo usbports usbdiag usbpeek usbpower usbmouse mousetest hello ticks args exec_args readline exec_test waitprobe fileread compiler_demo heapprobe statprobe fileprobe cwdprobe stdioprobe dirprobe errnoprobe badptrprobe fault sleep_test timerfdprobe signalfdprobe connectprobe ptrguard spinwkr pgrpprobe preempt_test crtprobe displayprobe inputprobe pipeprobe dupprobe forkprobe execveprobe envprobe mathprobe soundprobe plasma mandel wolf3d fractint tcpecho sockeof ftpd
USER_PROGS := $(filter-out fractint,$(USER_PROGS))
USER_SRCS=$(addprefix $(USER_DIR)/,$(addsuffix .c,$(USER_PROGS)))
USER_LIBC_SRCS=\
	$(USER_DIR)/libc/malloc.c \
	$(USER_DIR)/libc/assert.c \
	$(USER_DIR)/libc/stdio.c \
	$(USER_DIR)/libc/time.c \
	$(USER_DIR)/libc/stdlib_extra.c \
	$(USER_DIR)/libc/string_bsd.c \
	$(USER_DIR)/libc/stdio_ext.c \
	$(USER_DIR)/libc/stdio_scan.c \
	$(USER_DIR)/libc/crypt.c \
	$(USER_DIR)/term_keys.c \
	$(USER_DIR)/setjmp.asm
USER_LIBM_SRCS=\
	$(USER_DIR)/libm/math.c
USER_POSIX_SRCS=\
	$(USER_DIR)/posix/conio_compat.c \
	$(USER_DIR)/posix/core.c \
	$(USER_DIR)/posix/dos_compat.c \
	$(USER_DIR)/posix/dirent.c \
	$(USER_DIR)/posix/select.c
USER_RUNTIME_SRCS=$(USER_LIBC_SRCS) $(USER_LIBM_SRCS) $(USER_POSIX_SRCS)
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
FRACTINT_COMMON_NAMES=3d ant bigflt biginit bignum bignumc calcfrac cmdfiles decoder editpal encoder evolve f16 fracsubr fractalb fractalp fractals fractint framain2 frasetup gifview hcmplx help history intro jb jiim line3d loadfdos loadfile loadmap lorenz lsys lsysf memory miscfrac miscovl miscres mpmath_c parser parserfp plot3d printer prompts1 prompts2 realdos rotate slideshw soi soi1 stereo targa testpt tgaview zoom
FRACTINT_UNIX_NAMES=calcmand calmanfp diskvidu fpu087 fracsuba general unix
FRACTINT_COMMON_OBJS=$(addprefix $(FRACTINT_OBJ_DIR)/common/,$(addsuffix .o,$(FRACTINT_COMMON_NAMES)))
FRACTINT_UNIX_OBJS=$(addprefix $(FRACTINT_OBJ_DIR)/unix/,$(addsuffix .o,$(FRACTINT_UNIX_NAMES)))
FRACTINT_PORT_OBJS=$(FRACTINT_OBJ_DIR)/port/runtime.o
FRACTINT_OBJS=$(FRACTINT_COMMON_OBJS) $(FRACTINT_UNIX_OBJS) $(FRACTINT_PORT_OBJS)
FRACTINT_CPPFLAGS=$(USER_PUBLIC_CPPFLAGS) -I$(FRACTINT_DIR)/headers
FRACTINT_PORT_CPPFLAGS=$(FRACTINT_CPPFLAGS)
FRACTINT_DEFS=-DXFRACT -DNOBSTRING -DHAVESTRI -DBIG_ANSI_C -DLINUX -DDO_NOT_USE_LONG_DOUBLE -DSRCDIR=\"/usr/share/xfractint\"
FRACTINT_COMPAT_CFLAGS=-Wno-implicit-function-declaration -Wno-incompatible-pointer-types
FRACTINT_DATA_FILES=$(wildcard $(FRACTINT_DIR)/maps/*.map) \
                    $(wildcard $(FRACTINT_DIR)/pars/*.par) \
                    $(wildcard $(FRACTINT_DIR)/formulas/*.frm) \
                    $(wildcard $(FRACTINT_DIR)/lsystem/*.l) \
                    $(wildcard $(FRACTINT_DIR)/ifs/*.ifs)
FRACTINT_DATA_FLAT_ENTRIES=$(foreach file,$(FRACTINT_DATA_FILES),usr/share/xfractint/$(notdir $(file))=$(file))
FRACTINT_DATA_TREE_ENTRIES=$(foreach file,$(FRACTINT_DATA_FILES),usr/share/xfractint/$(patsubst $(FRACTINT_DIR)/%,%,$(file))=$(file))
FRACTINT_DATA_ENTRIES=$(FRACTINT_DATA_FLAT_ENTRIES) $(FRACTINT_DATA_TREE_ENTRIES)
USER_INCLUDE_FILES=$(shell find $(USER_INCLUDE_DIR) -type f | sort)
USER_INCLUDE_ENTRIES=$(foreach file,$(USER_INCLUDE_FILES),usr/include/$(patsubst $(USER_INCLUDE_DIR)/%,%,$(file))=$(CURDIR)/$(file))
USER_UAPI_FILES=$(wildcard $(KERNEL_DIR)/uapi_*.h)
USER_UAPI_ENTRIES=$(foreach file,$(USER_UAPI_FILES),usr/include/$(notdir $(file))=$(CURDIR)/$(file))
EXT2_BIN_PROGS=echo about uptime halt reboot date pwd cat more man fsread ls tree touch rm mkdir rmdir cp mv edit bmpview bootsplash diskview gui shell ip ipconfig meminfo memmap cpuz netinfo dhcp netsend netrecv arpgw ping pinggw pingpublic netcheck ataread usbinfo usbports usbdiag usbpeek usbpower usbmouse mousetest top soundprobe
EXT2_DEMO_PROGS=hello plasma mandel wolf3d fractint
EXT2_TEST_PROGS=ticks args exec_args readline exec_test waitprobe fileread compiler_demo heapprobe statprobe fileprobe cwdprobe stdioprobe dirprobe errnoprobe badptrprobe fault sleep_test timerfdprobe signalfdprobe connectprobe ptrguard spinwkr pgrpprobe preempt_test crtprobe displayprobe inputprobe pipeprobe dupprobe forkprobe execveprobe envprobe mathprobe
USER_DYNAMIC_NO_CRT0=echo about uptime date pwd cat more man fsread ls tree touch rm mkdir rmdir cp mv meminfo memmap cpuz top hello args exec_args readline exec_test waitprobe fileread compiler_demo heapprobe statprobe fileprobe cwdprobe dirprobe errnoprobe badptrprobe sleep_test timerfdprobe ptrguard preempt_test inputprobe stdioprobe ip ipconfig netinfo dhcp netsend netrecv arpgw ping pinggw pingpublic netcheck ticks fault signalfdprobe connectprobe spinwkr pgrpprobe tcpecho sockeof ftpd halt reboot ataread usbinfo usbports usbdiag usbpeek usbpower usbmouse mousetest soundprobe displayprobe
USER_DYNAMIC_WITH_CRT0=crtprobe mathprobe pipeprobe dupprobe forkprobe execveprobe envprobe
USER_DYNAMIC_PROGS=$(USER_DYNAMIC_NO_CRT0) $(USER_DYNAMIC_WITH_CRT0)
USER_DYNAMIC_PIE_NO_CRT0=hello pwd cat meminfo date
USER_DYNAMIC_PIE_WITH_CRT0=
USER_DYNAMIC_PIE_PROGS=$(USER_DYNAMIC_PIE_NO_CRT0) $(USER_DYNAMIC_PIE_WITH_CRT0)
USER_DYNAMIC_LEGACY_NO_CRT0=$(filter-out $(USER_DYNAMIC_PIE_NO_CRT0),$(USER_DYNAMIC_NO_CRT0))
USER_DYNAMIC_LEGACY_WITH_CRT0=$(filter-out $(USER_DYNAMIC_PIE_WITH_CRT0),$(USER_DYNAMIC_WITH_CRT0))
USER_DYNAMIC_LEGACY_PROGS=$(USER_DYNAMIC_LEGACY_NO_CRT0) $(USER_DYNAMIC_LEGACY_WITH_CRT0)
EXT2_BIN_STATIC_PROGS=$(filter-out $(USER_DYNAMIC_PROGS),$(EXT2_BIN_PROGS))
EXT2_DEMO_STATIC_PROGS=$(filter-out $(USER_DYNAMIC_PROGS),$(EXT2_DEMO_PROGS))
EXT2_TEST_STATIC_PROGS=$(filter-out $(USER_DYNAMIC_PROGS),$(EXT2_TEST_PROGS))
EXT2_SBIN_PROGS=tcpecho sockeof ftpd
EXT2_SBIN_STATIC_PROGS=$(filter-out $(USER_DYNAMIC_PROGS),$(EXT2_SBIN_PROGS))
EXT2_BIN_ENTRIES=$(foreach prog,$(EXT2_BIN_STATIC_PROGS),bin/$(prog)=$(BIN_DIR)/$(prog).elf) \
                 $(foreach prog,$(filter $(USER_DYNAMIC_LEGACY_PROGS),$(EXT2_BIN_PROGS)),bin/$(prog)=$(BIN_DIR)/$(prog).dyn.elf) \
                 $(foreach prog,$(filter $(USER_DYNAMIC_PIE_PROGS),$(EXT2_BIN_PROGS)),bin/$(prog)=$(BIN_DIR)/$(prog).pie.elf)
EXT2_DEMO_ENTRIES=$(foreach prog,$(EXT2_DEMO_STATIC_PROGS),usr/bin/$(prog)=$(BIN_DIR)/$(prog).elf) \
                  $(foreach prog,$(filter $(USER_DYNAMIC_LEGACY_PROGS),$(EXT2_DEMO_PROGS)),usr/bin/$(prog)=$(BIN_DIR)/$(prog).dyn.elf) \
                  $(foreach prog,$(filter $(USER_DYNAMIC_PIE_PROGS),$(EXT2_DEMO_PROGS)),usr/bin/$(prog)=$(BIN_DIR)/$(prog).pie.elf)
EXT2_TEST_ENTRIES=$(foreach prog,$(EXT2_TEST_STATIC_PROGS),usr/libexec/tests/$(prog)=$(BIN_DIR)/$(prog).elf) \
                  $(foreach prog,$(filter $(USER_DYNAMIC_LEGACY_PROGS),$(EXT2_TEST_PROGS)),usr/libexec/tests/$(prog)=$(BIN_DIR)/$(prog).dyn.elf) \
                  $(foreach prog,$(filter $(USER_DYNAMIC_PIE_PROGS),$(EXT2_TEST_PROGS)),usr/libexec/tests/$(prog)=$(BIN_DIR)/$(prog).pie.elf) \
                  usr/libexec/tests/wolf3d-srcprobe=$(WOLF3D_SOURCE_PROBE_BIN)
EXT2_TEST_ENTRIES+=usr/libexec/tests/dynhello=$(BIN_DIR)/dynhello.elf \
                   usr/libexec/tests/piehello=$(BIN_DIR)/piehello.elf \
                   usr/libexec/tests/piecrtprobe=$(BIN_DIR)/piecrtprobe.elf \
                   usr/libexec/tests/piedlopenprobe=$(BIN_DIR)/piedlopenprobe.elf \
                   usr/libexec/tests/dyncrtprobe=$(BIN_DIR)/dyncrtprobe.elf \
                   usr/libexec/tests/dynmathprobe=$(BIN_DIR)/dynmathprobe.elf \
                   usr/libexec/tests/dynstdioprobe=$(BIN_DIR)/dynstdioprobe.elf \
                   usr/libexec/tests/dynlinkprobe=$(BIN_DIR)/dynlinkprobe.elf \
                   usr/libexec/tests/dynpathprobe=$(BIN_DIR)/dynpathprobe.elf \
                   usr/libexec/tests/dynfiniprobe=$(BIN_DIR)/dynfiniprobe.elf \
                   usr/libexec/tests/dlopenprobe=$(BIN_DIR)/dlopenprobe.elf \
                   usr/libexec/tests/pluginhost=$(BIN_DIR)/pluginhost.elf \
                   usr/libexec/tests/static/hello=$(BIN_DIR)/hello.elf \
                   usr/libexec/tests/static/crtprobe=$(BIN_DIR)/crtprobe.elf \
                   usr/libexec/tests/static/mathprobe=$(BIN_DIR)/mathprobe.elf \
                   usr/libexec/tests/static/stdioprobe=$(BIN_DIR)/stdioprobe.elf
EXT2_SBIN_ENTRIES=$(foreach prog,$(EXT2_SBIN_STATIC_PROGS),usr/sbin/$(prog)=$(BIN_DIR)/$(prog).elf) \
                  $(foreach prog,$(filter $(USER_DYNAMIC_LEGACY_PROGS),$(EXT2_SBIN_PROGS)),usr/sbin/$(prog)=$(BIN_DIR)/$(prog).dyn.elf) \
                  $(foreach prog,$(filter $(USER_DYNAMIC_PIE_PROGS),$(EXT2_SBIN_PROGS)),usr/sbin/$(prog)=$(BIN_DIR)/$(prog).pie.elf) \
                  usr/sbin/cserve=$(CSERVER_BIN)
EXT2_APP_ENTRIES=$(EXT2_BIN_ENTRIES) $(EXT2_DEMO_ENTRIES) $(EXT2_TEST_ENTRIES)
EXT2_APP_ENTRIES+= $(EXT2_SBIN_ENTRIES)
MAN_PAGE_FILES=$(wildcard man/man*/*)
MAN_PAGE_ENTRIES=$(foreach page,$(MAN_PAGE_FILES),usr/share/$(page)=$(CURDIR)/$(page))
USER_STATIC_LIB_ENTRIES=usr/lib/crt0.o=$(USER_CRT0_OBJ) usr/lib/libc.a=$(USER_LIBC) usr/lib/libm.a=$(USER_LIBM) usr/lib/libposix.a=$(USER_LIBPOSIX)
USER_DYN_LOADER_ENTRY=lib/ld-smallos.so=$(LD_SMALLOS_BIN)
USER_DYN_LIBC_ENTRY=lib/libc.so=$(USER_DYN_LIBC)
USER_DYN_FINI_ENTRY=lib/libdynfini.so=$(USER_DYN_FINI)
USER_DYN_PATH_ENTRY=usr/lib/libdynpath.so=$(USER_DYN_PATH)
USER_DYN_DLOPEN_DEP_ENTRY=usr/lib/libdlplugdep.so=$(USER_DYN_DLOPEN_DEP)
USER_DYN_DLOPEN_PLUGIN_ENTRY=usr/lib/libdlplug.so=$(USER_DYN_DLOPEN_PLUGIN)
USER_DYN_DIAMOND_BASE_ENTRY=usr/lib/libdldiamondbase.so=$(USER_DYN_DIAMOND_BASE)
USER_DYN_DIAMOND_LEFT_ENTRY=usr/lib/libdldiamondleft.so=$(USER_DYN_DIAMOND_LEFT)
USER_DYN_DIAMOND_RIGHT_ENTRY=usr/lib/libdldiamondright.so=$(USER_DYN_DIAMOND_RIGHT)
USER_DYN_DIAMOND_ENTRY=usr/lib/libdldiamond.so=$(USER_DYN_DIAMOND)
USER_DYN_PLUGIN_HELPER_ENTRY=usr/lib/smallos/plugins/plugin_helper.so=$(USER_DYN_PLUGIN_HELPER)
USER_DYN_PLUGIN_ALPHA_ENTRY=usr/lib/smallos/plugins/alpha.so=$(USER_DYN_PLUGIN_ALPHA)
USER_DYN_PLUGIN_BETA_ENTRY=usr/lib/smallos/plugins/beta.so=$(USER_DYN_PLUGIN_BETA)
USER_DYN_EXTRA_ENTRIES=$(USER_DYN_FINI_ENTRY) $(USER_DYN_PATH_ENTRY) $(USER_DYN_DLOPEN_DEP_ENTRY) $(USER_DYN_DLOPEN_PLUGIN_ENTRY) $(USER_DYN_DIAMOND_BASE_ENTRY) $(USER_DYN_DIAMOND_LEFT_ENTRY) $(USER_DYN_DIAMOND_RIGHT_ENTRY) $(USER_DYN_DIAMOND_ENTRY) $(USER_DYN_PLUGIN_HELPER_ENTRY) $(USER_DYN_PLUGIN_ALPHA_ENTRY) $(USER_DYN_PLUGIN_BETA_ENTRY)
USER_LIB_ENTRIES=$(USER_STATIC_LIB_ENTRIES) $(USER_DYN_LOADER_ENTRY) $(USER_DYN_LIBC_ENTRY) $(USER_DYN_EXTRA_ENTRIES)
USER_LIB_ENTRIES_NO_LOADER=$(USER_STATIC_LIB_ENTRIES) $(USER_DYN_LIBC_ENTRY) $(USER_DYN_EXTRA_ENTRIES)
USER_LIB_ENTRIES_NO_LIBC=$(USER_STATIC_LIB_ENTRIES) $(USER_DYN_LOADER_ENTRY)
WOLF3D_DATA_FILES=$(sort $(wildcard $(WOLF3D_DATA_DIR)/*.WL1) $(wildcard $(WOLF3D_DATA_DIR)/*.WL6) $(wildcard $(WOLF3D_DATA_DIR)/*.SOD) $(wildcard $(WOLF3D_DATA_DIR)/*.SDM))
ifeq ($(WOLF3D_STAGE_DATA),1)
WOLF3D_DATA_ENTRIES=$(foreach file,$(WOLF3D_DATA_FILES),usr/share/wolf3d/$(notdir $(file))=$(file))
else
WOLF3D_DATA_ENTRIES=
endif
EXT2_EXTRA_DIRS=tmp/ var/log/ lib/ usr/include/ usr/include/arpa/ usr/include/netinet/ usr/include/sys/ usr/lib/ usr/lib/smallos/ usr/lib/smallos/plugins/ usr/libexec/tests/static/ usr/share/examples/tinycc/ usr/share/man/man1/ usr/share/man/man2/ usr/share/man/man3/ usr/share/man/man4/ usr/share/man/man5/ usr/share/man/man6/ usr/share/man/man7/ usr/share/man/man8/ usr/share/wolf3d/
EXT2_BASE_EXTRA_ENTRIES=usr/bin/tcc=$(TINYCC_SMALOS_BIN) $(USER_INCLUDE_ENTRIES) $(USER_UAPI_ENTRIES) usr/share/examples/tinycc/tccmath.c=$(CURDIR)/samples/tccmath.c usr/share/examples/tinycc/tccagg.c=$(CURDIR)/samples/tccagg.c usr/share/examples/tinycc/tcctree.c=$(CURDIR)/samples/tcctree.c usr/share/examples/tinycc/tccmini.c=$(CURDIR)/samples/tccmini.c usr/share/examples/tinycc/tccsysroot.c=$(CURDIR)/samples/tccsysroot.c usr/share/examples/tinycc/tccposix.c=$(CURDIR)/samples/tccposix.c etc/cserve.ini=$(CURDIR)/samples/cserve.ini var/www/index.html=$(CURDIR)/samples/cserve_index.html var/log/boot.txt=$(CURDIR)/samples/boot.txt boot/splash.bmp=$(CURDIR)/assets/boot_splash.bmp $(MAN_PAGE_ENTRIES)
EXT2_EXTRA_ENTRIES=$(EXT2_BASE_EXTRA_ENTRIES) $(USER_LIB_ENTRIES)
FRACTINT_EXTRA_ENTRIES=usr/share/xfractint/fractint.hlp=$(FRACTINT_DIR)/fractint.hlp usr/share/xfractint/sstools.ini=$(FRACTINT_DIR)/sstools.ini $(FRACTINT_DATA_ENTRIES)
EXT2_ALL_EXTRA_ENTRIES=$(EXT2_EXTRA_ENTRIES) $(FRACTINT_EXTRA_ENTRIES) $(WOLF3D_DATA_ENTRIES)
EXT2_EXTRA_DIRS+= usr/share/xfractint/ usr/share/xfractint/maps/ usr/share/xfractint/pars/ usr/share/xfractint/formulas/ usr/share/xfractint/lsystem/ usr/share/xfractint/ifs/
EXT2_EXTRA_FILES=$(foreach entry,$(EXT2_ALL_EXTRA_ENTRIES),$(word 2,$(subst =, ,$(entry))))

KERNEL_OBJS=$(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASM_SRCS)) \
            $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(KERNEL_C_SRCS))

USER_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(USER_SRCS))
GUI_OBJS=$(OBJ_DIR)/user/gui/app.o $(OBJ_DIR)/user/gui/shell_window.o
USER_SHELL_OBJS=$(OBJ_DIR)/user/shell/app.o
USER_LIBC_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.c,$(USER_LIBC_SRCS))) \
               $(patsubst $(USER_DIR)/%.asm,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.asm,$(USER_LIBC_SRCS)))
USER_LIBM_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.c,$(USER_LIBM_SRCS)))
USER_POSIX_OBJS=$(patsubst $(USER_DIR)/%.c,$(OBJ_DIR)/user/%.o,$(filter $(USER_DIR)/%.c,$(USER_POSIX_SRCS)))
USER_RUNTIME_OBJS=$(USER_LIBC_OBJS) $(USER_LIBM_OBJS) $(USER_POSIX_OBJS)
USER_CRT0_OBJ=$(OBJ_DIR)/user/crt/crt0.o
USER_PIE_OBJ_DIR=$(OBJ_DIR)/user-pie
USER_PIE_CRT0_OBJ=$(USER_PIE_OBJ_DIR)/crt/crt0.pic.o
USER_LIB_DIR=$(OBJ_DIR)/user/lib
USER_LIBC=$(USER_LIB_DIR)/libc.a
USER_LIBM=$(USER_LIB_DIR)/libm.a
USER_LIBPOSIX=$(USER_LIB_DIR)/libposix.a
USER_LIB_ARCHIVES=$(USER_LIBPOSIX) $(USER_LIBC) $(USER_LIBM)
USER_LINK_LIBS=-L$(USER_LIB_DIR) --start-group -lposix -lc -lm --end-group
USER_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_PROGS))) $(BIN_DIR)/fractint.elf
USER_DYN_OBJ_DIR=$(OBJ_DIR)/user-dyn
USER_DYN_LIB_DIR=$(BIN_DIR)/lib
LD_SMALLOS_OBJ=$(USER_DYN_OBJ_DIR)/ld_smallos.o
LD_SMALLOS_BOOT_OBJ=$(USER_DYN_OBJ_DIR)/ld_smallos_boot.pic.o
LD_SMALLOS_OBJS=$(LD_SMALLOS_BOOT_OBJ) $(LD_SMALLOS_OBJ)
LD_SMALLOS_BIN=$(USER_DYN_LIB_DIR)/ld-smallos.so
USER_DYN_RUNTIME_OBJS=$(patsubst $(USER_DIR)/%.c,$(USER_DYN_OBJ_DIR)/%.pic.o,$(filter $(USER_DIR)/%.c,$(USER_RUNTIME_SRCS))) \
                      $(patsubst $(USER_DIR)/%.asm,$(USER_DYN_OBJ_DIR)/%.pic.o,$(filter $(USER_DIR)/%.asm,$(USER_RUNTIME_SRCS)))
USER_DYN_LIBC=$(USER_DYN_LIB_DIR)/libc.so
USER_DYN_FINI=$(USER_DYN_LIB_DIR)/libdynfini.so
USER_DYN_PATH=$(USER_DYN_LIB_DIR)/libdynpath.so
USER_DYN_DLOPEN_DEP=$(USER_DYN_LIB_DIR)/libdlplugdep.so
USER_DYN_DLOPEN_PLUGIN=$(USER_DYN_LIB_DIR)/libdlplug.so
USER_DYN_DIAMOND_BASE=$(USER_DYN_LIB_DIR)/libdldiamondbase.so
USER_DYN_DIAMOND_LEFT=$(USER_DYN_LIB_DIR)/libdldiamondleft.so
USER_DYN_DIAMOND_RIGHT=$(USER_DYN_LIB_DIR)/libdldiamondright.so
USER_DYN_DIAMOND=$(USER_DYN_LIB_DIR)/libdldiamond.so
USER_DYN_PLUGIN_HELPER=$(USER_DYN_LIB_DIR)/plugin_helper.so
USER_DYN_PLUGIN_ALPHA=$(USER_DYN_LIB_DIR)/alpha.so
USER_DYN_PLUGIN_BETA=$(USER_DYN_LIB_DIR)/beta.so
USER_DYN_EXTRA_SHARED=$(USER_DYN_FINI) $(USER_DYN_PATH) $(USER_DYN_DLOPEN_DEP) $(USER_DYN_DLOPEN_PLUGIN) $(USER_DYN_DIAMOND_BASE) $(USER_DYN_DIAMOND_LEFT) $(USER_DYN_DIAMOND_RIGHT) $(USER_DYN_DIAMOND) $(USER_DYN_PLUGIN_HELPER) $(USER_DYN_PLUGIN_ALPHA) $(USER_DYN_PLUGIN_BETA)
USER_DYN_PROGS=dynhello dyncrtprobe dynmathprobe dynstdioprobe dynlinkprobe dynpathprobe dynfiniprobe dlopenprobe pluginhost
USER_DYN_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_DYN_PROGS)))
USER_PIE_PROBE_PROGS=piehello piecrtprobe piedlopenprobe
USER_PIE_PROBE_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_PIE_PROBE_PROGS)))
DYNFAILPROBE_BIN=$(BIN_DIR)/dynfailprobe.elf
DYN_NO_LOADER_EXT2=$(BIN_DIR)/ext2.dyn-no-loader.img
DYN_NO_LIBC_EXT2=$(BIN_DIR)/ext2.dyn-no-libc.img
DYN_NO_LOADER_IMG=$(IMG_DIR)/smallos-dyn-no-loader.img
DYN_NO_LIBC_IMG=$(IMG_DIR)/smallos-dyn-no-libc.img
DYN_NEG_TEST_ENTRIES=usr/libexec/tests/dynfailprobe=$(DYNFAILPROBE_BIN)
DYN_NEG_APP_ENTRIES=$(EXT2_BIN_ENTRIES) $(EXT2_DEMO_ENTRIES) $(EXT2_TEST_ENTRIES) $(DYN_NEG_TEST_ENTRIES) usr/sbin/cserve=$(CSERVER_BIN)
DYN_NO_LOADER_ALL_EXTRA_ENTRIES=$(EXT2_BASE_EXTRA_ENTRIES) $(USER_LIB_ENTRIES_NO_LOADER) $(FRACTINT_EXTRA_ENTRIES) $(WOLF3D_DATA_ENTRIES)
DYN_NO_LIBC_ALL_EXTRA_ENTRIES=$(EXT2_BASE_EXTRA_ENTRIES) $(USER_LIB_ENTRIES_NO_LIBC) $(FRACTINT_EXTRA_ENTRIES) $(WOLF3D_DATA_ENTRIES)
DYN_NO_LOADER_EXTRA_FILES=$(foreach entry,$(DYN_NO_LOADER_ALL_EXTRA_ENTRIES),$(word 2,$(subst =, ,$(entry))))
DYN_NO_LIBC_EXTRA_FILES=$(foreach entry,$(DYN_NO_LIBC_ALL_EXTRA_ENTRIES),$(word 2,$(subst =, ,$(entry))))
USER_DYNAMIC_NO_CRT0_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .dyn.elf,$(USER_DYNAMIC_LEGACY_NO_CRT0)))
USER_DYNAMIC_WITH_CRT0_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .dyn.elf,$(USER_DYNAMIC_LEGACY_WITH_CRT0)))
USER_DYNAMIC_LEGACY_ELFS=$(USER_DYNAMIC_NO_CRT0_ELFS) $(USER_DYNAMIC_WITH_CRT0_ELFS)
USER_DYNAMIC_PIE_NO_CRT0_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .pie.elf,$(USER_DYNAMIC_PIE_NO_CRT0)))
USER_DYNAMIC_PIE_WITH_CRT0_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .pie.elf,$(USER_DYNAMIC_PIE_WITH_CRT0)))
USER_DYNAMIC_PIE_ELFS=$(USER_DYNAMIC_PIE_NO_CRT0_ELFS) $(USER_DYNAMIC_PIE_WITH_CRT0_ELFS) $(USER_PIE_PROBE_ELFS)
USER_DYNAMIC_ELFS=$(USER_DYNAMIC_LEGACY_ELFS) $(USER_DYNAMIC_PIE_ELFS)
USER_DYNAMIC_STATIC_ELFS=$(addprefix $(BIN_DIR)/,$(addsuffix .elf,$(USER_DYNAMIC_PROGS)))
USER_ELFS+=$(USER_DYN_ELFS) $(USER_DYNAMIC_ELFS) $(USER_DYN_EXTRA_SHARED)

OBJ_SUBDIRS=$(sort \
	$(dir $(KERNEL_OBJS)) \
	$(dir $(USER_OBJS)) \
	$(dir $(USER_RUNTIME_OBJS)) \
	$(dir $(USER_DYN_RUNTIME_OBJS)) \
	$(dir $(USER_PIE_CRT0_OBJ)) \
	$(USER_PIE_OBJ_DIR) \
	$(USER_DYN_OBJ_DIR) \
	$(dir $(USER_CRT0_OBJ)) \
	$(USER_LIB_DIR) \
	$(USER_DYN_LIB_DIR) \
	$(dir $(GUI_OBJS)) \
	$(dir $(USER_SHELL_OBJS)) \
	$(dir $(FRACTINT_OBJS)) \
)

BUILD_SUBDIRS=$(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR) $(GEN_DIR) $(IMG_DIR) $(dir $(IMG_FILE)) $(TOOLS_DIR) $(OBJ_SUBDIRS) $(STATE_DIR)
BUILD_SUBDIRS+=$(TINYCC_SMALOS_OBJ_DIR) $(CSERVER_OBJ_DIR)

all: artifacts

image: check-third-party image-layout-check

artifacts: image vmdk

dirs:
	mkdir -p $(BUILD_SUBDIRS)

deps:
	git submodule update --init --recursive
	$(MAKE) fractint-source

fractint-source: $(FRACTINT_SVN_STAMP)

wolf3d-shareware-data:
	mkdir -p $(dir $(WOLF3D_SHAREWARE_ZIP)) $(WOLF3D_DATA_DIR)
	curl -L --fail -o $(WOLF3D_SHAREWARE_ZIP) $(WOLF3D_SHAREWARE_URL)
	$(PYTHON3) -c 'import os,sys,zipfile; z,d=sys.argv[1:3]; os.makedirs(d,exist_ok=True); zf=zipfile.ZipFile(z); [open(os.path.join(d,os.path.basename(n)),"wb").write(zf.read(n)) for n in zf.namelist() if n.upper().endswith(".WL1")]' $(WOLF3D_SHAREWARE_ZIP) $(WOLF3D_DATA_DIR)

$(FRACTINT_SVN_STAMP):
	rm -rf $(FRACTINT_DIR)
	mkdir -p $(dir $(FRACTINT_DIR))
	svn export -q -r $(FRACTINT_SVN_REV) $(FRACTINT_SVN_URL) $(FRACTINT_DIR)
	touch $@

check-third-party: $(FRACTINT_SVN_STAMP)
	@if [ ! -f "$(THIRD_PARTY_TINYCC_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_CSERVER_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_FTP_CLIENT_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_FTP_SERVER_SENTINEL)" ] || \
	    [ ! -f "$(THIRD_PARTY_WOLF3D_SENTINEL)" ]; then \
		echo "Missing third-party dependencies."; \
		echo "Run: git submodule update --init --recursive"; \
		echo "Then run: make deps"; \
		echo "Or clone with: git clone --recurse-submodules <repo-url>"; \
		exit 1; \
	fi

$(OBJ_DIR)/boot/%.o: $(BOOT_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/%.o: $(KERNEL_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(OBJ_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c Makefile $(KERNEL_CONFIG_STAMP) | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/drivers/%.o: $(DRIVERS_DIR)/%.c Makefile $(KERNEL_CONFIG_STAMP) | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DRIVER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/drivers/display.o \
$(OBJ_DIR)/drivers/fb_console.o \
$(OBJ_DIR)/drivers/screen.o \
$(OBJ_DIR)/drivers/terminal.o: DRIVER_CFLAGS += $(DISPLAY_DRIVER_CFLAGS)

$(OBJ_DIR)/drivers/usb.o: DRIVER_CFLAGS += -Os

$(OBJ_DIR)/exec/%.o: $(EXEC_DIR)/%.c Makefile $(KERNEL_CONFIG_STAMP) | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KERNEL_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/%.o: $(USER_DIR)/%.c | dirs
	$(CC) $(USER_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/wolf3d.o: $(USER_DIR)/wolf3d.c $(WOLF3D_PORT_INCLUDE_DIR)/wolf3d_port.h | dirs
	$(CC) $(WOLF3D_NATIVE_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(WOLF3D_PALETTE_H): $(WOLF3D_PALETTE_SRC) Makefile | dirs
	$(PYTHON3) -c 'import sys; data=open(sys.argv[1],"rb").read(); \
matches=[i for i in range(len(data)-768) if max(data[i:i+768]) <= 63 and data[i:i+6] == b"\0\0\0\0\0*" and len(set(data[i:i+768])) >= 40]; \
palette=data[matches[0]:matches[0]+768]; \
out=open(sys.argv[2],"w"); \
out.write("#ifndef SMALLOS_WOLF3D_PALETTE_H\n#define SMALLOS_WOLF3D_PALETTE_H\n\nstatic const unsigned char smallos_wolf3d_palette[768] = {\n"); \
	[out.write("    " + ", ".join(str(b) for b in palette[i:i+12]) + ",\n") for i in range(0, 768, 12)]; \
	out.write("};\n\n#endif\n")' $< $@

$(WOLF3D_SIGNON_H): $(WOLF3D_SIGNON_SRC) Makefile | dirs
	$(PYTHON3) tools/extract_wolf3d_signon.py $< $@

$(WOLF3D_SRC_STAMP): $(THIRD_PARTY_WOLF3D_SENTINEL) tools/prepare_wolf3d_source.py Makefile | dirs
	$(PYTHON3) tools/prepare_wolf3d_source.py $(WOLF3D_SRC_DIR) $(WOLF3D_SRC_MIRROR)
	touch $@

$(OBJ_DIR)/user/ports/wolf3d/upstream/%.o: $(WOLF3D_SRC_STAMP) $(WOLF3D_PORT_INCLUDE_DIR)/wolf3d_port.h
	mkdir -p $(dir $@)
	$(CC) -x c $(WOLF3D_NATIVE_CPPFLAGS) -DSMALLOS_WOLF3D_SOURCE_PROBE $(CFLAGS) $(USER_CFLAGS) -Dmain=wolf3d_upstream_main -Wno-unknown-pragmas -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-switch -Wno-pointer-sign -Wno-incompatible-pointer-types -Wno-char-subscripts -Wno-implicit-function-declaration $(DEPFLAGS) -MF $(@:.o=.d) -c $(WOLF3D_SRC_MIRROR)/$*.C -o $@

$(WOLF3D_NATIVE_SHIM_OBJ): $(WOLF3D_PORT_DIR)/wolf3d_dos_shim.c $(WOLF3D_PORT_INCLUDE_DIR)/wolf3d_port.h | dirs
	mkdir -p $(dir $@)
	$(CC) $(WOLF3D_NATIVE_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(WOLF3D_PLATFORM_SHIM_OBJ): $(WOLF3D_PORT_DIR)/wolf3d_platform_shim.c $(WOLF3D_PORT_INCLUDE_DIR)/wolf3d_port.h $(WOLF3D_SRC_STAMP) $(WOLF3D_PALETTE_H) $(WOLF3D_SIGNON_H) | dirs
	mkdir -p $(dir $@)
	$(CC) $(WOLF3D_NATIVE_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) -Wno-implicit-function-declaration $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(WOLF3D_CONFIG_SHIM_OBJ): $(WOLF3D_PORT_DIR)/wolf3d_config_shim.c $(WOLF3D_PORT_INCLUDE_DIR)/wolf3d_port.h $(WOLF3D_SRC_STAMP) | dirs
	mkdir -p $(dir $@)
	$(CC) $(WOLF3D_NATIVE_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(WOLF3D_ASSETS_OBJ): $(WOLF3D_PORT_DIR)/wolf3d_assets.c $(WOLF3D_PALETTE_H) $(WOLF3D_SIGNON_H) | dirs
	mkdir -p $(dir $@)
	$(CC) $(WOLF3D_NATIVE_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(WOLF3D_SOURCE_PROBE_OBJ): $(WOLF3D_PORT_DIR)/wolf3d_source_probe.c $(WOLF3D_PORT_INCLUDE_DIR)/wolf3d_port.h $(WOLF3D_SRC_STAMP) | dirs
	mkdir -p $(dir $@)
	$(CC) $(WOLF3D_NATIVE_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(WOLF3D_SOURCE_PROBE_BIN): $(WOLF3D_SOURCE_PROBE_OBJ) $(WOLF3D_NATIVE_SHIM_OBJ) $(WOLF3D_PLATFORM_SHIM_OBJ) $(WOLF3D_CONFIG_SHIM_OBJ) $(WOLF3D_ASSETS_OBJ) $(WOLF3D_UPSTREAM_PROBE_OBJS) $(OBJ_DIR)/user/gfx.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

wolf3d-source-probe: $(WOLF3D_SOURCE_PROBE_BIN) $(WOLF3D_UPSTREAM_EXTRA_COMPILE_OBJS)

$(filter $(OBJ_DIR)/user/libc/%.o $(OBJ_DIR)/user/libm/%.o $(OBJ_DIR)/user/posix/%.o,$(USER_RUNTIME_OBJS)): USER_CFLAGS += -ffunction-sections -fdata-sections

$(OBJ_DIR)/user/crt/%.o: $(USER_DIR)/crt/%.c | dirs
	$(CC) $(USER_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/gui/%.o: $(USER_DIR)/gui/%.c | dirs
	$(CC) $(USER_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/shell/%.o: $(USER_DIR)/shell/%.c | dirs
	$(CC) $(USER_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(OBJ_DIR)/user/%.o: $(USER_DIR)/%.asm | dirs
	$(ASM) -f elf32 $< -o $@

$(USER_LIBC): $(USER_LIBC_OBJS) | dirs
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBM): $(USER_LIBM_OBJS) | dirs
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBPOSIX): $(USER_POSIX_OBJS) | dirs
	rm -f $@
	$(AR) rcs $@ $^

$(USER_DYN_OBJ_DIR)/%.pic.o: $(USER_DIR)/%.c | dirs
	mkdir -p $(dir $@)
	$(CC) $(USER_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) -fPIC -ffunction-sections -fdata-sections $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(USER_DYN_OBJ_DIR)/%.pic.o: $(USER_DIR)/%.asm | dirs
	mkdir -p $(dir $@)
	$(ASM) -f elf32 $< -o $@

$(USER_PIE_OBJ_DIR)/%.pic.o: $(USER_DIR)/%.c | dirs
	mkdir -p $(dir $@)
	$(CC) $(USER_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) -fPIC -ffunction-sections -fdata-sections $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(USER_PIE_OBJ_DIR)/%.pic.o: $(USER_DIR)/%.asm | dirs
	mkdir -p $(dir $@)
	$(ASM) -f elf32 $< -o $@

$(LD_SMALLOS_OBJ): $(USER_DIR)/ld_smallos.c $(KERNEL_DIR)/elf.h $(KERNEL_DIR)/uapi_syscall.h | dirs
	mkdir -p $(dir $@)
	$(CC) $(USER_PUBLIC_CPPFLAGS) $(CFLAGS) -Os -ffunction-sections -fdata-sections -c $< -o $@

$(LD_SMALLOS_BIN): $(LD_SMALLOS_OBJS) | dirs
	$(LD) $(LD_SMALLOS_LDFLAGS) $^ -o $@

$(USER_DYN_LIBC): $(USER_DYN_RUNTIME_OBJS) | dirs
	$(LD) -m elf_i386 -shared -soname libc.so --hash-style=sysv -z now $^ $(LIBGCC_FILE) -o $@

$(USER_DYN_FINI): $(USER_DYN_OBJ_DIR)/dynfini.pic.o | dirs
	$(LD) -m elf_i386 -shared -soname libdynfini.so --hash-style=sysv -z now $^ -o $@

$(USER_DYN_PATH): $(USER_DYN_OBJ_DIR)/dynpath.pic.o | dirs
	$(LD) -m elf_i386 -shared -soname libdynpath.so --hash-style=sysv -z now $^ -o $@

$(USER_DYN_DLOPEN_DEP): $(USER_DYN_OBJ_DIR)/dlopen_dep.pic.o | dirs
	$(LD) -m elf_i386 -shared -soname libdlplugdep.so --hash-style=sysv -z now $^ -o $@

$(USER_DYN_DLOPEN_PLUGIN): $(USER_DYN_OBJ_DIR)/dlopen_plugin.pic.o $(USER_DYN_DLOPEN_DEP) | dirs
	$(LD) -m elf_i386 -shared -soname libdlplug.so --hash-style=sysv -z now -rpath /usr/lib $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -ldlplugdep -o $@

$(USER_DYN_DIAMOND_BASE): $(USER_DYN_OBJ_DIR)/dlopen_diamond_base.pic.o | dirs
	$(LD) -m elf_i386 -shared -soname libdldiamondbase.so --hash-style=sysv -z now $^ -o $@

$(USER_DYN_DIAMOND_LEFT): $(USER_DYN_OBJ_DIR)/dlopen_diamond_left.pic.o $(USER_DYN_DIAMOND_BASE) | dirs
	$(LD) -m elf_i386 -shared -soname libdldiamondleft.so --hash-style=sysv -z now -rpath /usr/lib $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -ldldiamondbase -o $@

$(USER_DYN_DIAMOND_RIGHT): $(USER_DYN_OBJ_DIR)/dlopen_diamond_right.pic.o $(USER_DYN_DIAMOND_BASE) | dirs
	$(LD) -m elf_i386 -shared -soname libdldiamondright.so --hash-style=sysv -z now -rpath /usr/lib $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -ldldiamondbase -o $@

$(USER_DYN_DIAMOND): $(USER_DYN_OBJ_DIR)/dlopen_diamond.pic.o $(USER_DYN_DIAMOND_LEFT) $(USER_DYN_DIAMOND_RIGHT) | dirs
	$(LD) -m elf_i386 -shared -soname libdldiamond.so --hash-style=sysv -z now -rpath /usr/lib $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -ldldiamondleft -ldldiamondright -o $@

$(USER_DYN_PLUGIN_HELPER): $(USER_DYN_OBJ_DIR)/plugin_helper.pic.o | dirs
	$(LD) -m elf_i386 -shared -soname plugin_helper.so --hash-style=sysv -z now $^ -o $@

$(USER_DYN_PLUGIN_ALPHA): $(USER_DYN_OBJ_DIR)/plugin_alpha.pic.o $(USER_DYN_PLUGIN_HELPER) | dirs
	$(LD) -m elf_i386 -shared -soname alpha.so --hash-style=sysv -z now -rpath /usr/lib/smallos/plugins $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -l:plugin_helper.so -o $@

$(USER_DYN_PLUGIN_BETA): $(USER_DYN_OBJ_DIR)/plugin_beta.pic.o $(USER_DYN_PLUGIN_HELPER) | dirs
	$(LD) -m elf_i386 -shared -soname beta.so --hash-style=sysv -z now -rpath /usr/lib/smallos/plugins $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -l:plugin_helper.so -o $@

$(USER_DYNAMIC_NO_CRT0_ELFS): $(BIN_DIR)/%.dyn.elf: $(OBJ_DIR)/user/%.o $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(USER_DYNAMIC_WITH_CRT0_ELFS): $(BIN_DIR)/%.dyn.elf: $(OBJ_DIR)/user/%.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(USER_DYNAMIC_PIE_NO_CRT0_ELFS): $(BIN_DIR)/%.pie.elf: $(USER_PIE_OBJ_DIR)/%.pic.o $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_PIE_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(USER_DYNAMIC_PIE_WITH_CRT0_ELFS): $(BIN_DIR)/%.pie.elf: $(USER_PIE_OBJ_DIR)/%.pic.o $(USER_PIE_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_PIE_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/dynhello.elf: $(OBJ_DIR)/user/hello.o $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/piehello.elf: $(USER_PIE_OBJ_DIR)/hello.pic.o $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_PIE_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/piecrtprobe.elf: $(USER_PIE_OBJ_DIR)/crtprobe.pic.o $(USER_PIE_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_PIE_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/piedlopenprobe.elf: $(USER_PIE_OBJ_DIR)/dlopenprobe.pic.o $(USER_PIE_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_PIE_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/dyncrtprobe.elf: $(OBJ_DIR)/user/crtprobe.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/dynmathprobe.elf: $(OBJ_DIR)/user/mathprobe.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/dynstdioprobe.elf: $(OBJ_DIR)/user/stdioprobe.o $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/dynlinkprobe.elf: $(OBJ_DIR)/user/dynlinkprobe.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(USER_DYN_FINI) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -ldynfini -lc -o $@

$(BIN_DIR)/dynpathprobe.elf: $(OBJ_DIR)/user/dynpathprobe.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(USER_DYN_PATH) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) -rpath /usr/lib $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -ldynpath -lc -o $@

$(BIN_DIR)/dynfiniprobe.elf: $(OBJ_DIR)/user/dynfiniprobe.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/dlopenprobe.elf: $(OBJ_DIR)/user/dlopenprobe.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(BIN_DIR)/pluginhost.elf: $(OBJ_DIR)/user/pluginhost.o $(USER_CRT0_OBJ) $(USER_DYN_LIBC) $(LD_SMALLOS_BIN) | dirs
	$(LD) $(USER_DYN_LDFLAGS) $(filter %.o,$^) -L$(USER_DYN_LIB_DIR) -lc -o $@

$(DYNFAILPROBE_BIN): $(OBJ_DIR)/user/dynfailprobe.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

dyn-size-report: $(USER_DYNAMIC_STATIC_ELFS) $(USER_DYNAMIC_ELFS) $(USER_DYN_ELFS) $(USER_DYN_LIBC) $(USER_DYN_EXTRA_SHARED) $(LD_SMALLOS_BIN)
	@static_total=$$(wc -c $(USER_DYNAMIC_STATIC_ELFS) | awk 'END {print $$1}'); \
	dyn_prog_total=$$(wc -c $(USER_DYNAMIC_ELFS) | awk 'END {print $$1}'); \
	runtime_total=$$(wc -c $(USER_DYN_LIBC) $(USER_DYN_EXTRA_SHARED) $(LD_SMALLOS_BIN) | awk 'END {print $$1}'); \
	dyn_total=$$((dyn_prog_total + runtime_total)); \
	printf '%-34s %10s\n' category bytes; \
	printf '%-34s %10s\n' converted-static-total $$static_total; \
	printf '%-34s %10s\n' converted-dynamic-programs $$dyn_prog_total; \
	printf '%-34s %10s\n' shared-runtime $$runtime_total; \
	printf '%-34s %10s\n' dynamic-programs-plus-runtime $$dyn_total; \
	printf '%-34s %+10d\n' net-vs-static $$((dyn_total - static_total)); \
	printf '\n%-34s %10s\n' artifact bytes; \
	wc -c $(USER_DYNAMIC_ELFS) $(USER_DYN_LIBC) $(USER_DYN_EXTRA_SHARED) $(LD_SMALLOS_BIN) | sed 's/^ *//'

dynamic-link-check: $(USER_DYNAMIC_ELFS) $(USER_DYN_ELFS) $(USER_DYN_LIBC) $(USER_DYN_EXTRA_SHARED) $(LD_SMALLOS_BIN)
	$(PYTHON3) tools/verify_dynamic_link.py \
		--interp /lib/ld-smallos.so \
		--libc $(USER_DYN_LIBC) \
		--loader $(LD_SMALLOS_BIN) \
		$(foreach shared,$(USER_DYN_EXTRA_SHARED),--shared $(shared)) \
		$(foreach pie,$(USER_DYNAMIC_PIE_ELFS),--pie $(pie)) \
		$(USER_DYNAMIC_LEGACY_ELFS) $(USER_DYN_ELFS)

$(TINYCC_SMALOS_PATCH_STAMP): check-third-party patches/tinycc/smallos.patch | dirs
	rm -rf $(TINYCC_SMALOS_SRC_DIR)
	mkdir -p $(TINYCC_SMALOS_SRC_DIR)
	(cd $(CURDIR)/third_party/tinycc && tar cf - --exclude=.git .) | (cd $(TINYCC_SMALOS_SRC_DIR) && tar xf -)
	patch -d $(TINYCC_SMALOS_SRC_DIR) -p1 < patches/tinycc/smallos.patch
	touch $@

$(TINYCC_SMALOS_OBJ): $(TINYCC_SMALOS_PATCH_STAMP) $(TINYCC_CONFIG_STAMP) | dirs
	$(CC) $(USER_PUBLIC_CPPFLAGS) $(CFLAGS) -Os -ffunction-sections -fdata-sections -I$(TINYCC_DIR) -I$(TINYCC_SMALOS_SRC_DIR) -DTCC_TARGET_I386 -DTCC_TARGET_SMALLOS -c $(TINYCC_SMALOS_SRC) -o $@

$(TINYCC_SMALOS_BIN): $(TINYCC_SMALOS_OBJ) $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) -s $(filter %.o,$^) $(USER_LINK_LIBS) $(LIBGCC_FILE) -o $@

$(CSERVER_OBJ_DIR)/%.o: $(CSERVER_DIR)/src/%.c check-third-party | dirs
	$(CC) $(USER_PUBLIC_CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -std=c2x -D_GNU_SOURCE -I$(CSERVER_DIR)/include -c $< -o $@

$(CSERVER_BIN): $(CSERVER_OBJS) $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) $(LIBGCC_FILE) -o $@

$(FRACTINT_DIR)/headers/helpdefs.h $(FRACTINT_DIR)/fractint.hlp: $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/dos_help/help.src $(FRACTINT_DIR)/dos_help/hc.c
	$(MAKE) -C $(FRACTINT_DIR) fractint.hlp CC="$(FRACTINT_HELP_CC)" WITHXFT= XFTHFD= OPT="-O2" HELP=help.src
	mv -f $(FRACTINT_DIR)/dos_help/helpdefs.h $(FRACTINT_DIR)/headers/helpdefs.h

$(FRACTINT_OBJ_DIR)/common/fractint.o: $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/common/fractint.c $(FRACTINT_DIR)/headers/helpdefs.h Makefile | dirs
	$(CC) $(FRACTINT_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(FRACTINT_DEFS) $(FRACTINT_COMPAT_CFLAGS) -Dmain=fractint_upstream_main -ffunction-sections -fdata-sections $(DEPFLAGS) -MF $(@:.o=.d) -c $(FRACTINT_DIR)/common/fractint.c -o $@

$(FRACTINT_OBJ_DIR)/common/%.o: $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/common/%.c $(FRACTINT_DIR)/headers/helpdefs.h Makefile | dirs
	$(CC) $(FRACTINT_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(FRACTINT_DEFS) $(FRACTINT_COMPAT_CFLAGS) -ffunction-sections -fdata-sections $(DEPFLAGS) -MF $(@:.o=.d) -c $(FRACTINT_DIR)/common/$*.c -o $@

$(FRACTINT_OBJ_DIR)/unix/%.o: $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/unix/%.c $(FRACTINT_DIR)/headers/helpdefs.h Makefile | dirs
	$(CC) $(FRACTINT_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(FRACTINT_DEFS) $(FRACTINT_COMPAT_CFLAGS) -ffunction-sections -fdata-sections $(DEPFLAGS) -MF $(@:.o=.d) -c $(FRACTINT_DIR)/unix/$*.c -o $@

$(FRACTINT_OBJ_DIR)/port/%.o: $(FRACTINT_SVN_STAMP) $(FRACTINT_PORT_DIR)/%.c $(FRACTINT_DIR)/headers/helpdefs.h Makefile | dirs
	$(CC) $(FRACTINT_PORT_CPPFLAGS) $(CFLAGS) $(USER_CFLAGS) $(FRACTINT_DEFS) $(FRACTINT_COMPAT_CFLAGS) -ffunction-sections -fdata-sections $(DEPFLAGS) -MF $(@:.o=.d) -c $(FRACTINT_PORT_DIR)/$*.c -o $@

$(BIN_DIR)/crtprobe.elf: $(OBJ_DIR)/user/crtprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/pipeprobe.elf: $(OBJ_DIR)/user/pipeprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/dupprobe.elf: $(OBJ_DIR)/user/dupprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/forkprobe.elf: $(OBJ_DIR)/user/forkprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/execveprobe.elf: $(OBJ_DIR)/user/execveprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/envprobe.elf: $(OBJ_DIR)/user/envprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/mathprobe.elf: $(OBJ_DIR)/user/mathprobe.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/bmpview.elf: $(OBJ_DIR)/user/bmpview.o $(OBJ_DIR)/user/image_bmp.o $(OBJ_DIR)/user/gfx.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/bootsplash.elf: $(OBJ_DIR)/user/bootsplash.o $(OBJ_DIR)/user/image_bmp.o $(OBJ_DIR)/user/gfx.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/diskview.elf: $(OBJ_DIR)/user/diskview.o $(OBJ_DIR)/user/gfx.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/gui.elf: $(OBJ_DIR)/user/gui.o $(GUI_OBJS) $(OBJ_DIR)/user/gfx.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/shell.elf: $(OBJ_DIR)/user/shell.o $(USER_SHELL_OBJS) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/plasma.elf: $(OBJ_DIR)/user/plasma.o $(OBJ_DIR)/user/gfx.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/mandel.elf: $(OBJ_DIR)/user/mandel.o $(OBJ_DIR)/user/gfx.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/wolf3d.elf: $(OBJ_DIR)/user/wolf3d.o $(WOLF3D_NATIVE_SHIM_OBJ) $(WOLF3D_PLATFORM_SHIM_OBJ) $(WOLF3D_CONFIG_SHIM_OBJ) $(WOLF3D_ASSETS_OBJ) $(WOLF3D_UPSTREAM_PROBE_OBJS) $(OBJ_DIR)/user/gfx.o $(USER_CRT0_OBJ) $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(BIN_DIR)/fractint.elf: $(FRACTINT_OBJS) $(USER_CRT0_OBJ) $(OBJ_DIR)/user/gfx.o $(OBJ_DIR)/user/gfx_indexed.o $(OBJ_DIR)/user/gfx_text.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) $(LIBGCC_FILE) -o $@

$(KERNEL_CONFIG_STAMP): FORCE | dirs
	@cfg='NIC_DRIVER=$(NIC_DRIVER) DISPLAY_BACKEND=$(DISPLAY_BACKEND) SERIAL_CONSOLE=$(SERIAL_CONSOLE) BOOT_RAMDISK_FALLBACK=$(BOOT_RAMDISK_FALLBACK) BOOT_SKIP_USB_STORAGE=$(BOOT_SKIP_USB_STORAGE)'; \
	if [ ! -f $@ ] || [ "$$(cat $@)" != "$$cfg" ]; then \
		printf '%s\n' "$$cfg" > $@; \
	fi

$(BIN_DIR)/kernel.elf: $(KERNEL_OBJS) linker.ld $(KERNEL_CONFIG_STAMP) Makefile | dirs
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(BIN_DIR)/kernel.bin: $(BIN_DIR)/kernel.elf | dirs
	$(OBJCOPY) -O binary $< $@

$(BIN_DIR)/%.elf: $(OBJ_DIR)/user/%.o $(USER_LIB_ARCHIVES) | dirs
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) $(USER_LINK_LIBS) -o $@

$(TOOLS_DIR)/mkext2: tools/mkext2.c | dirs
	$(HOST_CC) -o $@ $<

$(TOOLS_DIR)/mkimage: tools/mkimage.c | dirs
	$(HOST_CC) -o $@ $<

# ext2 seed image (generated from the current tree)
#
$(BIN_DIR)/ext2.seed.img: $(USER_ELFS) $(CSERVER_BIN) $(WOLF3D_SOURCE_PROBE_BIN) $(TOOLS_DIR)/mkext2 $(EXT2_EXTRA_FILES) $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/fractint.hlp Makefile | dirs
	$(TOOLS_DIR)/mkext2 $@ $(EXT2_APP_ENTRIES) $(EXT2_ALL_EXTRA_ENTRIES) $(EXT2_EXTRA_DIRS)

$(DYN_NO_LOADER_EXT2): $(USER_ELFS) $(DYNFAILPROBE_BIN) $(CSERVER_BIN) $(WOLF3D_SOURCE_PROBE_BIN) $(TOOLS_DIR)/mkext2 $(DYN_NO_LOADER_EXTRA_FILES) $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/fractint.hlp Makefile | dirs
	$(TOOLS_DIR)/mkext2 $@ $(DYN_NEG_APP_ENTRIES) $(DYN_NO_LOADER_ALL_EXTRA_ENTRIES) $(EXT2_EXTRA_DIRS)

$(DYN_NO_LIBC_EXT2): $(USER_ELFS) $(DYNFAILPROBE_BIN) $(CSERVER_BIN) $(WOLF3D_SOURCE_PROBE_BIN) $(TOOLS_DIR)/mkext2 $(DYN_NO_LIBC_EXTRA_FILES) $(FRACTINT_SVN_STAMP) $(FRACTINT_DIR)/fractint.hlp Makefile | dirs
	$(TOOLS_DIR)/mkext2 $@ $(DYN_NEG_APP_ENTRIES) $(DYN_NO_LIBC_ALL_EXTRA_ENTRIES) $(EXT2_EXTRA_DIRS)

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

$(GEN_DIR)/loader2.gen.asm: $(BOOT_DIR)/loader2.asm FORCE | dirs
	sed \
		-e "s/__STAGE2_STACK_TOP__/$(STAGE2_STACK_TOP)/" \
		-e "s/__STAGE2_STACK_TOP_32__/$(STAGE2_STACK_TOP_32)/" \
		-e "s/__FORCE_VGA_BACKEND__/$(LOADER2_FORCE_VGA_BACKEND)/" \
		-e "s/__FORCE_CHS_BOOT__/$(BOOT_FORCE_CHS)/" \
		-e "s/__VBE_DIAG__/$(BOOT_VBE_DIAG)/" \
		-e "s/__VBE_RELAXED__/$(BOOT_VBE_RELAXED)/" \
		-e "s/__RAMDISK_FALLBACK_POLICY__/$(LOADER2_RAMDISK_FALLBACK_POLICY)/" \
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

$(DYN_NO_LOADER_IMG): boot-layout-check $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(DYN_NO_LOADER_EXT2) $(TOOLS_DIR)/mkimage | dirs
	$(TOOLS_DIR)/mkimage \
		--boot $(BIN_DIR)/boot.bin \
		--loader $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fs $(DYN_NO_LOADER_EXT2) \
		--out $@ \
		--sector-size $(BOOT_SECTOR_SIZE) \
		--loader-size $(LOADER2_SIZE_BYTES) \
		--boot-partition-table-offset $(BOOT_PARTITION_TABLE_OFFSET) \
		--boot-partition-entry-size $(BOOT_PARTITION_ENTRY_SIZE)

$(DYN_NO_LIBC_IMG): boot-layout-check $(BIN_DIR)/boot.bin $(BIN_DIR)/loader2.bin $(BIN_DIR)/kernel.bin $(DYN_NO_LIBC_EXT2) $(TOOLS_DIR)/mkimage | dirs
	$(TOOLS_DIR)/mkimage \
		--boot $(BIN_DIR)/boot.bin \
		--loader $(BIN_DIR)/loader2.bin \
		--kernel $(BIN_DIR)/kernel.bin \
		--fs $(DYN_NO_LIBC_EXT2) \
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
		--kernel-load-addr $(KERNEL_OFFSET) \
		--reserved-before $(LOADER2_LOAD_ADDR) \
		--boot-partition-table-offset $(BOOT_PARTITION_TABLE_OFFSET) \
		--boot-partition-entry-size $(BOOT_PARTITION_ENTRY_SIZE)

qemu-image: image

img: image

usb-image usb-vbe-image run-usb-storage-fallback run-headless-usb-storage-fallback usb-ramdisk-fallback-smoke: BOOT_RAMDISK_FALLBACK=always
run-usb-storage run-headless-usb-storage usb-storage-smoke: BOOT_RAMDISK_FALLBACK=never

usb-image: image
	@cp "$(IMG_FILE)" "$(USB_IMG_FILE)"
	@sha256sum "$(USB_IMG_FILE)" > "$(USB_IMG_FILE).sha256"
	@printf 'USB/raw image: %s\n' "$(USB_IMG_FILE)"
	@cat "$(USB_IMG_FILE).sha256"

usb-vbe-image: image
	@printf 'USB/VBE raw image: %s\n' "$(IMG_FILE)"
	@sha256sum "$(IMG_FILE)"

vmdk: check-third-party esxi-vmdk-build

esxi-vmdk: vmdk

esxi-vmdk-build: $(ESXI_VMDK_FILE)

$(ESXI_VMDK_FILE): image-layout-check | dirs
	@if [ -n "$(ESXI_VMDK_SIZE)" ]; then \
		cp $(IMG_FILE) $(ESXI_RAW_FILE); \
		truncate -s "$(ESXI_VMDK_SIZE)" "$(ESXI_RAW_FILE)"; \
		$(QEMU_IMG) convert -f raw -O vmdk -o adapter_type=ide,subformat=monolithicSparse $(ESXI_RAW_FILE) $@; \
		rm -f $(ESXI_RAW_FILE); \
	else \
		$(QEMU_IMG) convert -f raw -O vmdk -o adapter_type=ide,subformat=monolithicSparse $(IMG_FILE) $@; \
	fi
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
CSERVE_SMOKE_CLIENTS?=24
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
QEMU_MEMORY_MB?=64
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
QEMU_USB_STORAGE_FLAGS=-drive if=none,id=stick,format=raw,file=$(IMG_FILE) \
          -device pci-ohci,id=ohci \
          -device usb-storage,bus=ohci.0,drive=stick,bootindex=1 \
          -boot order=d -m $(QEMU_MEMORY_MB) \
          -serial file:$(SERIAL_LOG) \
          $(QEMU_NETFLAGS)

.PHONY: all image img artifacts dirs deps fractint-source wolf3d-shareware-data wolf3d-source-probe check-third-party dyn-size-report dynamic-link-check dynlink-negative-smoke run run-gtk run-sdl run-tap run-headless run-headless-tap run-usb-storage run-headless-usb-storage run-usb-storage-fallback run-headless-usb-storage-fallback usb-storage-smoke usb-ramdisk-fallback-smoke test framebuffer-smoke vga-smoke gui-smoke display-smoke display-smoke-one socket-eof-smoke socket-parallel-smoke ftp-smoke ftp-loop-smoke cserve-smoke smoke smoke-reboot smoke-halt clean boot-layout-check image-layout-check qemu-image usb-image usb-vbe-image vmdk esxi-vmdk esxi-vmdk-build esxi-deploy esxi-serial-log esxi-smoke verify verify-display verify-network verify-full reset-disk tinycc-host tinycc-host-clean FORCE

FORCE:

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

run-usb-storage: image-layout-check
	$(QEMU) $(QEMU_USB_STORAGE_FLAGS) -display $(QEMU_DISPLAY)

run-headless-usb-storage: image-layout-check
	$(QEMU) $(QEMU_USB_STORAGE_FLAGS) -display $(QEMU_HEADLESS_DISPLAY) \
	    -monitor unix:/tmp/smallos-monitor.sock,server,nowait \
	    -daemonize -pidfile /tmp/smallos.pid

run-usb-storage-fallback: image-layout-check
	$(MAKE) run-usb-storage BOOT_RAMDISK_FALLBACK=always

run-headless-usb-storage-fallback: image-layout-check
	$(MAKE) run-headless-usb-storage BOOT_RAMDISK_FALLBACK=always

usb-storage-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1 BOOT_RAMDISK_FALLBACK=never
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless-usb-storage SERIAL_CONSOLE=1 BOOT_RAMDISK_FALLBACK=never
	$(PYTHON3) tools/usb_storage_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

usb-ramdisk-fallback-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1 BOOT_RAMDISK_FALLBACK=always BOOT_SKIP_USB_STORAGE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless-usb-storage SERIAL_CONSOLE=1 BOOT_RAMDISK_FALLBACK=always BOOT_SKIP_USB_STORAGE=1
	$(PYTHON3) tools/usb_storage_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT) \
		--marker "Preloading ext2 fallback..." \
		--marker "boot: PASS storage: boot ramdisk fallback" \
		--marker "boot: PASS ext2: volume mounted" \
		--marker "SmallOS ready" \
		--marker "SmallOS user shell" \
		--marker "/> "

dynlink-negative-smoke: $(DYN_NO_LOADER_IMG) $(DYN_NO_LIBC_IMG)
	@mkdir -p $(SMOKE_DIR)
	$(PYTHON3) tools/dynlink_negative_smoke.py \
		--scenario no-loader \
		--image $(DYN_NO_LOADER_IMG) \
		--qemu $(QEMU) \
		--serial $(SMOKE_DIR)/dynlink-no-loader.serial.log \
		--monitor $(SMOKE_DIR)/dynlink-no-loader.monitor.sock \
		--pidfile $(SMOKE_DIR)/dynlink-no-loader.pid \
		--memory $(QEMU_MEMORY_MB) \
		--net-mac $(QEMU_NET_MAC) \
		--timeout $(SMOKE_TIMEOUT)
	$(PYTHON3) tools/dynlink_negative_smoke.py \
		--scenario no-libc \
		--image $(DYN_NO_LIBC_IMG) \
		--qemu $(QEMU) \
		--serial $(SMOKE_DIR)/dynlink-no-libc.serial.log \
		--monitor $(SMOKE_DIR)/dynlink-no-libc.monitor.sock \
		--pidfile $(SMOKE_DIR)/dynlink-no-libc.pid \
		--memory $(QEMU_MEMORY_MB) \
		--net-mac $(QEMU_NET_MAC) \
		--timeout $(SMOKE_TIMEOUT)

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

gui-smoke:
	$(MAKE) reset-disk image-layout-check SERIAL_CONSOLE=1
	@if [ -f $(PIDFILE) ]; then kill "$$(cat $(PIDFILE))" 2>/dev/null || true; fi
	rm -f $(SERIAL_LOG) $(MONITOR_SOCK) $(PIDFILE)
	$(MAKE) run-headless SERIAL_CONSOLE=1
	$(PYTHON3) tools/gui_smoke.py \
		--monitor $(MONITOR_SOCK) \
		--serial $(SERIAL_LOG) \
		--pidfile $(PIDFILE) \
		--timeout $(SMOKE_TIMEOUT)

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

display-smoke: framebuffer-smoke vga-smoke gui-smoke

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
	$(MAKE) dynamic-link-check
	$(MAKE) dynlink-negative-smoke
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
