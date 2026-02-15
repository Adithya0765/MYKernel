// process.h - Process Management for Alteo OS
#ifndef PROCESS_H
#define PROCESS_H

#include "stdint.h"

// Process states
#define PROC_STATE_UNUSED    0
#define PROC_STATE_READY     1
#define PROC_STATE_RUNNING   2
#define PROC_STATE_BLOCKED   3
#define PROC_STATE_SLEEPING  4
#define PROC_STATE_ZOMBIE    5

// Limits
#define MAX_PROCESSES    64
#define KERNEL_STACK_SZ  8192
#define USER_STACK_SZ    (64 * 1024)   // 64KB user stack
#define PROC_NAME_MAX    32

// Priority levels
#define PRIORITY_LOW     0
#define PRIORITY_NORMAL  1
#define PRIORITY_HIGH    2
#define PRIORITY_REALTIME 3

// User-space stack location (per-process, mapped at this virtual address)
#define USER_STACK_TOP   0x7FFFFFFFE000ULL

// CPU context saved during context switch
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, ss;
} cpu_context_t;

// Process Control Block (PCB)
typedef struct {
    int pid;                     // Process ID
    int ppid;                    // Parent process ID
    int state;                   // Current state
    int priority;                // Priority level (0-3)
    int exit_code;               // Exit code (when zombie)
    char name[PROC_NAME_MAX];    // Process name
    cpu_context_t context;       // Saved CPU registers
    uint64_t stack_base;         // Base of kernel stack
    uint64_t stack_top;          // Top of kernel stack (initial RSP)
    uint64_t sleep_until;        // Tick count to wake up (if sleeping)
    uint64_t cpu_time;           // Total CPU ticks consumed
    uint64_t created_at;         // Tick when process was created
    int time_slice;              // Remaining time quantum (ticks)
    int default_slice;           // Default time quantum

    // --- Phase 1 additions ---
    uint64_t kernel_rsp;         // Saved kernel RSP for context switching
    uint64_t page_table;         // PML4 physical address (process address space)
    uint8_t  is_user;            // 1 = user-mode process, 0 = kernel-mode
    uint64_t user_stack_base;    // Base of user-mode stack
    uint64_t user_stack_top;     // Top of user-mode stack
    uint64_t entry_point;        // Entry point address (for ELF/PE loaded processes)
} process_t;

// Process table and management
void process_init(void);
int process_create(const char* name, void (*entry)(void), int priority);
void process_terminate(int pid, int exit_code);
void process_exit(int exit_code);
void process_set_state(int pid, int state);
void process_sleep(int pid, uint64_t ticks);
void process_wake_sleepers(uint64_t current_tick);

// Process queries
process_t* process_get(int pid);
process_t* process_get_current(void);
int process_get_pid(void);
int process_count(void);
int process_count_by_state(int state);
const char* process_state_name(int state);

// Process table access (for scheduler/sysinfo)
process_t* process_get_table(void);
int process_get_max(void);

// Set the current process PID (called by scheduler during context switch)
void process_set_current(int pid);

// Context switch support
extern void switch_context(uint64_t* old_rsp, uint64_t new_rsp);

#endif
