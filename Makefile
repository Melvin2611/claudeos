# ClaudeOS build system
#   make          – build kernel, userland, initrd and claudeos.iso
#   make run      – boot the ISO in QEMU (KVM if available) with disk, network and sound
#   make run-uefi – same, but boot through UEFI firmware (OVMF)
#   make disk     – create build/disk.img (FAT32, persistent /home) if missing
#   make clean

BUILD   := build
ISO     := claudeos.iso
CC      := gcc
LD      := ld
NASM    := nasm
GCCINC  := $(shell $(CC) -print-file-name=include)
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

BASE_CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-fcf-protection=none -march=x86-64 -mtune=generic -fno-strict-aliasing -fno-omit-frame-pointer \
	-fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-nostdinc -isystem $(GCCINC) -Icommon/include -MMD -MP

KCFLAGS := $(BASE_CFLAGS) -Wno-address-of-packed-member -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -Ikernel/include -DKERNEL
UCFLAGS := $(BASE_CFLAGS) -Iuser/libc/include -Iuser/libgui/include -DFMT_FLOAT -DUSERLAND

# ---------------------------------------------------------------- kernel
KSRC_C   := $(shell find kernel -name '*.c')
KSRC_ASM := $(shell find kernel -name '*.asm')
CSRC     := $(wildcard common/src/*.c)
KOBJ := $(KSRC_C:%.c=$(BUILD)/%.o) $(KSRC_ASM:%.asm=$(BUILD)/%.asm.o) \
        $(CSRC:common/src/%.c=$(BUILD)/kcommon/%.o)
KERNEL := $(BUILD)/kernel.elf

# ---------------------------------------------------------------- userland
LIBC_SRC  := $(wildcard user/libc/src/*.c)
LIBC_OBJ  := $(LIBC_SRC:%.c=$(BUILD)/%.o) $(CSRC:common/src/%.c=$(BUILD)/ucommon/%.o)
LIBC      := $(BUILD)/user/libc.a
CRT0      := $(BUILD)/user/libc/crt0.o
LIBGUI_SRC := $(wildcard user/libgui/src/*.c)
LIBGUI_OBJ := $(LIBGUI_SRC:%.c=$(BUILD)/%.o)
LIBGUI    := $(BUILD)/user/libgui.a

BIN_SRC   := $(wildcard user/bin/*.c)
BINS      := $(BIN_SRC:user/bin/%.c=$(BUILD)/bin/%)
APP_DIRS  := $(patsubst %/,%,$(sort $(dir $(wildcard user/apps/*/*.c))))
APPS      := $(APP_DIRS:user/apps/%=$(BUILD)/apps/%)

ROOTFS_FILES := $(shell find rootfs assets -type f 2>/dev/null)
INITRD := $(BUILD)/initrd.tar

.PHONY: all clean run run-uefi run-tcg disk iso
all: $(ISO)

# kernel objects
$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@
$(BUILD)/kernel/%.asm.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -o $@ $<
$(BUILD)/kcommon/%.o: common/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/core/assets.asm.o: $(wildcard assets/fonts/*.fnt)

$(KERNEL): $(KOBJ) kernel/linker.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld -o $@ $(KOBJ)
	@objdump -d $@ > $(BUILD)/kernel.dis 2>/dev/null || true
	@nm -n $@ | grep -i ' [tw] ' > $(BUILD)/kernel.sym || true

# userland objects
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/ucommon/%.o: common/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(CRT0): user/libc/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

$(LIBC): $(LIBC_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@ && ar rcs $@ $^
$(LIBGUI): $(LIBGUI_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@ && ar rcs $@ $^

ULINK = $(LD) -nostdlib -static -z max-page-size=0x1000 -T user/user.ld

$(BUILD)/bin/%: $(BUILD)/user/bin/%.o $(CRT0) $(LIBC) $(LIBGUI) user/user.ld
	@mkdir -p $(dir $@)
	$(ULINK) -o $@ $(CRT0) $< $(LIBGUI) $(LIBC) $(LIBGCC)
	strip -s $@

define APP_RULE
$(BUILD)/apps/$(1): $(patsubst user/%.c,$(BUILD)/user/%.o,$(wildcard user/apps/$(1)/*.c)) $(CRT0) $(LIBC) $(LIBGUI) user/user.ld
	@mkdir -p $$(dir $$@)
	$$(ULINK) -o $$@ $$(CRT0) $(patsubst user/%.c,$(BUILD)/user/%.o,$(wildcard user/apps/$(1)/*.c)) $$(LIBGUI) $$(LIBC) $$(LIBGCC)
	strip -s $$@
endef
$(foreach a,$(APP_DIRS:user/apps/%=%),$(eval $(call APP_RULE,$(a))))

# initrd: rootfs/ + generated assets + binaries
$(INITRD): $(BINS) $(APPS) $(ROOTFS_FILES)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/system
	cp -r rootfs/. $(BUILD)/rootfs/
	cp -r assets/. $(BUILD)/rootfs/system/
	$(if $(strip $(BINS) $(APPS)),cp $(BINS) $(APPS) $(BUILD)/rootfs/bin/)
	tar --format=ustar --owner=0 --group=0 -cf $@ -C $(BUILD)/rootfs .

$(ISO): $(KERNEL) $(INITRD) grub/grub.cfg
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/kernel.elf
	cp $(INITRD) $(BUILD)/iso/boot/initrd.tar
	cp grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2> $(BUILD)/grub-mkrescue.log || (cat $(BUILD)/grub-mkrescue.log; false)
	python3 tools/fix-efi-nx.py $@
	@echo "==> $@ ready"

iso: $(ISO)

# ---------------------------------------------------------------- run
DISK := $(BUILD)/disk.img
disk: $(DISK)
$(DISK):
	@mkdir -p $(BUILD)
	truncate -s 512M $@
	mkfs.fat -F 32 -s 8 -n CLAUDEOS $@ > /dev/null

KVM := $(shell test -w /dev/kvm && echo "-enable-kvm -cpu host")
AUDIO ?= pipewire
QEMU_FLAGS := -machine pc -m 1G -vga std -rtc base=localtime \
	-drive file=$(DISK),format=raw,if=ide,index=0 -cdrom $(ISO) -boot d \
	-nic user,model=e1000 -audiodev $(AUDIO),id=snd0 -device AC97,audiodev=snd0 \
	-serial stdio

run: $(ISO) $(DISK)
	qemu-system-x86_64 $(KVM) $(QEMU_FLAGS)

run-tcg: $(ISO) $(DISK)
	qemu-system-x86_64 $(QEMU_FLAGS)

OVMF_CODE := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS := /usr/share/edk2/x64/OVMF_VARS.4m.fd
run-uefi: $(ISO) $(DISK)
	cp -n $(OVMF_VARS) $(BUILD)/ovmf_vars.fd 2>/dev/null || true
	qemu-system-x86_64 $(KVM) $(QEMU_FLAGS) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/kcommon $(BUILD)/ucommon $(BUILD)/user $(BUILD)/bin \
	       $(BUILD)/rootfs $(BUILD)/iso $(BUILD)/*.elf $(BUILD)/*.tar $(BUILD)/*.dis $(BUILD)/*.sym $(ISO)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
