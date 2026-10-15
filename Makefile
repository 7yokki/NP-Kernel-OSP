PROJECT ?= npkernel
ifeq ($(strip $(PROJECT)),)
$(error PROJECT must not be empty)
endif
NPK_ENABLE_USER_DEMO ?= 0
NPK_ENABLE_USER_SHELL ?= 1
USER_PAYLOAD ?= asm
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
CFLAGS += -DNPK_ENABLE_USER_DEMO=$(NPK_ENABLE_USER_DEMO) -DNPK_ENABLE_USER_SHELL=$(NPK_ENABLE_USER_SHELL)
LDFLAGS := -m elf_x86_64 -nostdlib -z max-page-size=0x1000 --gc-sections -T linker.ld

C_SOURCES := $(shell find boot src arch/x86_64 -name '*.c')
ASM_SOURCES := $(shell find arch/x86_64 -name '*.asm')
C_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SOURCES))
KERNEL := $(BUILD)/$(PROJECT).elf
ISO_ROOT := $(BUILD)/iso_root
ISO := $(BUILD)/$(PROJECT).iso
LIMINE_CONFIG := $(BUILD)/limine.conf

.PHONY: all clean limine initramfs iso disk run run-disk run-uefi debug check test-elf-corpus test-device-manager test-usb-keyboard test-usb-qemu test-bash-qemu test-user-libc

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
USER_C_OBJ := $(BUILD)/user/user_exit.o
MMAP_OBJ := $(BUILD)/user/mmap.o
MMAP_ELF := $(BUILD)/user/mmap.elf
FORK_OBJ := $(BUILD)/user/fork.o
FORK_ELF := $(BUILD)/user/fork.elf
USER_BASH_START_OBJ := $(BUILD)/user/bash_start.o
USER_BASH_ELF := $(BUILD)/user/bash.elf
USER_LIBC_SOURCES := user/libc/syscall.c user/libc/string.c user/libc/unistd.c user/libc/stdlib.c user/libc/stdio.c user/libc/dirent.c user/libc/uname.c
USER_LIBC_OBJECTS := $(patsubst user/%.c,$(BUILD)/user/%.o,$(USER_LIBC_SOURCES))
USER_BASH_OBJ := $(BUILD)/user/bash.o
USER_BASH_OBJECTS := $(USER_BASH_OBJ) $(USER_LIBC_OBJECTS)
USER_CFLAGS := --target=x86_64-unknown-none-elf -std=c17 -O2 -g3 -ffreestanding -fno-builtin \
               -fno-stack-protector -fno-stack-check -fno-asynchronous-unwind-tables \
               -fno-unwind-tables -fno-pie -fno-plt -mno-red-zone -mno-sse -mno-sse2 \
               -mno-mmx -mno-80387 -Wall -Wextra -Werror -Iuser/include -Iinclude
INITRAMFS := $(BUILD)/initramfs.cpio
DISK := $(BUILD)/npkernel-disk.img

ifeq ($(USER_PAYLOAD),c-exit)
$(USER_C_OBJ): tests/user_exit.c
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-none-elf -std=c17 -O2 -g -ffreestanding \
		-fno-stack-protector -fno-pie -fno-plt -mno-red-zone -mno-sse \
		-mno-sse2 -mno-mmx -mno-80387 -Wall -Wextra -Werror -c $< -o $@
$(USER_ELF): $(USER_C_OBJ) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T user/linker.ld $(USER_C_OBJ) -o $@
else
$(USER_OBJ): user/hello.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@
$(USER_ELF): $(USER_OBJ) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T user/linker.ld $(USER_OBJ) -o $@
endif

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

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(USER_BASH_START_OBJ): user/bash_start.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf $< -o $@
$(USER_BASH_ELF): $(USER_BASH_START_OBJ) $(USER_BASH_OBJECTS) user/linker.ld
	$(LD) -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T user/linker.ld $(USER_BASH_START_OBJ) $(USER_BASH_OBJECTS) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf $< -o $@

$(INITRAMFS): tools/make_initramfs.py user/ddc.json $(USER_ELF) $(MMAP_ELF) $(FORK_ELF) $(USER_BASH_ELF) $(shell find initramfs -type f)
	python3 tools/make_initramfs.py $(INITRAMFS)

initramfs: $(INITRAMFS)
disk: $(DISK)
$(DISK): | $(BUILD)
	qemu-img create -f raw $@ 32M

$(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(LIMINE_DIR)/BOOTX64.EFI: $(LIMINE_DIR)/limine

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
	qemu-system-x86_64 -M q35 -m 512M -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown
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

$(BUILD)/device_manager_host: src/device/device_manager.c src/device/device_config.c tests/device_manager_host.c include/npk/device_manager.h include/npk/device_config.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c17 -O2 -fno-builtin -Wall -Wextra -Werror -Iinclude src/device/device_manager.c src/device/device_config.c tests/device_manager_host.c -o $@
test-device-manager: $(BUILD)/device_manager_host
	$(BUILD)/device_manager_host

$(BUILD)/usb_generic_keyboard_host: src/drivers/usb_generic_keyboard.c tests/usb_generic_keyboard_host.c include/npk/usb_generic_keyboard.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c17 -O2 -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Iinclude src/drivers/usb_generic_keyboard.c tests/usb_generic_keyboard_host.c -Wl,--gc-sections -o $@
test-usb-keyboard: $(BUILD)/usb_generic_keyboard_host
	$(BUILD)/usb_generic_keyboard_host
test-usb-qemu: $(ISO)
	python3 tests/qemu_usb_smoke.py $(ISO)
test-bash-qemu: $(ISO)
	python3 tests/qemu_bash_smoke.py $(ISO)

$(BUILD)/user_libc_host: user/libc/stdlib.c user/libc/stdio.c user/libc/string.c tests/libc_host.c tests/libc_host_rename.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c17 -O2 -fno-builtin -Wall -Wextra -Werror -Iuser/include -Iinclude -include tests/libc_host_rename.h user/libc/stdlib.c user/libc/stdio.c user/libc/string.c tests/libc_host.c -o $@
test-user-libc: $(BUILD)/user_libc_host
	$(BUILD)/user_libc_host
	@echo "USER_LIBC_TESTS passed"

clean:
	rm -rf $(BUILD)

-include $(C_OBJECTS:.o=.d) $(USER_BASH_OBJECTS:.o=.d)
