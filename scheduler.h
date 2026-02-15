// scheduler.h - CPU Scheduler for Alteo OS
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "stdint.h"
#include "process.h"

// Scheduler algorithms
#define SCHED_ROUND_ROBIN    0
#define SCHED_PRIORITY       1

// Time quantum defaults (in ticks)
#define QUANTUM_LOW       20
#define QUANTUM_NORMAL    10
#define QUANTUM_HIGH       5
#define QUANTUM_REALTIME   2

// Scheduler statistics
typedef struct {
    uint64_t total_switches;    // Total context switches
    uint64_t total_ticks;       // Total scheduler ticks
    uint64_t idle_ticks;        // Ticks spent idle
    int algorithm;              // Current scheduling algorithm
    int current_pid;            // Currently running process PID
    int ready_count;            // Number of ready processes
} scheduler_stats_t;

// Initialize the scheduler
void scheduler_init(void);

// Main scheduler tick (called from timer IRQ)
void scheduler_tick(uint64_t current_tick);

// Yield CPU voluntarily (cooperative multitasking)
void scheduler_yield(void);

// Select next process to run
int scheduler_select_next(void);

// Set scheduling algorithm
void scheduler_set_algorithm(int algo);

// Get scheduler statistics
scheduler_stats_t scheduler_get_stats(void);

// Block/unblock processes
void scheduler_block(int pid);
void scheduler_unblock(int pid);

// Check if scheduler is active
int scheduler_is_running(void);

// Timer IRQ handler for preemptive context switching
// Called from irq0_switch in switch.asm
// Returns the RSP to restore (may be different process)
uint64_t timer_irq_handler(uint64_t current_rsp);

#endif
