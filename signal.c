// signal.c - POSIX Signal Implementation for Alteo OS
// Signal delivery, handler registration, masking, and default actions
#include "signal.h"
#include "process.h"

// Per-process signal state
typedef struct {
    sigaction_t handlers[NSIG];    // Signal handlers
    uint64_t    pending;           // Bitmask of pending signals
    uint64_t    blocked;           // Bitmask of blocked signals
    int         in_handler;        // Currently executing a signal handler
    int         saved_state;       // Saved process state before handler
} signal_state_t;

static signal_state_t sig_states[MAX_PROCESSES];
static int signal_initialized = 0;

// ---- Helpers ----
static void sig_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}

static int proc_slot_for_signal(int pid) {
    process_t* table = process_get_table();
    for (int i = 0; i < process_get_max(); i++) {
        if (table[i].pid == pid && table[i].state != PROC_STATE_UNUSED) return i;
    }
    return -1;
}

// Get default action for standard POSIX signals
int signal_default_action(int sig) {
    switch (sig) {
        case SIGHUP:   return SIG_ACTION_TERM;
        case SIGINT:   return SIG_ACTION_TERM;
        case SIGQUIT:  return SIG_ACTION_CORE;
        case SIGILL:   return SIG_ACTION_CORE;
        case SIGTRAP:  return SIG_ACTION_CORE;
        case SIGABRT:  return SIG_ACTION_CORE;
        case SIGBUS:   return SIG_ACTION_CORE;
        case SIGFPE:   return SIG_ACTION_CORE;
        case SIGKILL:  return SIG_ACTION_TERM;
        case SIGUSR1:  return SIG_ACTION_TERM;
        case SIGSEGV:  return SIG_ACTION_CORE;
        case SIGUSR2:  return SIG_ACTION_TERM;
        case SIGPIPE:  return SIG_ACTION_TERM;
        case SIGALRM:  return SIG_ACTION_TERM;
        case SIGTERM:  return SIG_ACTION_TERM;
        case SIGCHLD:  return SIG_ACTION_IGN;
        case SIGCONT:  return SIG_ACTION_CONT;
        case SIGSTOP:  return SIG_ACTION_STOP;
        case SIGTSTP:  return SIG_ACTION_STOP;
        default:       return SIG_ACTION_TERM;
    }
}

// ---- Public API ----

void signal_init(void) {
    sig_memset(sig_states, 0, sizeof(sig_states));

    // Set all handlers to SIG_DFL for all processes
    for (int i = 0; i < MAX_PROCESSES; i++) {
        for (int s = 0; s < NSIG; s++) {
            sig_states[i].handlers[s].sa_handler = SIG_DFL;
            sig_states[i].handlers[s].sa_mask = 0;
            sig_states[i].handlers[s].sa_flags = 0;
        }
        sig_states[i].pending = 0;
        sig_states[i].blocked = 0;
        sig_states[i].in_handler = 0;
    }

    signal_initialized = 1;
}

int signal_send(int pid, int sig) {
    if (!signal_initialized) return -1;
    if (sig < 1 || sig >= NSIG) return -1;

    int slot = proc_slot_for_signal(pid);
    if (slot < 0) return -1;

    // Cannot catch or ignore SIGKILL and SIGSTOP
    if (sig == SIGKILL) {
        process_terminate(pid, 128 + SIGKILL);
        return 0;
    }
    if (sig == SIGSTOP) {
        process_set_state(pid, PROC_STATE_BLOCKED);
        return 0;
    }

    // SIGCONT always resumes
    if (sig == SIGCONT) {
        process_t* p = process_get(pid);
        if (p && p->state == PROC_STATE_BLOCKED) {
            process_set_state(pid, PROC_STATE_READY);
        }
        // Clear pending SIGSTOP/SIGTSTP
        sig_states[slot].pending &= ~((1ULL << SIGSTOP) | (1ULL << SIGTSTP));
        return 0;
    }

    // Add to pending set
    sig_states[slot].pending |= (1ULL << sig);

    // If the process is blocked/sleeping, wake it up to handle the signal
    process_t* p = process_get(pid);
    if (p && (p->state == PROC_STATE_BLOCKED || p->state == PROC_STATE_SLEEPING)) {
        process_set_state(pid, PROC_STATE_READY);
    }

    return 0;
}

int signal_sigaction(int pid, int sig, const sigaction_t* act, sigaction_t* oldact) {
    if (!signal_initialized) return -1;
    if (sig < 1 || sig >= NSIG) return -1;
    if (sig == SIGKILL || sig == SIGSTOP) return -1;

    int slot = proc_slot_for_signal(pid);
    if (slot < 0) return -1;

    // Return old handler if requested
    if (oldact) {
        *oldact = sig_states[slot].handlers[sig];
    }

    // Set new handler if provided
    if (act) {
        sig_states[slot].handlers[sig] = *act;
    }

    return 0;
}

int signal_return(int pid) {
    if (!signal_initialized) return -1;

    int slot = proc_slot_for_signal(pid);
    if (slot < 0) return -1;

    // Mark no longer in handler
    sig_states[slot].in_handler = 0;
    return 0;
}

int signal_procmask(int pid, int how, const uint64_t* set, uint64_t* oldset) {
    if (!signal_initialized) return -1;

    int slot = proc_slot_for_signal(pid);
    if (slot < 0) return -1;

    // Return old mask
    if (oldset) {
        *oldset = sig_states[slot].blocked;
    }

    if (set) {
        uint64_t mask = *set;
        // Cannot block SIGKILL or SIGSTOP
        mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

        switch (how) {
            case SIG_BLOCK:
                sig_states[slot].blocked |= mask;
                break;
            case SIG_UNBLOCK:
                sig_states[slot].blocked &= ~mask;
                break;
            case SIG_SETMASK:
                sig_states[slot].blocked = mask;
                break;
            default:
                return -1;
        }
    }

    return 0;
}

void signal_check_pending(int pid) {
    if (!signal_initialized) return;

    int slot = proc_slot_for_signal(pid);
    if (slot < 0) return;

    // Don't deliver signals while already in a handler
    if (sig_states[slot].in_handler) return;

    signal_state_t* ss = &sig_states[slot];

    // Check for deliverable signals (pending & ~blocked)
    uint64_t deliverable = ss->pending & ~ss->blocked;
    if (deliverable == 0) return;

    // Find the lowest-numbered pending signal
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        // Clear from pending
        ss->pending &= ~(1ULL << sig);

        sigaction_t* handler = &ss->handlers[sig];

        if (handler->sa_handler == SIG_IGN) {
            // Explicitly ignored
            continue;
        }

        if (handler->sa_handler == SIG_DFL) {
            // Default action
            int action = signal_default_action(sig);
            switch (action) {
                case SIG_ACTION_TERM:
                case SIG_ACTION_CORE:
                    process_terminate(pid, 128 + sig);
                    return;
                case SIG_ACTION_STOP:
                    process_set_state(pid, PROC_STATE_BLOCKED);
                    return;
                case SIG_ACTION_CONT:
                    // Already handled in signal_send
                    continue;
                case SIG_ACTION_IGN:
                    continue;
            }
        } else {
            // User handler installed — in a full implementation we'd push a
            // signal frame onto the user stack and redirect execution to the
            // handler. For now, mark that we're in a handler context.
            ss->in_handler = 1;
            ss->saved_state = 0;

            // Block additional signals during handler execution
            ss->blocked |= handler->sa_mask;
            ss->blocked |= (1ULL << sig); // Block this signal during its handler

            // In a full implementation:
            // 1. Save user context (registers) to signal frame on user stack
            // 2. Set up return trampoline pointing to sys_sigreturn
            // 3. Set user RIP to handler->sa_handler, RDI to sig
            // 4. Return to userspace — handler executes, calls sigreturn

            // For kernel-mode signal handling (simplified):
            // Just call the handler directly if it's a kernel-space function pointer
            // This is safe because all our processes currently run in kernel mode
            void (*fn)(int) = handler->sa_handler;
            if ((uint64_t)fn > 1) {
                fn(sig);
            }

            ss->in_handler = 0;
            // Restore signal mask (simplified — real impl uses saved mask from frame)
            ss->blocked &= ~handler->sa_mask;
            ss->blocked &= ~(1ULL << sig);

            return; // Handle one signal at a time
        }
    }
}

int signal_is_pending(int pid, int sig) {
    if (!signal_initialized) return 0;
    if (sig < 1 || sig >= NSIG) return 0;

    int slot = proc_slot_for_signal(pid);
    if (slot < 0) return 0;

    return (sig_states[slot].pending & (1ULL << sig)) ? 1 : 0;
}
