# The Bleed Kernel by Mellurboo

the bleed kernel is an Operating System Kernel written in C and targets the x86_64 platform exclusively.

![BleedKernel Booting](https://github.com/Bleed-Kernel/Bleed-Resources/blob/main/Bleed-Booting.png?raw=true)

## Requirements
The System Requirements for Bleed are Rather Slim, it's a simple operating system.
**Your Must have**
- An x86_64 Processor (No 32-Bit Support)
- At Minimum 512MiB of Memory
- ACPI-Compliant firmware with HPET
- IOAPIC Support
- SSE4.2 Support
  
**Recommended** not required but Bleed Will Use It:
- User-Mode Instruction Prevention (UMIP)
- Supervisor Memory Access Protection (SMAP)

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
- [x] PS/2 Mouse Drivers
- [x] FAT32 Support
- [x] Ext2 Support
- [x] IDE Harddrive Support
- [x] SATA Support
- [x] NVMe Support
- [x] Mount Points for Block Devices

## Upcomming Features
- [ ] xHCI (HID)
- [ ] Relocatable Executables
- [ ] Network Drivers
- [ ] Symetric Multiprocessing (SMP)
- [ ] Task-Specific Framebuffers

![Verdict Shell Splash](https://github.com/Bleed-Kernel/Bleed-Resources/blob/main/Bleed-Verdict-Shell.png?raw=true)

## Hardware Specifications
I use many machines to test bleed on real hardware so I know it works the way it should.

The Dedicated test machine for the bleed kernel is a Dell Optiplex 3040 with an i5-6500 and 4 GiB of Memory, however the machine I
use to make bleed and often also test on is a Custom Build with a Ryzen 5 9600x and 16 GiB of Memory.

## Related Projects
[The Verdict Shell - Default shell for the Bleed Kernel](https://codeberg.org/Bleed-Kernel/Verdict-Shell)

[libc interface for the Bleed Kernel](https://codeberg.org/Bleed-Kernel/blibc)
