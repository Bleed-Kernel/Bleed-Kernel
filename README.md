- The Bleed Kernel by Mellurboo
  -
the bleed kernel is an Operating System Kernel written in C and targets the x86_64 platform exclusively.

![Doom running under the bleed kernel](https://cdn.discordapp.com/attachments/1250900206543700138/1473111046397886625/image.png?ex=69950512&is=6993b392&hm=4b7d92854b79d97b8291549d0f2987d8ab7060de777d313befd55dfcc954ec59&)

## Requirements
Generally any reasonable hardware should have this.
- CPU - 64-bit Processor*
- RAM - <128MiB
- IOAPIC
- HPET

*your processor must support
- AVX2 (XSAVE, XGETBV, XSETBV)
- SMAP (clac, stac)
- APIC
- PAT
- UMIP (optional, the kernel will use it if it's there)

## Bleed Features and why its so cool
- [x] General Hardware Security Featuees (UMIP, NX, WP)
- [x] Mathmatical Features speeding up memory operations (AVX2/SSE)
- [x] Crash Signature for easy debugging
- [x] ACPI Power and Timer Stack using the HPET
- [x] IOACPI for Device Interrupts (over the common and obsolete PIT)
- [x] Kernel and User Memory Management including Seperation
- [x] Round Robin Premptive Scheduler for Multitasking + Context Switching
- [x] ELF (non-relocatable) User Program Execution (yes, it can run doom)
- [x] Unix-Like Devices and Device Handler

## Upcomming Features
- [ ] Network Drivers
- [ ] Symetric Multiprocessing (SMP)
- [ ] PS/2 Mouse Drivers
- [ ] xHCI (HID)
- [ ] Relocatable Executables
- [ ] Task-Specific Framebuffers

## Related Projects
[The Verdict Shell - Default shell for the Bleed Kernel](https://codeberg.org/Bleed-Kernel/Verdict-Shell)

[libc interface for the Bleed Kernel](https://codeberg.org/Bleed-Kernel/blibc)