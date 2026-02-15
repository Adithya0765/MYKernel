// signal.h - POSIX Signal Implementation for Alteo OS
// Provides signal delivery, handler registration, and signal masking
#ifndef SIGNAL_H
#define SIGNAL_H

#include "stdint.h"
#include "syscall.h"

// Signal mask manipulation
#define SIG_BLOCK     0   // Block signals in set
#define SIG_UNBLOCK   1   // Unblock signals in set
#define SIG_SETMASK   2   // Set signal mask to set

// Default signal actions
#define SIG_ACTION_TERM    0   // Terminate process
#define SIG_ACTION_IGN     1   // Ignore signal
#define SIG_ACTION_CORE    2   // Terminate + core dump
#define SIG_ACTION_STOP    3   // Stop process
#define SIG_ACTION_CONT    4   // Continue process

// Initialize signal subsystem
void signal_init(void);

// Send a signal to a process (returns 0 on success)
int signal_send(int pid, int sig);

// Register a signal handler for a process
int signal_sigaction(int pid, int sig, const sigaction_t* act, sigaction_t* oldact);

// Return from signal handler (restore context)
int signal_return(int pid);

// Modify the signal mask for a process
int signal_procmask(int pid, int how, const uint64_t* set, uint64_t* oldset);

// Check and deliver pending signals for a process (called on return to userspace)
void signal_check_pending(int pid);

// Get the default action for a signal number
int signal_default_action(int sig);

// Check if a signal is pending for a process
int signal_is_pending(int pid, int sig);

#endif
