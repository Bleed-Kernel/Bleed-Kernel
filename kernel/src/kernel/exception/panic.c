#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <ansii.h>
#include <mm/cow.h>
#include <sched/scheduler.h>
#include <drivers/serial/serial.h>
#include <kernel/exception/panic.h>
#include <kernel/exception/stack_trace.h>
#include <boot/bootlogger/bootlogger.h>

#define hcf() do { \
    __asm__ volatile ("hlt"); \
} while (0)

#define PRINT_REG(name, val) do { \
    bset_color(BCOL_GREY, BCOL_BLACK); \
    bprintf("  %s ", name); \
    bset_color(BCOL_WHITE, BCOL_BLACK); \
    bprintf("0x%llx ", val); \
} while(0)

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

void *ke_user_program_exception(struct isr_stackframe *sf, task_t *task){
    uint64_t cr2 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    if (sf->vector == 14 && paging_handle_cow_fault(task, cr2, sf->error_code))
        return sf;

    if (sf->vector == 14)
        serial_printf("Page Fault info: error code: %p, cr2: %p\n", sf->error_code, cr2);
    
    serial_printf("\nA User Program Crashed\n %u (%s) at %p (%s - %u)\n", sf->vector, exception_name(sf->vector), sf->rip, get_current_task()->name, get_current_task()->id);

    kprintf("%s[ERROR]%s %s (%llu) has crashed and the task was killed [%s]\n", 
        RED_FG, 
        WHITE_FG, 
        get_current_task()->name, 
        get_current_task()->id,
        exception_name(sf->vector));

    exit();

    __builtin_unreachable();
}

static void dump_page_fault_info(uint64_t err, uint64_t cr2) {
    bset_color(BCOL_RED, BCOL_BLACK);
    bprintf("\n  PAGE FAULT\n");
    
    bset_color(BCOL_GREY, BCOL_BLACK);
    bprintf("  Address   : ");
    bset_color(BCOL_CYAN, BCOL_BLACK);
    bprintf("0x%p\n", (void*)cr2);

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

    bset_color(BCOL_GREY, BCOL_BLACK);
    bprintf("  Action    : ");
    bset_color(BCOL_WHITE, BCOL_BLACK);
    bprintf("%s\n", access);
    
    bset_color(BCOL_GREY, BCOL_BLACK);
    bprintf("  Privilege : ");
    bset_color(BCOL_WHITE, BCOL_BLACK);
    bprintf("%s\n", privilege);
    
    bset_color(BCOL_GREY, BCOL_BLACK);
    bprintf("  Page Flags: ");
    bset_color(BCOL_RED, BCOL_BLACK);
    bprintf("%s\n", violation);

    if (!(err & (1 << 0)) && cr2 < 0x1000) {
        bset_color(BCOL_YELLOW, BCOL_BLACK);
        bprintf("            : NULL pointer dereference\n");
    } else if ((err & (1 << 4)) && (err & (1 << 0))) {
        bset_color(BCOL_YELLOW, BCOL_BLACK);
        bprintf("            : NX Violation\n");
    }
    breset_color();
}

static inline void print_separator(const char* title) {
    bset_color(BCOL_GREY, BCOL_BLACK);
    bprintf("\n--[ ");
    bset_color(BCOL_WHITE, BCOL_BLACK);
    bprintf("%s ", title);
    bset_color(BCOL_GREY, BCOL_BLACK);
    bprintf("]----------------------------------------------------\n");
    breset_color();
}

__attribute__((noreturn))
void ke_panic(struct isr_stackframe *sf, const char *pstring){
    if (!sf && !pstring) hcf();
    bconsole_init(); //users who dont enable verbose will never see a panic unless we do this.
    breset_color();

    if (sf) {
        serial_write("\n[PANIC] Vec:"); serial_write_hex(sf->vector);
        serial_write(" RIP:"); serial_write_hex(sf->rip);

        bset_color(BCOL_RED, BCOL_BLACK);
        bprintf("\n  KERNEL PANIC: ");
        bset_color(BCOL_WHITE, BCOL_BLACK);
        bprintf("CPU EXCEPTION: %s (0x%x)\n", exception_name(sf->vector), (uint8_t)sf->vector);

        task_t *current = get_current_task();

        print_separator("PROCESS CONTEXT");
        if (current) {
            bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("  Current Task: ");
            bset_color(BCOL_GREEN, BCOL_BLACK); bprintf("%s ", current->name);
            bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("(PID: %llu)\n", current->id);
        } else {
            bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("  Current Task: ");
            bset_color(BCOL_YELLOW, BCOL_BLACK); bprintf("<none>\n");
        }

        uint64_t sym_addr;
        const char *name = stack_trace_symbol_lookup(sf->rip, &sym_addr);

        bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("  Instruction : ");
        bset_color(BCOL_GREEN, BCOL_BLACK); bprintf("0x%p", (void*)sf->rip);
        if (name) {
            bset_color(BCOL_YELLOW, BCOL_BLACK);
            bprintf(" <%s + 0x%llx>", name, sf->rip - sym_addr);
        }
        
        bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("\n  Flags       : ");
        bset_color(BCOL_GREY, BCOL_BLACK);  bprintf("0x%llx ", sf->rflags);
        bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("| CS: ");
        bset_color(BCOL_GREY, BCOL_BLACK);  bprintf("0x%x\n", (uint16_t)sf->cs);

        print_separator("CPU CONTEXT");
        PRINT_REG("RAX", sf->rax); PRINT_REG("R8 ", sf->r8);  PRINT_REG("R12", sf->r12); bprintf("\n");
        PRINT_REG("RBX", sf->rbx); PRINT_REG("R9 ", sf->r9);  PRINT_REG("R13", sf->r13); bprintf("\n");
        PRINT_REG("RCX", sf->rcx); PRINT_REG("R10", sf->r10); PRINT_REG("R14", sf->r14); bprintf("\n");
        PRINT_REG("RDX", sf->rdx); PRINT_REG("R11", sf->r11); PRINT_REG("R15", sf->r15); bprintf("\n");
        PRINT_REG("RSI", sf->rsi); PRINT_REG("RDI", sf->rdi); PRINT_REG("RBP", sf->rbp); bprintf("\n");

        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        if (sf->vector == 14) {
            dump_page_fault_info(sf->error_code, cr2);
        }

        print_separator("BACKTRACE");
        stack_trace_print((uint64_t*)sf->rbp);

        print_separator("DEBUG SIGNATURE");
        bset_color(BCOL_GREY, BCOL_BLACK);
        bprintf("  Put this code into https://bleedkernel.com/panic.html or share it to report the issue\n\n");
        bset_color(BCOL_YELLOW, BCOL_BLACK);
        bprintf("  ");

    } else {
        serial_write("\n[PANIC] "); serial_write(pstring); serial_write("\n");

        bset_color(BCOL_RED, BCOL_BLACK);
        bprintf("\n  KERNEL PANIC: ");
        bset_color(BCOL_WHITE, BCOL_BLACK);
        bprintf("%s\n", pstring);

        task_t *current = get_current_task();

        print_separator("PROCESS CONTEXT");
        if (current) {
            bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("  Current Task: ");
            bset_color(BCOL_GREEN, BCOL_BLACK); bprintf("%s ", current->name);
            bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("(PID: %llu)\n", current->id);
        } else {
            bset_color(BCOL_WHITE, BCOL_BLACK); bprintf("  Current Task: ");
            bset_color(BCOL_YELLOW, BCOL_BLACK); bprintf("<none>\n");
        }

        print_separator("BACKTRACE");
        uint64_t cur_rbp;
        __asm__ volatile ("mov %%rbp, %0" : "=r"(cur_rbp));
        stack_trace_print((uint64_t*)cur_rbp);
    }

    hcf();
    for (;;) {}
}

extern void* ke_processor_exception(void *frame){
    struct isr_stackframe *sf = (struct isr_stackframe *)frame;
    task_t *responsible_task = get_current_task();
    
    if (sf){
        if (((sf->cs & P_USER) == P_USER))
            return ke_user_program_exception(sf, responsible_task);
    }

    static volatile int panic_active = 0;
    if (__atomic_exchange_n(&panic_active, 1, __ATOMIC_ACQ_REL) != 0) {
        serial_write("\n[NESTED PANIC] Vector:");
        serial_write_hex(sf ? sf->vector : 0xFF);
        serial_write(" RIP:");
        serial_write_hex(sf ? sf->rip : 0);
        serial_write("\nSystem halted.\n");
        asm volatile ("cli");
        hcf();
    }

    ke_panic(sf, NULL);
    
    return frame;
}