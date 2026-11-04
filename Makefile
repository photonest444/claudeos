# ClaudeOS build system.
#   make            -> build/claudeos.iso (bootable, GRUB + kernel + initrd)
#   make run        -> boot the ISO in QEMU
#   make run-disk   -> same, plus a persistent FAT16 hard disk (disk.img)
#   make clean
#
# A host gcc with -m32 support is enough; if an i686-elf cross compiler is
# installed it is used automatically (make CROSS=i686-elf- to force).

ifeq ($(shell command -v i686-elf-gcc 2>/dev/null),)
CROSS ?=
else
CROSS ?= i686-elf-
endif

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AS      := nasm
PYTHON  ?= python3
QEMU    ?= qemu-system-i386
GRUB_MKRESCUE ?= $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || echo grub-mkrescue)

BUILD   := build
ISO     := $(BUILD)/claudeos.iso
KERNEL  := $(BUILD)/kernel.elf
INITRD  := $(BUILD)/initrd.img
DISK    := disk.img

CFLAGS_COMMON := -m32 -std=gnu11 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
                 -fno-asynchronous-unwind-tables -mgeneral-regs-only -O2 -Wall -Wextra \
                 -Wno-unused-parameter -g
KCFLAGS := $(CFLAGS_COMMON)
UCFLAGS := $(CFLAGS_COMMON)
LDFLAGS := -m elf_i386 -nostdlib -z noexecstack

# ---- kernel ---------------------------------------------------------------
KERNEL_C   := $(shell find kernel -name '*.c') common/format.c
KERNEL_ASM := $(shell find kernel -name '*.asm')
KERNEL_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C)) $(patsubst %.asm,$(BUILD)/%.o,$(KERNEL_ASM))
KERNEL_HDR := $(shell find kernel/include common -name '*.h')

# ---- user programs ----------------------------------------------------------
USER_APPS  := $(basename $(notdir $(wildcard user/apps/*.c)))
USER_BINS  := $(addprefix $(BUILD)/user/,$(USER_APPS))
ULIB_OBJ   := $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/libc.o $(BUILD)/user/lib/format.o

.PHONY: all iso run run-disk run-serial debug test disk clean
all: iso
iso: $(ISO)

$(BUILD)/kernel/%.o: kernel/%.c $(KERNEL_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/common/%.o: common/%.c common/format.h
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 $< -o $@

$(KERNEL): $(KERNEL_OBJ) kernel/linker.ld
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KERNEL_OBJ)

$(BUILD)/user/lib/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user/lib/libc.o: user/lib/libc.c user/lib/claudeos.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/lib/format.o: common/format.c common/format.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/apps/%.c user/lib/claudeos.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%: $(BUILD)/user/%.o $(ULIB_OBJ) user/linker.ld
	$(LD) $(LDFLAGS) -s -T user/linker.ld -o $@ $(ULIB_OBJ) $<

# ---- file system images -------------------------------------------------------
$(BUILD)/rootfs.stamp: $(USER_BINS) $(shell find rootfs -type f)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin
	cp -r rootfs/. $(BUILD)/rootfs/
	cp $(USER_BINS) $(BUILD)/rootfs/bin/
	touch $@

$(INITRD): $(BUILD)/rootfs.stamp tools/mkfat.py
	$(PYTHON) tools/mkfat.py $(BUILD)/rootfs $@ --size-kb 2048 --fat 12 --label CLAUDEOS

# Persistent hard disk (FAT16, 32 MB). Created once; delete it to reset.
$(DISK): | $(BUILD)/rootfs.stamp
	$(PYTHON) tools/mkfat.py $(BUILD)/rootfs $@ --size-kb 32768 --fat 16 --label CLAUDEHD
disk: $(DISK)

# ---- ISO ------------------------------------------------------------------------
$(ISO): $(KERNEL) $(INITRD) grub/grub.cfg
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/kernel.elf
	cp $(INITRD) $(BUILD)/isodir/boot/initrd.img
	cp grub/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/isodir 2>&1 | grep -v '^xorriso : NOTE' || true
	@test -f $@ && echo "==> $@ ready"

# ---- running ---------------------------------------------------------------------
QEMU_FLAGS := -m 256M -serial stdio -no-reboot

run: $(ISO)
	$(QEMU) -cdrom $(ISO) $(QEMU_FLAGS)

run-disk: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -boot d $(QEMU_FLAGS)

# Headless: the CLI is mirrored to the terminal through the serial port.
run-serial: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 256M -serial stdio -display none -no-reboot

# Wait for GDB on :1234 -> gdb build/kernel.elf -ex 'target remote :1234'
debug: $(ISO)
	$(QEMU) -cdrom $(ISO) $(QEMU_FLAGS) -s -S

# Headless smoke test through the serial console.
test: $(ISO)
	$(PYTHON) tools/smoketest.py $(ISO)

clean:
	rm -rf $(BUILD)
