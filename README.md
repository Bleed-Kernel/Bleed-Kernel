
# Bleed Kernel
A 64-bit Operating System in C using the Limine Bootloader for UEFI and BIOS systems

## Roadmap
- [x]  GDT
- [x]  IDT
- [x]  Exception Handler
- [x]  PMM
- [x]  Vaddr Paddr Conversion
- [x]  Paging Allocation
- [x]  TempFS + VFS
- [x]  initrd
- [x]  PIC
- [x]  PIT
- [x]  Schedular (round robin)
- [x]  Review Kernel Code (Quality Assurance)
- [x]  Kernel Thread Context Switcher
- [x]  ELF Loading
- [x]  ACPI
- [ ]  APIC
- [ ]  HPET
- [ ]  SMP

ill plan more as i go along!

## Run Locally

Virtualise Locally using QEMU (BIOS)
```
make run
```

If you want to specifically use UEFI
```
make run-uefi
```

you can only make the iso without running by doing
```
make all
```

and clean it all up with
```
make clean
```

Note: the Git commit history is a bit weird becuase im learning and getting used to Git on a cli, sorry </3
