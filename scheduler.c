// scheduler.c - CPU Scheduler for Alteo OS
// Implements Round-Robin and Priority-based scheduling with real context switching
#include "scheduler.h"
#include "process.h"
#include "gdt.h"

static int sched_initialized = 0;
static int sched_running = 0;
static int sched_algorithm = SCHED_PRIORITY;

// Statistics
static scheduler_stats_t stats;

// Current process index in process table
static int current_slot = 0;

// Forward declaration of external IRQ EOI function
extern void irq_send_eoi(uint8_t irq);

void scheduler_init(void) {
    stats.total_switches = 0;
    stats.total_ticks = 0;
    stats.idle_ticks = 0;
    stats.algorithm = sched_algorithm;
    stats.current_pid = 0;
    stats.ready_count = 0;
    sched_initialized = 1;
    sched_running = 1;
}

// Round-robin: pick next READY process after current
static int select_round_robin(void) {
    process_t* table = process_get_table();
    int max = process_get_max();
    int start = current_slot;

    // Try from current+1 to end, then wrap to 0
    for (int attempts = 0; attempts < max; attempts++) {
        int idx = (start + 1 + attempts) % max;
        if (table[idx].state == PROC_STATE_READY) {
            return idx;
        }
    }
    // If nothing ready, stay on current if it's running
    if (table[current_slot].state == PROC_STATE_RUNNING) {
        return current_slot;
    }
    return 0;  // Fall back to kernel/idle
}

// Priority-based: pick highest priority READY process
static int select_priority(void) {
    process_t* table = process_get_table();
    int max = process_get_max();
    int best_slot = -1;
    int best_prio = -1;

    for (int i = 0; i < max; i++) {
        if (table[i].state == PROC_STATE_READY) {
            if (table[i].priority > best_prio) {
                best_prio = table[i].priority;
                best_slot = i;
            } else if (table[i].priority == best_prio && best_slot >= 0) {
                // Same priority: round-robin among them (pick one after current)
                if (i > current_slot && (best_slot <= current_slot || best_slot > i)) {
                    best_slot = i;
                }
            }
        }
    }

    if (best_slot >= 0) return best_slot;

    // No ready process - stay on current or idle
    if (table[current_slot].state == PROC_STATE_RUNNING) {
        return current_slot;
    }
    return 0;  // kernel/idle
}

// Select next process to run
int scheduler_select_next(void) {
    if (!sched_initialized) return 0;

    if (sched_algorithm == SCHED_ROUND_ROBIN) {
        return select_round_robin();
    } else {
        return select_priority();
    }
}

// Main scheduler tick - called from timer IRQ (IRQ0)
void scheduler_tick(uint64_t current_tick) {
    if (!sched_initialized || !sched_running) return;

    stats.total_ticks++;

    process_t* table = process_get_table();

    // Wake up sleeping processes
    process_wake_sleepers(current_tick);

    // Update current process CPU time
    process_t* current = process_get_current();
    if (current) {
        current->cpu_time++;
    }

    // Decrement time slice of current process
    if (current && current->state == PROC_STATE_RUNNING) {
        current->time_slice--;

        if (current->time_slice <= 0) {
            // Time quantum expired - move to ready, pick next
            current->state = PROC_STATE_READY;
            current->time_slice = current->default_slice;

            int next_slot = scheduler_select_next();
            if (next_slot != current_slot) {
                // Context switch needed
                stats.total_switches++;

                // Mark old process as ready (if not already changed)
                if (table[current_slot].state == PROC_STATE_RUNNING) {
                    table[current_slot].state = PROC_STATE_READY;
                }

                // Mark new process as running
                table[next_slot].state = PROC_STATE_RUNNING;
                current_slot = next_slot;
                stats.current_pid = table[next_slot].pid;
            } else {
                // Same process continues
                current->state = PROC_STATE_RUNNING;
                current->time_slice = current->default_slice;
            }
        }
    } else {
        // Current process isn't running - find a new one
        stats.idle_ticks++;

        int next_slot = scheduler_select_next();
        if (table[next_slot].state == PROC_STATE_READY) {
            table[next_slot].state = PROC_STATE_RUNNING;
            current_slot = next_slot;
            stats.current_pid = table[next_slot].pid;
            stats.total_switches++;
        }
    }

    // Update ready count
    stats.ready_count = process_count_by_state(PROC_STATE_READY);
}

// Cooperative yield - give up remaining time slice (with real context switch)
void scheduler_yield(void) {
    if (!sched_initialized) return;

    process_t* table = process_get_table();

    // Move current to ready
    if (table[current_slot].state == PROC_STATE_RUNNING) {
        table[current_slot].state = PROC_STATE_READY;
        table[current_slot].time_slice = table[current_slot].default_slice;
    }

    int next = scheduler_select_next();
    if (next >= 0 && next != current_slot) {
        int old_slot = current_slot;

        table[next].state = PROC_STATE_RUNNING;
        current_slot = next;
        stats.current_pid = table[next].pid;
        stats.total_switches++;

        // Update the current PID in the process subsystem
        process_set_current(table[next].pid);

        // Update TSS RSP0 so interrupts from user mode land on the right kernel stack
        if (table[next].stack_top) {
            tss_set_rsp0(table[next].stack_top);
        }

        // Perform the actual context switch only if both processes have valid stacks
        if (table[old_slot].kernel_rsp != 0 || old_slot == 0) {
            if (table[next].kernel_rsp != 0) {
                switch_context(&table[old_slot].kernel_rsp, table[next].kernel_rsp);
            }
        }
    } else if (next == current_slot) {
        // Same process continues
        table[current_slot].state = PROC_STATE_RUNNING;
    }
}

// Set scheduling algorithm
void scheduler_set_algorithm(int algo) {
    if (algo == SCHED_ROUND_ROBIN || algo == SCHED_PRIORITY) {
        sched_algorithm = algo;
        stats.algorithm = algo;
    }
}

// Get statistics
scheduler_stats_t scheduler_get_stats(void) {
    return stats;
}

// Block a process
void scheduler_block(int pid) {
    process_set_state(pid, PROC_STATE_BLOCKED);

    // If blocking current process, trigger reschedule
    process_t* current = process_get_current();
    if (current && current->pid == pid) {
        scheduler_yield();
    }
}

// Unblock a process
void scheduler_unblock(int pid) {
    process_set_state(pid, PROC_STATE_READY);
    process_t* p = process_get(pid);
    if (p) {
        p->time_slice = p->default_slice;
    }
}

int scheduler_is_running(void) {
    return sched_initialized && sched_running;
}

// ---------- Preemptive Timer IRQ Handler ----------
// Called from irq0_switch in switch.asm.
// Takes the current stack pointer (pointing to saved registers_t frame),
// runs the scheduler, and returns the RSP to restore (may be different).
uint64_t timer_irq_handler(uint64_t current_rsp) {
    if (!sched_initialized || !sched_running) {
        irq_send_eoi(0);
        return current_rsp;
    }

    // Send EOI early so we don't miss timer ticks
    irq_send_eoi(0);

    process_t* table = process_get_table();

    stats.total_ticks++;

    // Wake up sleeping processes
    process_wake_sleepers(stats.total_ticks);

    // Save current process's register frame RSP
    if (table[current_slot].state == PROC_STATE_RUNNING ||
        table[current_slot].state == PROC_STATE_READY) {
        table[current_slot].kernel_rsp = current_rsp;
    }

    // Update current process CPU time
    if (table[current_slot].state == PROC_STATE_RUNNING) {
        table[current_slot].cpu_time++;
        table[current_slot].time_slice--;

        if (table[current_slot].time_slice <= 0) {
            // Time quantum expired
            table[current_slot].state = PROC_STATE_READY;
            table[current_slot].time_slice = table[current_slot].default_slice;

            int next = scheduler_select_next();
            if (next != current_slot && table[next].kernel_rsp != 0) {
                table[next].state = PROC_STATE_RUNNING;
                current_slot = next;
                stats.current_pid = table[next].pid;
                stats.total_switches++;
                process_set_current(table[next].pid);

                if (table[next].stack_top) {
                    tss_set_rsp0(table[next].stack_top);
                }

                return table[next].kernel_rsp;
            } else {
                // No switch - same process continues
                table[current_slot].state = PROC_STATE_RUNNING;
                table[current_slot].time_slice = table[current_slot].default_slice;
            }
        }
    } else {
        // Current process not running - find someone else
        stats.idle_ticks++;

        int next = scheduler_select_next();
        if (next >= 0 && table[next].state == PROC_STATE_READY &&
            table[next].kernel_rsp != 0) {
            table[next].state = PROC_STATE_RUNNING;
            current_slot = next;
            stats.current_pid = table[next].pid;
            stats.total_switches++;
            process_set_current(table[next].pid);

            if (table[next].stack_top) {
                tss_set_rsp0(table[next].stack_top);
            }

            return table[next].kernel_rsp;
        }
    }

    stats.ready_count = process_count_by_state(PROC_STATE_READY);
    return current_rsp;
}
