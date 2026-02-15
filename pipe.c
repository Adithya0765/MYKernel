// pipe.c - Kernel Pipe Implementation for Alteo OS
// Circular buffer pipes for IPC between processes
#include "pipe.h"

// Pipe structure with circular buffer
typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    int      head;           // Write position
    int      tail;           // Read position
    int      count;          // Bytes in buffer
    int      state;          // PIPE_STATE_FREE or PIPE_STATE_ACTIVE
    int      read_open;      // Is the read end open?
    int      write_open;     // Is the write end open?
    int      readers;        // Number of read end references
    int      writers;        // Number of write end references
} pipe_t;

static pipe_t pipes[MAX_PIPES];
static int pipe_initialized = 0;

// ---- Helpers ----
static void pipe_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}

static void pipe_memcpy(void* dst, const void* src, int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

// ---- Public API ----

void pipe_init(void) {
    pipe_memset(pipes, 0, sizeof(pipes));
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].state = PIPE_STATE_FREE;
    }
    pipe_initialized = 1;
}

int pipe_create(void) {
    if (!pipe_initialized) return -1;

    // Find a free pipe slot
    for (int i = 0; i < MAX_PIPES; i++) {
        if (pipes[i].state == PIPE_STATE_FREE) {
            pipe_memset(&pipes[i], 0, sizeof(pipe_t));
            pipes[i].state = PIPE_STATE_ACTIVE;
            pipes[i].head = 0;
            pipes[i].tail = 0;
            pipes[i].count = 0;
            pipes[i].read_open = 1;
            pipes[i].write_open = 1;
            pipes[i].readers = 1;
            pipes[i].writers = 1;
            return i;
        }
    }
    return -1; // No free pipes
}

void pipe_close(int pipe_idx, int end) {
    if (pipe_idx < 0 || pipe_idx >= MAX_PIPES) return;
    if (pipes[pipe_idx].state != PIPE_STATE_ACTIVE) return;

    if (end == 0) {
        // Close read end
        if (pipes[pipe_idx].readers > 0) {
            pipes[pipe_idx].readers--;
            if (pipes[pipe_idx].readers == 0)
                pipes[pipe_idx].read_open = 0;
        }
    } else {
        // Close write end
        if (pipes[pipe_idx].writers > 0) {
            pipes[pipe_idx].writers--;
            if (pipes[pipe_idx].writers == 0)
                pipes[pipe_idx].write_open = 0;
        }
    }

    // If both ends closed, free the pipe
    if (!pipes[pipe_idx].read_open && !pipes[pipe_idx].write_open) {
        pipes[pipe_idx].state = PIPE_STATE_FREE;
    }
}

int pipe_read(int pipe_idx, void* buf, int count) {
    if (pipe_idx < 0 || pipe_idx >= MAX_PIPES) return -1;
    if (pipes[pipe_idx].state != PIPE_STATE_ACTIVE) return -1;
    if (!buf || count <= 0) return -1;

    pipe_t* p = &pipes[pipe_idx];

    // If buffer is empty
    if (p->count == 0) {
        // If write end is closed, return 0 (EOF)
        if (!p->write_open) return 0;
        // Otherwise, no data available (would block in a real impl)
        return 0;
    }

    // Read up to count bytes from the circular buffer
    int to_read = count;
    if (to_read > p->count) to_read = p->count;

    uint8_t* dst = (uint8_t*)buf;
    for (int i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= to_read;

    return to_read;
}

int pipe_write(int pipe_idx, const void* buf, int count) {
    if (pipe_idx < 0 || pipe_idx >= MAX_PIPES) return -1;
    if (pipes[pipe_idx].state != PIPE_STATE_ACTIVE) return -1;
    if (!buf || count <= 0) return -1;

    pipe_t* p = &pipes[pipe_idx];

    // If read end is closed, writing would cause SIGPIPE
    if (!p->read_open) return -1;

    // Write up to count bytes, limited by available space
    int space = PIPE_BUF_SIZE - p->count;
    int to_write = count;
    if (to_write > space) to_write = space;
    if (to_write == 0) return 0; // Buffer full (would block)

    const uint8_t* src = (const uint8_t*)buf;
    for (int i = 0; i < to_write; i++) {
        p->buf[p->head] = src[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count += to_write;

    return to_write;
}

int pipe_available(int pipe_idx) {
    if (pipe_idx < 0 || pipe_idx >= MAX_PIPES) return 0;
    if (pipes[pipe_idx].state != PIPE_STATE_ACTIVE) return 0;
    return pipes[pipe_idx].count;
}

int pipe_is_active(int pipe_idx) {
    if (pipe_idx < 0 || pipe_idx >= MAX_PIPES) return 0;
    return pipes[pipe_idx].state == PIPE_STATE_ACTIVE;
}
