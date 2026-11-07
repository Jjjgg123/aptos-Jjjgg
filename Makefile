PREFIX ?= i686-elf-

CC      := $(PREFIX)gcc
LD      := $(PREFIX)ld
NASM    ?= nasm
OBJCOPY ?= $(PREFIX)objcopy
PYTHON  ?= python3

BUILD   := build
IMG     := $(BUILD)/os.img
MEM     ?= 128
MINI_MB ?= 8
FLOPPY_KB ?= 1440

CFLAGS  := -m32 -march=i386 -mtune=i386 -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -mno-sse -mno-mmx -mno-80387 -mno-red-zone \
           -Ikernel
LDFLAGS := -m elf_i386 -T linker.ld --build-id=none

CSRC    := $(wildcard kernel/*.c)
ASMSRC  := $(wildcard kernel/*.asm)
OBJS    := $(patsubst kernel/%.c,$(BUILD)/%.o,$(CSRC)) \
           $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ASMSRC))

APP_CFLAGS := -m32 -march=i386 -mtune=i386 -std=gnu11 -O2 -Wall -ffreestanding -nostdlib -fno-pie \
              -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
              -fno-unwind-tables -mno-sse -mno-mmx -mno-80387 -Iapps/lib
APPS       := installer hello guess freecell spider
SETUP_APPS := setup

QEMU    := qemu-system-i386
QFLAGS  := -m $(MEM) -vga std -drive file=$(IMG),format=raw,index=0,media=disk

.PHONY: all run test mini floppy apps iso clean tools

all: $(IMG)

$(BUILD)/stage1.bin: boot/stage1.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@$(PYTHON) tools/check_kernel.py $@ $(PREFIX)nm
	@echo "--- kernel sections ---"
	@$(PREFIX)objdump -h $@ | awk 'NR>4 && $$2!="" {printf "    %-12s %8d bytes\n", $$2, strtonum("0x"$$3)}' || true

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "kernel.bin: $$(stat -c%s $@) bytes"

$(BUILD)/app_start.o: apps/lib/start.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/app_lib.o: apps/lib/aptos.c | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/%.app: apps/%.c $(BUILD)/app_start.o $(BUILD)/app_lib.o apps/linker.ld
	$(CC) $(APP_CFLAGS) -c $< -o $(BUILD)/$*.app.o
	$(LD) -m elf_i386 -T apps/linker.ld --build-id=none -o $@ \
	    $(BUILD)/app_start.o $(BUILD)/app_lib.o $(BUILD)/$*.app.o
	@echo "  app $@: $$(stat -c%s $@) bytes"

APP_TARGETS := $(addprefix $(BUILD)/,$(addsuffix .app,$(APPS)))

$(BUILD)/setup.app: apps/setup.c $(BUILD)/app_start.o $(BUILD)/app_lib.o apps/linker.ld
	$(CC) $(APP_CFLAGS) -c $< -o $(BUILD)/setup.app.o
	$(LD) -m elf_i386 -T apps/linker.ld --build-id=none -o $@ \
	    $(BUILD)/app_start.o $(BUILD)/app_lib.o $(BUILD)/setup.app.o
	@echo "  app $@: $$(stat -c%s $@) bytes"

$(BUILD)/setup.lst: tools/mklst.py $(BUILD)/setup.app
	$(PYTHON) tools/mklst.py --out $@ --name "aptos 2.0 setup" --vendor aptos \
	    --description "system installer" --installer setup \
	    --app setup=$(BUILD)/setup.app

$(BUILD)/SYSTEM.ERT: tools/mkert.py $(BUILD)/kernel.bin $(IMG)
	$(PYTHON) tools/mkert.py --out $@ --disk $(IMG) --kernel $(BUILD)/kernel.bin

$(BUILD)/aptos-install.iso: tools/mkcd.py $(BUILD)/boot.img $(BUILD)/SYSTEM.ERT $(BUILD)/setup.lst
	$(PYTHON) tools/mkcd.py --out $@ --boot $(BUILD)/boot.img --media floppy144 \
	    --volume APTOS_SETUP \
	    --file /KERNEL.ELF=$(BUILD)/kernel.bin \
	    --file /SETUP.LST=$(BUILD)/setup.lst \
	    --file /SYSTEM.ERT=$(BUILD)/SYSTEM.ERT

$(BUILD)/boot.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimg.py
	$(PYTHON) tools/mkimg.py --out $@ --boot-only --size-kb 1440 \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(BUILD)/kernel.bin

iso: $(BUILD)/aptos-install.iso

$(BUILD)/apps.lst: tools/mklst.py $(APP_TARGETS)
	$(PYTHON) tools/mklst.py --out $@ --name "aptos starter pack" --vendor aptos \
	    --description "installer + demo apps" --installer installer \
	    $(foreach a,$(APPS),--app $(a)=$(BUILD)/$(a).app)

apps: $(BUILD)/apps.lst

$(IMG): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimg.py $(BUILD)/apps.lst $(BUILD)/setup.lst
	$(PYTHON) tools/mkimg.py --out $@ \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(BUILD)/kernel.bin --apps $(BUILD)/apps.lst \
	    --add /SETUP.LST=$(BUILD)/setup.lst \
	    $(foreach a,$(APPS),--install $(a)=$(BUILD)/$(a).app)

$(BUILD)/os-mini.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimg.py $(BUILD)/apps.lst
	$(PYTHON) tools/mkimg.py --out $@ \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(BUILD)/kernel.bin --size-mb $(MINI_MB) \
	    --apps $(BUILD)/apps.lst \
	    $(foreach a,$(APPS),--install $(a)=$(BUILD)/$(a).app)

$(BUILD)/floppy.img: tools/mkimg.py | $(BUILD)
	$(PYTHON) tools/mkimg.py --out $@ --fs-only --size-kb $(FLOPPY_KB) --label FDISK

mini: $(BUILD)/os-mini.img
floppy: $(BUILD)/floppy.img

$(BUILD):
	@mkdir -p $(BUILD)

run: $(IMG)
	$(QEMU) $(QFLAGS) -serial stdio

QEMU_ARGS ?= $(if $(wildcard /opt/tc/usr/share/qemu/bios-256k.bin),-L /opt/tc/usr/share/qemu,)

test: $(IMG)
	$(PYTHON) tools/qemu_test.py $(IMG) $(foreach a,$(QEMU_ARGS),--qemu-arg=$(a)) \
	    --step "delay:9" --step "type:ls" --step "key:ret" --step "delay:2" \
	    --step "type:cat HELLO.TXT" --step "key:ret" --step "delay:2" \
	    --step "shot:$(BUILD)/screen.png" --serial $(BUILD)/serial.log

clean:
	rm -rf $(BUILD)

-include $(BUILD)/*.d
