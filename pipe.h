// pipe.h - Kernel Pipe Implementation for Alteo OS
// Provides POSIX-style pipes for inter-process communication
#ifndef PIPE_H
#define PIPE_H

#include "stdint.h"

// Pipe configuration
#define PIPE_BUF_SIZE    4096   // Size of pipe circular buffer
#define MAX_PIPES        32     // Maximum concurrent pipes

// Pipe states
#define PIPE_STATE_FREE     0
#define PIPE_STATE_ACTIVE   1

// Initialize pipe subsystem
void pipe_init(void);

// Create a new pipe, returns pipe index or -1 on failure
int pipe_create(void);

// Close one end of a pipe (end: 0=read, 1=write)
void pipe_close(int pipe_idx, int end);

// Read from pipe's read end. Returns bytes read, 0 on EOF, -1 on error
int pipe_read(int pipe_idx, void* buf, int count);

// Write to pipe's write end. Returns bytes written, -1 on error
int pipe_write(int pipe_idx, const void* buf, int count);

// Get number of bytes available to read
int pipe_available(int pipe_idx);

// Check if pipe is still active (has at least one end open)
int pipe_is_active(int pipe_idx);

#endif
