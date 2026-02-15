# Makefile for Alteo x64 (ISO Graphics Edition)

AS = nasm
CC = gcc
LD = ld
OBJCOPY = objcopy

# Flags
ASFLAGS = -f elf64
CFLAGS = -m64 -ffreestanding -mno-red-zone -fno-builtin -fno-exceptions \
         -O2 -Wall -Wextra -nostdlib -nostdinc -fno-pic \
         -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mcmodel=kernel

# Flags for GPU files that use floating-point (SSE enabled for these files only)
# These files must not be called from interrupt handlers without SSE state save
GPU_CFLAGS = -m64 -ffreestanding -mno-red-zone -fno-builtin -fno-exceptions \
             -O2 -Wall -Wextra -nostdlib -nostdinc -fno-pic \
             -mno-mmx -mcmodel=kernel

LDFLAGS = -m elf_x86_64 -n -T linker.ld -nostdlib

# Object Files
OBJS = boot.o kernel.o keyboard.o mouse.o pmm.o heap.o graphics.o font.o \
       idt.o idt_asm.o isr.o isr_asm.o irq.o irq_asm.o \
       process.o scheduler.o syscall.o vfs.o ata.o fat32.o \
       e1000.o ethernet.o ip.o tcp.o socket.o ac97.o \
       gdt.o gdt_asm.o vmm.o switch.o elf.o pe.o \
       pci.o acpi.o apic.o xhci.o usb.o usb_hid.o blkdev.o \
       pipe.o signal.o shm.o devfs.o procfs.o ext2.o \
       gpu.o nv_display.o nv_2d.o nv_fifo.o nv_3d.o nv_mem.o nv_power.o \
       opengl.o compositor.o

all: alteo.iso

# Compile Rules
boot.o: boot.asm
	$(AS) $(ASFLAGS) boot.asm -o boot.o

idt_asm.o: idt_asm.asm
	$(AS) $(ASFLAGS) idt_asm.asm -o idt_asm.o

isr_asm.o: isr_asm.asm
	$(AS) $(ASFLAGS) isr_asm.asm -o isr_asm.o

irq_asm.o: irq_asm.asm
	$(AS) $(ASFLAGS) irq_asm.asm -o irq_asm.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

idt.o: idt.c
	$(CC) $(CFLAGS) -c idt.c -o idt.o

isr.o: isr.c
	$(CC) $(CFLAGS) -c isr.c -o isr.o

irq.o: irq.c
	$(CC) $(CFLAGS) -c irq.c -o irq.o

keyboard.o: keyboard.c
	$(CC) $(CFLAGS) -c keyboard.c -o keyboard.o

mouse.o: mouse.c
	$(CC) $(CFLAGS) -c mouse.c -o mouse.o

pmm.o: pmm.c
	$(CC) $(CFLAGS) -c pmm.c -o pmm.o

heap.o: heap.c
	$(CC) $(CFLAGS) -c heap.c -o heap.o

graphics.o: graphics.c
	$(CC) $(CFLAGS) -c graphics.c -o graphics.o

font.o: font.c
	$(CC) $(CFLAGS) -c font.c -o font.o

process.o: process.c
	$(CC) $(CFLAGS) -c process.c -o process.o

scheduler.o: scheduler.c
	$(CC) $(CFLAGS) -c scheduler.c -o scheduler.o

syscall.o: syscall.c
	$(CC) $(CFLAGS) -c syscall.c -o syscall.o

vfs.o: vfs.c
	$(CC) $(CFLAGS) -c vfs.c -o vfs.o

ata.o: ata.c
	$(CC) $(CFLAGS) -c ata.c -o ata.o

fat32.o: fat32.c
	$(CC) $(CFLAGS) -c fat32.c -o fat32.o

gdt.o: gdt.c
	$(CC) $(CFLAGS) -c gdt.c -o gdt.o

gdt_asm.o: gdt_asm.asm
	$(AS) $(ASFLAGS) gdt_asm.asm -o gdt_asm.o

vmm.o: vmm.c
	$(CC) $(CFLAGS) -c vmm.c -o vmm.o

switch.o: switch.asm
	$(AS) $(ASFLAGS) switch.asm -o switch.o

elf.o: elf.c
	$(CC) $(CFLAGS) -c elf.c -o elf.o

pe.o: pe.c
	$(CC) $(CFLAGS) -c pe.c -o pe.o

pci.o: pci.c
	$(CC) $(CFLAGS) -c pci.c -o pci.o

acpi.o: acpi.c
	$(CC) $(CFLAGS) -c acpi.c -o acpi.o

apic.o: apic.c
	$(CC) $(CFLAGS) -c apic.c -o apic.o

xhci.o: xhci.c
	$(CC) $(CFLAGS) -c xhci.c -o xhci.o

usb.o: usb.c
	$(CC) $(CFLAGS) -c usb.c -o usb.o

usb_hid.o: usb_hid.c
	$(CC) $(CFLAGS) -c usb_hid.c -o usb_hid.o

blkdev.o: blkdev.c
	$(CC) $(CFLAGS) -c blkdev.c -o blkdev.o

pipe.o: pipe.c
	$(CC) $(CFLAGS) -c pipe.c -o pipe.o

signal.o: signal.c
	$(CC) $(CFLAGS) -c signal.c -o signal.o

shm.o: shm.c
	$(CC) $(CFLAGS) -c shm.c -o shm.o

devfs.o: devfs.c
	$(CC) $(CFLAGS) -c devfs.c -o devfs.o

procfs.o: procfs.c
	$(CC) $(CFLAGS) -c procfs.c -o procfs.o

ext2.o: ext2.c
	$(CC) $(CFLAGS) -c ext2.c -o ext2.o

gpu.o: gpu.c
	$(CC) $(CFLAGS) -c gpu.c -o gpu.o

nv_display.o: nv_display.c
	$(CC) $(GPU_CFLAGS) -c nv_display.c -o nv_display.o

nv_2d.o: nv_2d.c
	$(CC) $(CFLAGS) -c nv_2d.c -o nv_2d.o

nv_fifo.o: nv_fifo.c
	$(CC) $(CFLAGS) -c nv_fifo.c -o nv_fifo.o

nv_3d.o: nv_3d.c
	$(CC) $(GPU_CFLAGS) -c nv_3d.c -o nv_3d.o

nv_mem.o: nv_mem.c
	$(CC) $(CFLAGS) -c nv_mem.c -o nv_mem.o

nv_power.o: nv_power.c
	$(CC) $(GPU_CFLAGS) -c nv_power.c -o nv_power.o

opengl.o: opengl.c
	$(CC) $(GPU_CFLAGS) -c opengl.c -o opengl.o

compositor.o: compositor.c
	$(CC) $(GPU_CFLAGS) -c compositor.c -o compositor.o

# Link Kernel
kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o kernel.bin

# Build ISO (Crucial Step)
alteo.iso: kernel.bin grub.cfg
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o alteo.iso isodir
	@echo "ISO Created Successfully: alteo.iso"

# Run ISO
# -boot d: Force boot from CD-ROM
# -m 512M: Give it enough RAM
run: alteo.iso
	qemu-system-x86_64 -cdrom alteo.iso -boot d -m 512M -vga std

clean:
	rm -f *.o kernel.bin alteo.iso
	rm -rf isodir

.PHONY: all run clean