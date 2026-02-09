#include <stdint.h>
#include <stdio.h>
#include <ansii.h>
#include <drivers/serial/serial.h>
#include <cpu/stack_trace.h>
#include <drivers/framebuffer/framebuffer.h>
#include <sched/scheduler.h>

static int faulted = 0;

struct isr_stackframe {
    uint64_t rax; uint64_t rbx; uint64_t rcx; uint64_t rdx;
    uint64_t rsi; uint64_t rdi; uint64_t rbp;
    uint64_t r8;  uint64_t r9;  uint64_t r10; uint64_t r11;
    uint64_t r12; uint64_t r13; uint64_t r14; uint64_t r15;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} __attribute__((packed));

const char* exception_name(uint8_t vector) {
    static const char* names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
        "Stack Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD Exception",
        "Virtualization Exception", "Control Protection", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", 
        "Security Exception", "Reserved"
    };
    return vector < 32 ? names[vector] : "Unknown Internal Exception";
}

static void dump_page_fault_info(uint64_t err, uint64_t cr2) {
    kprintf("\n  " RGB_FG(255, 100, 100) "PAGE FAULT" RESET "\n");
    kprintf(GRAY_FG "  Address   : " CYAN_FG "0x%p\n" RESET, (void*)cr2);

    const char* access = (err & (1 << 4)) ? "EXECUTE (Instruction Fetch)" : 
                         (err & (1 << 1)) ? "WRITE" : "READ";

    const char* privilege = (err & (1 << 2)) ? "USER" : "SUPERVISOR";

    const char* violation;
    if (!(err & (1 << 0))) {
        violation = "PAGE NOT PRESENT";
    } else if (err & (1 << 3)) {
        violation = "RESERVED BIT VIOLATION";
    } else if (err & (1 << 5)) {
        violation = "PROTECTION KEY VIOLATION";
    } else if (err & (1 << 15)) {
        violation = "SGX VIOLATION";
    } else {
        violation = "ACCESS RIGHTS VIOLATION";
    }

    kprintf(GRAY_FG "  Action    : " WHITE_FG "%s\n" RESET, access);
    kprintf(GRAY_FG "  Privilege : " WHITE_FG "%s\n" RESET, privilege);
    kprintf(GRAY_FG "  Page Flags: " RED_FG "%s\n" RESET, violation);

    if (!(err & (1 << 0)) && cr2 < 0x1000) {
        kprintf(ORANGE_FG "            : NULL pointer dereference\n" RESET);
    } else if ((err & (1 << 4)) && (err & (1 << 0))) {
        kprintf(ORANGE_FG "            : NX Violation\n" RESET);
    }
}

static inline void print_separator(const char* title) {
    kprintf(GRAY_FG "\n--[ " WHITE_FG "%s " GRAY_FG "]" 
            "----------------------------------------------------\n" RESET, title);
}

struct __attribute__((packed)) panic_signature {
    uint64_t rip;
    uint64_t cr2;
    uint32_t error_code;
    uint8_t  vector;
};

static void generate_signature(void* data, uint64_t len) {
    const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t* bytes = (uint8_t*)data;

    for (uint64_t i = 0; i < len; i += 3) {
        if (i > 0 && (i % 6 == 0)) {
            kprintf(GRAY_FG " " YELLOW_FG);
        }

        uint32_t b = (bytes[i] << 16);
        if (i + 1 < len) b |= (bytes[i + 1] << 8);
        if (i + 2 < len) b |= bytes[i + 2];

        kprintf("%c%c", table[(b >> 18) & 0x3F], table[(b >> 12) & 0x3F]);
        if (i + 1 < len) kprintf("%c", table[(b >> 6) & 0x3F]);
        if (i + 2 < len) kprintf("%c", table[b & 0x3F]);
    }
    kprintf("\n");
}

extern void* ke_exception_handler(void *frame) {
    struct isr_stackframe *f = (struct isr_stackframe *)frame;
    
    if (f->cs == 0x1b){
        task_t *t = get_current_task();

        kprintf(
            RED_FG "\n[task %llu:%s] %s\n" RESET,
            t->id,
            t->name,
            exception_name(f->vector)
        );

        if (f->vector == 14) {
            uint64_t cr2;
            asm volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("  page fault at %p\n", (void*)cr2);
        }

        return sched_switch_task(t->next, t);
        __builtin_unreachable();
    }

    if (faulted){
        serial_write("A fault happend in the panic handler, its probably propogated through whatever caused it in the first place but we wont spam the screen\n");
        for(;;) { __asm__ volatile ("cli; hlt"); }
    }
    faulted = 1;
    kprintf("fault!"); // at the moment clearing the screen while the screen is empty causes ANOTHER panic
    kprintf("\x1b[J");

    serial_write("\n[PANIC] Vec:"); serial_write_hex(f->vector);
    serial_write(" RIP:"); serial_write_hex(f->rip);

    kprintf("\n  " RGB_FG(207, 45, 45) "KERNEL PANIC: ");
    kprintf(RGB_FG(255, 80, 80) "CPU EXCEPTION: %s (0x%x)" RESET "\n", 
            exception_name(f->vector), (uint8_t)f->vector);

    print_separator("PROCESS CONTEXT");
    kprintf("  Current Task: " GREEN_FG "%s" RESET " (PID: %llu)\n", get_current_task()->name, get_current_task()->id);

    uint64_t sym_addr;
    const char *name = stack_trace_symbol_lookup(f->rip, &sym_addr);
    
    kprintf("  Instruction : " GREEN_FG "0x%p" RESET, (void*)f->rip);
    if (name) kprintf(ORANGE_FG " <%s + 0x%llx>" RESET, name, f->rip - sym_addr);
    kprintf("\n  Flags       : " GRAY_FG "0x%016llx" RESET " | CS: " GRAY_FG "0x%02x" RESET "\n", 
            f->rflags, (uint16_t)f->cs);

    print_separator("CPU CONTEXT");
    kprintf(GRAY_FG "  RAX " WHITE_FG "0x%016llx " GRAY_FG " R8  " WHITE_FG "0x%016llx " GRAY_FG " R12 " WHITE_FG "0x%016llx\n", f->rax, f->r8,  f->r12);
    kprintf(GRAY_FG "  RBX " WHITE_FG "0x%016llx " GRAY_FG " R9  " WHITE_FG "0x%016llx " GRAY_FG " R13 " WHITE_FG "0x%016llx\n", f->rbx, f->r9,  f->r13);
    kprintf(GRAY_FG "  RCX " WHITE_FG "0x%016llx " GRAY_FG " R10 " WHITE_FG "0x%016llx " GRAY_FG " R14 " WHITE_FG "0x%016llx\n", f->rcx, f->r10, f->r14);
    kprintf(GRAY_FG "  RDX " WHITE_FG "0x%016llx " GRAY_FG " R11 " WHITE_FG "0x%016llx " GRAY_FG " R15 " WHITE_FG "0x%016llx\n", f->rdx, f->r11, f->r15);
    kprintf(GRAY_FG "  RSI " WHITE_FG "0x%016llx " GRAY_FG " RDI " WHITE_FG "0x%016llx " GRAY_FG " RBP " WHITE_FG "0x%016llx\n", f->rsi, f->rdi, f->rbp);
    
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    if (f->vector == 14) {
        dump_page_fault_info(f->error_code, cr2);
    }

    print_separator("BACKTRACE");
    uint64_t cur_rbp;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(cur_rbp));
    stack_trace_print((uint64_t*)cur_rbp);

    print_separator("DEBUG SIGNATURE");
    kprintf(GRAY_FG "  Put this code into https://bleedkernel.com/panic.html or share it to report the issue\n\n" RESET);
    kprintf("  " YELLOW_FG);

    struct panic_signature signature = {
        .rip = f->rip,
        .cr2 = cr2,
        .error_code = (uint32_t)f->error_code,
        .vector = (uint8_t)f->vector
    };

    generate_signature(&signature, sizeof(signature));
    kprintf("  " RESET);
    
    for(;;) { __asm__ volatile ("hlt"); }
}