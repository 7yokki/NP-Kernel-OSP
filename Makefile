PROJECT ?= npkernel
ifeq ($(strip $(PROJECT)),)
$(error PROJECT must not be empty)
endif
BUILD := build
LIMINE_DIR := $(BUILD)/limine
LIMINE_BRANCH := v9.x-binary
CC := clang
HOSTCC ?= clang
LD := ld.lld
OBJCOPY := objcopy
NASM := nasm

CFLAGS := --target=x86_64-unknown-none-elf -std=c17 -O2 -g3 \
          -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-pie -fno-plt -mno-red-zone -mcmodel=kernel \
          -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
          -Wall -Wextra -Werror -Iinclude
LDFLAGS := -m elf_x86_64 -nostdlib -z max-page-size=0x1000 --gc-sections -T linker.ld

C_SOURCES := $(shell find boot src arch/x86_64 -name '*.c')
ASM_SOURCES := $(shell find arch/x86_64 -name '*.asm')
C_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SOURCES))
KERNEL := $(BUILD)/$(PROJECT).elf
ISO_ROOT := $(BUILD)/iso_root
ISO := $(BUILD)/$(PROJECT).iso
LIMINE_CONFIG := $(BUILD)/limine.conf

.PHONY: all clean limine initramfs iso disk run run-disk run-uefi debug check test-elf-corpus

$(LIMINE_CONFIG): limine.conf | $(BUILD)
	@sed 's@/boot/npkernel\\.elf@/boot/$(PROJECT).elf@g' $< > $@

all: $(ISO)

$(BUILD):
	@mkdir -p $(BUILD)

$(LIMINE_DIR): | $(BUILD)
	git clone --depth=1 --branch $(LIMINE_BRANCH) https://github.com/limine-bootloader/limine.git $@

limine: $(LIMINE_DIR)

$(LIMINE_DIR)/limine: $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR) limine

USER_OBJ := $(BUILD)/user/hello.o
USER_ELF := $(BUILD)/user/hello.elf
MMAP_OBJ := $(BUILD)/user/mmap.o
MMAP_ELF := $(BUILD)/user/mmap.elf
FORK_OBJ := $(BUILD)/user/fork.o
FORK_ELF := $(BUILD)/user/fork.elf
INITRAMFS := $(BUILD)/initramfs.cpio
DISK := $(BUILD)/npkernel-disk.img

$(USER_OBJ): user/hello.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(USER_ELF): $(USER_OBJ) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T user/linker.ld $(USER_OBJ) -o $@

$(MMAP_OBJ): user/mmap.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(MMAP_ELF): $(MMAP_OBJ) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T user/linker.ld $(MMAP_OBJ) -o $@

$(FORK_OBJ): user/fork.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(FORK_ELF): $(FORK_OBJ) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T user/linker.ld $(FORK_OBJ) -o $@

$(INITRAMFS): tools/make_initramfs.py $(USER_ELF) $(MMAP_ELF) $(FORK_ELF) $(shell find initramfs -type f)
	python3 tools/make_initramfs.py $(INITRAMFS)

initramfs: $(INITRAMFS)

disk: $(DISK)

$(DISK): | $(BUILD)
	qemu-img create -f raw $@ 32M

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf $< -o $@

$(KERNEL): $(C_OBJECTS) $(ASM_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) $(C_OBJECTS) $(ASM_OBJECTS) -o $@
	$(OBJCOPY) --only-keep-debug $@ $(BUILD)/$(PROJECT).debug

$(ISO): $(KERNEL) $(LIMINE_DIR)/limine $(INITRAMFS) $(LIMINE_CONFIG)
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/$(PROJECT).elf
	cp $(INITRAMFS) $(ISO_ROOT)/boot/initramfs.cpio
	cp $(LIMINE_CONFIG) $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part \
		--efi-boot-image --protective-msdos-label $(ISO_ROOT) -o $@
	$(LIMINE_DIR)/limine bios-install $@

iso: $(ISO)

run: $(ISO)
	qemu-system-x86_64 -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

run-disk: $(ISO) $(DISK)
	qemu-system-x86_64 -M q35 -m 512M -cdrom $(ISO) -drive if=ide,format=raw,file=$(DISK) -serial stdio -no-reboot -no-shutdown

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd

run-uefi: $(ISO)
	qemu-system-x86_64 -M q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

debug: $(ISO)
	qemu-system-x86_64 -M q35 -m 512M -cdrom $(ISO) -serial stdio -S -gdb tcp::1234

check: $(KERNEL)
	readelf -h $(KERNEL)
	readelf -S $(KERNEL)
	nm -n $(KERNEL) | tail -30

test-elf-corpus: $(USER_ELF)
	@mkdir -p $(BUILD)/adversarial-elf
	python3 tools/elf_adversarial.py $(USER_ELF) $(BUILD)/adversarial-elf
	$(HOSTCC) -std=c17 -O2 -ffunction-sections -fdata-sections -Wno-incompatible-library-redeclaration -Iinclude -c src/exec/elf.c -o $(BUILD)/elf-host.o
	$(HOSTCC) -std=c17 -O2 -ffunction-sections -fdata-sections -Iinclude -c tests/elf_validate_host.c -o $(BUILD)/elf_validate_host.o
	$(HOSTCC) -Wl,--gc-sections $(BUILD)/elf-host.o $(BUILD)/elf_validate_host.o -o $(BUILD)/elf_validate_host
	$(BUILD)/elf_validate_host $(BUILD)/adversarial-elf/manifest.tsv

clean:
	rm -rf $(BUILD)

-include $(C_OBJECTS:.o=.d)
