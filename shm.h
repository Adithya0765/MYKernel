// shm.h - SysV Shared Memory for Alteo OS
// Provides shared memory segments for inter-process communication
#ifndef SHM_H
#define SHM_H

#include "stdint.h"

// Shared memory configuration
#define SHM_MAX_SEGMENTS   16         // Maximum shared memory segments
#define SHM_MAX_ATTACH     8          // Max attachments per segment
#define SHM_MAX_SIZE       (1024*1024) // 1MB max per segment

// shmget flags
#define IPC_CREAT     0x0200   // Create segment if it doesn't exist
#define IPC_EXCL      0x0400   // Fail if segment already exists
#define IPC_PRIVATE   0        // Private key (unique segment)

// Initialize shared memory subsystem
void shm_init(void);

// Get or create a shared memory segment
// Returns segment ID or -1 on failure
int shm_get(uint64_t key, uint64_t size, int flags);

// Attach shared memory to a process's address space
// Returns virtual address or 0 on failure
uint64_t shm_attach(int shmid, int pid, uint64_t addr);

// Detach shared memory from a process's address space
int shm_detach(int pid, uint64_t addr);

// Remove a shared memory segment (mark for deletion)
int shm_remove(int shmid);

// Get info about a shared memory segment
int shm_get_size(int shmid);
int shm_get_nattach(int shmid);

#endif
