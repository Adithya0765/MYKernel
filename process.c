// process.c - Process Management for Alteo OS
#include "process.h"
#include "heap.h"

// Process table
static process_t proc_table[MAX_PROCESSES];
static int current_pid = -1;
static int next_pid = 1;
static int proc_initialized = 0;

// Simple string helpers (no libc)
static void proc_strcpy(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < PROC_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void proc_memset(void* ptr, int val, int size) {
    unsigned char* p = (unsigned char*)ptr;
    for (int i = 0; i < size; i++) p[i] = (unsigned char)val;
}

// Trampoline: when a process's entry function returns, exit cleanly
static void process_exit_trampoline(void) {
    process_exit(0);
    // Should never reach here - halt just in case
    for (;;) { __asm__ volatile("hlt"); }
}

// Initialize the process subsystem
void process_init(void) {
    proc_memset(proc_table, 0, sizeof(proc_table));
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_table[i].pid = -1;
        proc_table[i].state = PROC_STATE_UNUSED;
    }
    current_pid = -1;
    next_pid = 1;
    proc_initialized = 1;

    // Create the kernel/idle process (PID 0)
    proc_table[0].pid = 0;
    proc_table[0].ppid = 0;
    proc_table[0].state = PROC_STATE_RUNNING;
    proc_table[0].priority = PRIORITY_LOW;
    proc_table[0].time_slice = 20;
    proc_table[0].default_slice = 20;
    proc_table[0].cpu_time = 0;
    proc_table[0].created_at = 0;
    proc_table[0].kernel_rsp = 0;  // Will be set on first context switch
    proc_table[0].page_table = 0;  // Uses kernel page table
    proc_table[0].is_user = 0;
    proc_strcpy(proc_table[0].name, "kernel");
    current_pid = 0;
}

// Find a free slot in the process table
static int find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_STATE_UNUSED) return i;
    }
    return -1;
}

// Create a new process
int process_create(const char* name, void (*entry)(void), int priority) {
    if (!proc_initialized) return -1;

    int slot = find_free_slot();
    if (slot < 0) return -1;   // No free slot

    process_t* p = &proc_table[slot];
    proc_memset(p, 0, sizeof(process_t));

    p->pid = next_pid++;
    p->ppid = (current_pid >= 0) ? current_pid : 0;
    p->state = PROC_STATE_READY;
    p->priority = priority;
    p->exit_code = 0;
    p->is_user = 0;          // Kernel-mode by default
    p->page_table = 0;       // Uses kernel page table
    p->entry_point = (uint64_t)entry;
    proc_strcpy(p->name, name);

    // Allocate kernel stack
    void* stack = kmalloc(KERNEL_STACK_SZ);
    if (!stack) {
        p->state = PROC_STATE_UNUSED;
        p->pid = -1;
        return -1;
    }
    p->stack_base = (uint64_t)stack;
    p->stack_top = p->stack_base + KERNEL_STACK_SZ;

    // Initialize CPU context for first switch
    proc_memset(&p->context, 0, sizeof(cpu_context_t));
    p->context.rip = (uint64_t)entry;
    p->context.rsp = p->stack_top - 8;  // Align stack
    p->context.rflags = 0x202;           // IF flag set (interrupts enabled)
    p->context.cs = 0x08;               // Kernel code segment
    p->context.ss = 0x10;               // Kernel data segment

    // Set up the kernel stack for switch_context()
    // When switch_context restores this process for the first time, it will:
    //   popfq, pop r15-r12, pop rbx, pop rbp, ret
    // So the initial stack must look like:
    //   [stack_top - 8]  = process_exit_trampoline (return addr for entry func)
    //   [stack_top - 16] = entry point (return addr for switch_context's ret)
    //   [stack_top - 24] = rbp = 0
    //   [stack_top - 32] = rbx = 0
    //   [stack_top - 40] = r12 = 0
    //   [stack_top - 48] = r13 = 0
    //   [stack_top - 56] = r14 = 0
    //   [stack_top - 64] = r15 = 0
    //   [stack_top - 72] = rflags = 0x202
    if (entry) {
        uint64_t* sp = (uint64_t*)p->stack_top;
        *(--sp) = (uint64_t)process_exit_trampoline; // Return addr for entry func
        *(--sp) = (uint64_t)entry;                    // Return addr for switch_context
        *(--sp) = 0;                                  // rbp
        *(--sp) = 0;                                  // rbx
        *(--sp) = 0;                                  // r12
        *(--sp) = 0;                                  // r13
        *(--sp) = 0;                                  // r14
        *(--sp) = 0;                                  // r15
        *(--sp) = 0x202;                              // rflags (IF set)
        p->kernel_rsp = (uint64_t)sp;
    } else {
        // No entry point (placeholder process) - not switchable
        p->kernel_rsp = 0;
    }

    // Set time quantum based on priority
    switch (priority) {
        case PRIORITY_REALTIME: p->default_slice = 2;  break;
        case PRIORITY_HIGH:     p->default_slice = 5;  break;
        case PRIORITY_NORMAL:   p->default_slice = 10; break;
        case PRIORITY_LOW:      p->default_slice = 20; break;
        default:                p->default_slice = 10; break;
    }
    p->time_slice = p->default_slice;
    p->cpu_time = 0;
    p->created_at = 0;  // Will be set by caller if needed

    return p->pid;
}

// Terminate a process by PID
void process_terminate(int pid, int exit_code) {
    if (pid <= 0) return;  // Can't kill kernel process
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid && proc_table[i].state != PROC_STATE_UNUSED) {
            proc_table[i].state = PROC_STATE_ZOMBIE;
            proc_table[i].exit_code = exit_code;

            // Free kernel stack
            if (proc_table[i].stack_base) {
                kfree((void*)proc_table[i].stack_base);
                proc_table[i].stack_base = 0;
                proc_table[i].stack_top = 0;
            }

            // Reparent children to kernel (PID 0)
            for (int j = 0; j < MAX_PROCESSES; j++) {
                if (proc_table[j].ppid == pid && proc_table[j].state != PROC_STATE_UNUSED) {
                    proc_table[j].ppid = 0;
                }
            }
            return;
        }
    }
}

// Current process exits
void process_exit(int exit_code) {
    if (current_pid > 0) {
        process_terminate(current_pid, exit_code);
    }
}

// Set process state
void process_set_state(int pid, int state) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid) {
            proc_table[i].state = state;
            return;
        }
    }
}

// Put a process to sleep for N ticks
void process_sleep(int pid, uint64_t ticks) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid) {
            proc_table[i].state = PROC_STATE_SLEEPING;
            proc_table[i].sleep_until = ticks;  // Absolute tick value
            return;
        }
    }
}

// Wake up sleeping processes whose timer has expired
void process_wake_sleepers(uint64_t current_tick) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_STATE_SLEEPING) {
            if (current_tick >= proc_table[i].sleep_until) {
                proc_table[i].state = PROC_STATE_READY;
                proc_table[i].time_slice = proc_table[i].default_slice;
            }
        }
    }
}

// Get process by PID
process_t* process_get(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid && proc_table[i].state != PROC_STATE_UNUSED) {
            return &proc_table[i];
        }
    }
    return (process_t*)0;
}

// Get current running process
process_t* process_get_current(void) {
    if (current_pid < 0) return (process_t*)0;
    return process_get(current_pid);
}

// Get current PID
int process_get_pid(void) {
    return current_pid;
}

// Count total active processes
int process_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_STATE_UNUSED) count++;
    }
    return count;
}

// Count processes in a specific state
int process_count_by_state(int state) {
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == state) count++;
    }
    return count;
}

// Get human-readable state name
const char* process_state_name(int state) {
    switch (state) {
        case PROC_STATE_UNUSED:   return "unused";
        case PROC_STATE_READY:    return "ready";
        case PROC_STATE_RUNNING:  return "running";
        case PROC_STATE_BLOCKED:  return "blocked";
        case PROC_STATE_SLEEPING: return "sleeping";
        case PROC_STATE_ZOMBIE:   return "zombie";
        default:                  return "unknown";
    }
}

// Get raw process table access
process_t* process_get_table(void) {
    return proc_table;
}

int process_get_max(void) {
    return MAX_PROCESSES;
}

// Set the current process PID (called by scheduler during context switch)
void process_set_current(int pid) {
    current_pid = pid;
}
