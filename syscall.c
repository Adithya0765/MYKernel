// syscall.c - POSIX-compatible System Call Interface for Alteo OS
// Provides libc-compatible syscall layer with VFS, pipe, signal, and IPC support
#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "gdt.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "pipe.h"
#include "signal.h"
#include "shm.h"

static int syscall_initialized = 0;

// Kernel syscall stack (used by syscall_entry in switch.asm)
static uint8_t kernel_syscall_stack_data[8192] __attribute__((aligned(16)));
uint64_t kernel_syscall_stack_top = 0;

// Per-process file descriptor table
// Maps process-local fd -> VFS fd (or pipe fd with high bit set)
#define PROC_MAX_FDS    64
#define PIPE_FD_FLAG    0x40000000

typedef struct {
    int vfs_fd;
    int flags;
    int in_use;
} proc_fd_entry_t;

static proc_fd_entry_t proc_fds[MAX_PROCESSES][PROC_MAX_FDS];
static uint64_t proc_brk[MAX_PROCESSES];

// MSR addresses
#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_SFMASK  0xC0000084
#define EFER_SCE    (1ULL << 0)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

extern void syscall_entry(void);

// ---- Helpers ----
static void sys_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int sys_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void sys_memset(void* p, int v, uint64_t n) {
    unsigned char* b = (unsigned char*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = (unsigned char)v;
}

// ---- Per-process FD helpers ----
static int proc_slot_for_pid(int pid) {
    process_t* table = process_get_table();
    for (int i = 0; i < process_get_max(); i++) {
        if (table[i].pid == pid && table[i].state != PROC_STATE_UNUSED) return i;
    }
    return -1;
}

static int alloc_proc_fd(int slot) {
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc_fds[slot][i].in_use) return i;
    }
    return -1;
}

// ---- Init ----
void syscall_init(void) {
    kernel_syscall_stack_top = (uint64_t)&kernel_syscall_stack_data[8192];
    sys_memset(proc_fds, 0, sizeof(proc_fds));
    sys_memset(proc_brk, 0, sizeof(proc_brk));
    for (int i = 0; i < MAX_PROCESSES; i++) proc_brk[i] = 0x400000;

    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);
    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1 << 9) | (1 << 8) | (1 << 10));
    syscall_initialized = 1;
}

// ============ Process Management ============

int sys_exit(int exit_code) {
    int pid = process_get_pid();
    if (pid > 0) {
        process_t* p = process_get(pid);
        if (p) signal_send(p->ppid, SIGCHLD);
        process_terminate(pid, exit_code);
        scheduler_yield();
    }
    return SYSCALL_OK;
}

int sys_getpid(void) { return process_get_pid(); }
int sys_yield(void)  { scheduler_yield(); return SYSCALL_OK; }

int sys_sleep(uint64_t ticks) {
    int pid = process_get_pid();
    if (pid < 0) return SYSCALL_ERROR;
    scheduler_stats_t st = scheduler_get_stats();
    process_sleep(pid, st.total_ticks + ticks);
    scheduler_yield();
    return SYSCALL_OK;
}

int sys_fork(void) {
    process_t* current = process_get_current();
    if (!current) return SYSCALL_ERROR;
    int child_pid = process_create(current->name, (void(*)(void))0, current->priority);
    if (child_pid < 0) return SYSCALL_ENOMEM;
    int ps = proc_slot_for_pid(current->pid);
    int cs = proc_slot_for_pid(child_pid);
    if (ps >= 0 && cs >= 0) {
        for (int i = 0; i < PROC_MAX_FDS; i++) proc_fds[cs][i] = proc_fds[ps][i];
        proc_brk[cs] = proc_brk[ps];
    }
    return child_pid;
}

int sys_wait(int pid) {
    process_t* target = process_get(pid);
    if (!target) return SYSCALL_ENOENT;
    process_t* current = process_get_current();
    if (!current) return SYSCALL_ERROR;
    if (target->ppid != current->pid) return SYSCALL_EPERM;
    if (target->state == PROC_STATE_ZOMBIE) {
        int code = target->exit_code;
        target->state = PROC_STATE_UNUSED;
        target->pid = -1;
        return code;
    }
    return SYSCALL_OK;
}

int sys_execve(const char* path, const char** argv, const char** envp) {
    (void)argv; (void)envp;
    if (!path) return SYSCALL_EINVAL;
    if (!vfs_exists(path)) return SYSCALL_ENOENT;
    return SYSCALL_ENOSYS;  // Stub — full ELF loading handled by elf.c
}

int sys_kill(int pid, int sig) {
    if (pid <= 0) return SYSCALL_EPERM;
    process_t* target = process_get(pid);
    if (!target) return SYSCALL_ENOENT;
    if (sig == SIGKILL) { process_terminate(pid, 128 + sig); return SYSCALL_OK; }
    if (sig == SIGSTOP) { process_set_state(pid, PROC_STATE_BLOCKED); return SYSCALL_OK; }
    if (sig == SIGCONT) {
        if (target->state == PROC_STATE_BLOCKED) process_set_state(pid, PROC_STATE_READY);
        return SYSCALL_OK;
    }
    return signal_send(pid, sig);
}

int sys_getppid(void) {
    process_t* c = process_get_current();
    return c ? c->ppid : SYSCALL_ERROR;
}

uint64_t sys_uptime(void) {
    scheduler_stats_t st = scheduler_get_stats();
    return st.total_ticks;
}

int sys_getprio(int pid) {
    process_t* p = process_get(pid);
    return p ? p->priority : SYSCALL_ENOENT;
}

int sys_setprio(int pid, int priority) {
    if (priority < PRIORITY_LOW || priority > PRIORITY_REALTIME) return SYSCALL_EINVAL;
    process_t* p = process_get(pid);
    if (!p) return SYSCALL_ENOENT;
    p->priority = priority;
    switch (priority) {
        case PRIORITY_REALTIME: p->default_slice = 2;  break;
        case PRIORITY_HIGH:     p->default_slice = 5;  break;
        case PRIORITY_NORMAL:   p->default_slice = 10; break;
        case PRIORITY_LOW:      p->default_slice = 20; break;
    }
    return SYSCALL_OK;
}

int sys_procinfo(int pid, procinfo_t* info) {
    if (!info) return SYSCALL_EINVAL;
    process_t* p = process_get(pid);
    if (!p) return SYSCALL_ENOENT;
    info->pid = p->pid; info->ppid = p->ppid;
    info->state = p->state; info->priority = p->priority;
    info->cpu_time = p->cpu_time;
    sys_strcpy(info->name, p->name, 32);
    return SYSCALL_OK;
}

int sys_meminfo(meminfo_t* info) {
    if (!info) return SYSCALL_EINVAL;
    info->total_memory = 512 * 1024 * 1024;
    info->used_memory = 24 * 1024 * 1024;
    info->free_memory = info->total_memory - info->used_memory;
    info->total_processes = process_count();
    info->running_processes = process_count_by_state(PROC_STATE_RUNNING);
    return SYSCALL_OK;
}

// ============ File I/O ============

int sys_open(const char* path, int flags) {
    if (!path) return SYSCALL_EINVAL;
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    int pfd = alloc_proc_fd(slot);
    if (pfd < 0) return SYSCALL_EMFILE;
    int vfs_flags = 0;
    if (flags & 0x01) vfs_flags |= VFS_O_WRONLY;
    if (flags & 0x02) vfs_flags |= VFS_O_RDWR;
    if (flags & 0x40) vfs_flags |= VFS_O_CREAT;
    if (flags & 0x200) vfs_flags |= VFS_O_TRUNC;
    if (flags & 0x400) vfs_flags |= VFS_O_APPEND;
    if (vfs_flags == 0) vfs_flags = VFS_O_RDONLY;
    int vfd = vfs_open(path, vfs_flags);
    if (vfd < 0) return SYSCALL_ENOENT;
    proc_fds[slot][pfd].vfs_fd = vfd;
    proc_fds[slot][pfd].flags = flags;
    proc_fds[slot][pfd].in_use = 1;
    return pfd;
}

int sys_close(int fd) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return SYSCALL_EBADF;
    int vfd = proc_fds[slot][fd].vfs_fd;
    if (vfd & PIPE_FD_FLAG) {
        pipe_close(vfd & ~PIPE_FD_FLAG, (proc_fds[slot][fd].flags & 0x01) ? 1 : 0);
    } else {
        vfs_close(vfd);
    }
    proc_fds[slot][fd].in_use = 0;
    return SYSCALL_OK;
}

int64_t sys_read(int fd, void* buf, uint64_t count) {
    if (!buf || count == 0) return SYSCALL_EINVAL;
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return SYSCALL_EBADF;
    int vfd = proc_fds[slot][fd].vfs_fd;
    if (vfd & PIPE_FD_FLAG) return pipe_read(vfd & ~PIPE_FD_FLAG, buf, (int)count);
    return vfs_read(vfd, buf, (uint32_t)count);
}

int64_t sys_write(int fd, const void* buf, uint64_t count) {
    if (!buf || count == 0) return SYSCALL_EINVAL;
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return SYSCALL_EBADF;
    int vfd = proc_fds[slot][fd].vfs_fd;
    if (vfd & PIPE_FD_FLAG) return pipe_write(vfd & ~PIPE_FD_FLAG, buf, (int)count);
    return vfs_write(vfd, buf, (uint32_t)count);
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return SYSCALL_EBADF;
    int vfd = proc_fds[slot][fd].vfs_fd;
    if (vfd & PIPE_FD_FLAG) return SYSCALL_ESPIPE;
    if (vfs_seek(vfd, (int32_t)offset, whence) < 0) return SYSCALL_EINVAL;
    return vfs_tell(vfd);
}

static void fill_stat(stat_t* buf, const char* path) {
    vfs_dirent_t ent;
    sys_memset(buf, 0, sizeof(stat_t));
    if (vfs_stat(path, &ent) == 0) {
        buf->st_size = ent.size;
        buf->st_mtime = ent.modified;
        buf->st_ctime = ent.created;
        buf->st_atime = ent.modified;
        buf->st_blksize = 4096;
        buf->st_blocks = (ent.size + 511) / 512;
        buf->st_nlink = 1;
        if (ent.type == VFS_DIRECTORY) buf->st_mode = S_IFDIR | 0755;
        else if (ent.type == VFS_SYMLINK) buf->st_mode = S_IFLNK | 0777;
        else {
            buf->st_mode = S_IFREG;
            if (ent.perms & VFS_PERM_READ)  buf->st_mode |= S_IRUSR | S_IRGRP | S_IROTH;
            if (ent.perms & VFS_PERM_WRITE) buf->st_mode |= S_IWUSR;
            if (ent.perms & VFS_PERM_EXEC)  buf->st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
        }
    }
}

int sys_stat(const char* path, stat_t* buf) {
    if (!path || !buf) return SYSCALL_EINVAL;
    if (!vfs_exists(path)) return SYSCALL_ENOENT;
    fill_stat(buf, path);
    return SYSCALL_OK;
}

int sys_fstat(int fd, stat_t* buf) {
    if (!buf) return SYSCALL_EINVAL;
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return SYSCALL_EBADF;
    int vfd = proc_fds[slot][fd].vfs_fd;
    if (vfd & PIPE_FD_FLAG) {
        sys_memset(buf, 0, sizeof(stat_t));
        buf->st_mode = S_IFIFO | 0600;
        buf->st_blksize = 4096;
        return SYSCALL_OK;
    }
    sys_memset(buf, 0, sizeof(stat_t));
    buf->st_mode = S_IFREG | 0644;
    buf->st_blksize = 4096;
    return SYSCALL_OK;
}

// ============ FD Manipulation ============

int sys_dup(int oldfd) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (oldfd < 0 || oldfd >= PROC_MAX_FDS || !proc_fds[slot][oldfd].in_use) return SYSCALL_EBADF;
    int newfd = alloc_proc_fd(slot);
    if (newfd < 0) return SYSCALL_EMFILE;
    proc_fds[slot][newfd] = proc_fds[slot][oldfd];
    return newfd;
}

int sys_dup2(int oldfd, int newfd) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (oldfd < 0 || oldfd >= PROC_MAX_FDS || !proc_fds[slot][oldfd].in_use) return SYSCALL_EBADF;
    if (newfd < 0 || newfd >= PROC_MAX_FDS) return SYSCALL_EINVAL;
    if (oldfd == newfd) return newfd;
    if (proc_fds[slot][newfd].in_use) sys_close(newfd);
    proc_fds[slot][newfd] = proc_fds[slot][oldfd];
    return newfd;
}

int sys_pipe(int pipefd[2]) {
    if (!pipefd) return SYSCALL_EINVAL;
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    int rfd = alloc_proc_fd(slot);
    if (rfd < 0) return SYSCALL_EMFILE;
    proc_fds[slot][rfd].in_use = 1;
    int wfd = alloc_proc_fd(slot);
    if (wfd < 0) { proc_fds[slot][rfd].in_use = 0; return SYSCALL_EMFILE; }
    int pidx = pipe_create();
    if (pidx < 0) { proc_fds[slot][rfd].in_use = 0; return SYSCALL_ENOMEM; }
    proc_fds[slot][rfd].vfs_fd = pidx | PIPE_FD_FLAG;
    proc_fds[slot][rfd].flags = 0;
    proc_fds[slot][wfd].vfs_fd = pidx | PIPE_FD_FLAG;
    proc_fds[slot][wfd].flags = 1;
    proc_fds[slot][wfd].in_use = 1;
    pipefd[0] = rfd; pipefd[1] = wfd;
    return SYSCALL_OK;
}

// ============ Directory Operations ============

int sys_getcwd(char* buf, uint64_t size) {
    if (!buf || size == 0) return SYSCALL_EINVAL;
    const char* cwd = vfs_getcwd();
    if ((uint64_t)(sys_strlen(cwd) + 1) > size) return SYSCALL_EINVAL;
    sys_strcpy(buf, cwd, (int)size);
    return SYSCALL_OK;
}

int sys_chdir(const char* path) {
    if (!path) return SYSCALL_EINVAL;
    if (!vfs_exists(path)) return SYSCALL_ENOENT;
    if (!vfs_is_dir(path)) return SYSCALL_ENOTDIR;
    return vfs_chdir(path) == 0 ? SYSCALL_OK : SYSCALL_ERROR;
}

int sys_mkdir(const char* path, uint32_t mode) {
    (void)mode;
    if (!path) return SYSCALL_EINVAL;
    if (vfs_exists(path)) return SYSCALL_EEXIST;
    return vfs_mkdir(path) >= 0 ? SYSCALL_OK : SYSCALL_ERROR;
}

int sys_rmdir(const char* path) {
    if (!path) return SYSCALL_EINVAL;
    if (!vfs_exists(path)) return SYSCALL_ENOENT;
    if (!vfs_is_dir(path)) return SYSCALL_ENOTDIR;
    return vfs_rmdir(path) == 0 ? SYSCALL_OK : SYSCALL_ERROR;
}

int sys_unlink(const char* path) {
    if (!path) return SYSCALL_EINVAL;
    if (!vfs_exists(path)) return SYSCALL_ENOENT;
    if (vfs_is_dir(path)) return SYSCALL_EISDIR;
    return vfs_delete(path) == 0 ? SYSCALL_OK : SYSCALL_ERROR;
}

int sys_readdir(int fd, sys_dirent_t* entry) {
    (void)fd; (void)entry;
    return SYSCALL_ENOSYS;
}

// ============ Memory Management ============

uint64_t sys_mmap(mmap_args_t* args) {
    if (!args || args->length == 0) return (uint64_t)SYSCALL_EINVAL;
    if (!(args->flags & MMAP_MAP_ANON)) return (uint64_t)SYSCALL_ENOSYS;
    uint64_t size = (args->length + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    uint64_t addr = args->addr;
    if (!addr || !(args->flags & MMAP_MAP_FIXED)) {
        static uint64_t mmap_next = 0x10000000;
        addr = mmap_next;
        mmap_next += size;
        if (mmap_next >= 0x40000000) return (uint64_t)SYSCALL_ENOMEM;
    }
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (args->prot & MMAP_PROT_WRITE) flags |= VMM_FLAG_WRITABLE;
    if (!(args->prot & MMAP_PROT_EXEC)) flags |= VMM_FLAG_NX;
    pte_t* pml4 = vmm_get_current_address_space();
    if (vmm_alloc_pages(pml4, addr, size, flags) < 0) return (uint64_t)SYSCALL_ENOMEM;
    sys_memset((void*)addr, 0, size);
    return addr;
}

int sys_munmap(uint64_t addr, uint64_t length) {
    if (addr == 0 || length == 0) return SYSCALL_EINVAL;
    if (addr & (VMM_PAGE_SIZE - 1)) return SYSCALL_EINVAL;
    uint64_t size = (length + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    pte_t* pml4 = vmm_get_current_address_space();
    for (uint64_t off = 0; off < size; off += VMM_PAGE_SIZE) {
        vmm_unmap_page(pml4, addr + off);
        vmm_invlpg(addr + off);
    }
    return SYSCALL_OK;
}

uint64_t sys_brk(uint64_t addr) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return 0;
    uint64_t current_brk = proc_brk[slot];
    if (addr == 0) return current_brk;
    uint64_t new_brk = (addr + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    uint64_t old_brk_a = (current_brk + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    if (new_brk > old_brk_a) {
        pte_t* pml4 = vmm_get_current_address_space();
        uint64_t fl = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
        for (uint64_t pg = old_brk_a; pg < new_brk; pg += VMM_PAGE_SIZE) {
            if (vmm_alloc_pages(pml4, pg, VMM_PAGE_SIZE, fl) < 0) return current_brk;
        }
    }
    proc_brk[slot] = addr;
    return addr;
}

// ============ Device / File Control ============

int sys_ioctl(int fd, uint64_t request, uint64_t arg) {
    (void)fd; (void)request; (void)arg;
    return SYSCALL_ENOSYS;
}

int sys_fcntl(int fd, int cmd, uint64_t arg) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return SYSCALL_EBADF;
    switch (cmd) {
        case F_DUPFD:  return sys_dup(fd);
        case F_GETFD:  return 0;
        case F_SETFD:  return SYSCALL_OK;
        case F_GETFL:  return proc_fds[slot][fd].flags;
        case F_SETFL:  proc_fds[slot][fd].flags = (int)arg; return SYSCALL_OK;
        default:       return SYSCALL_EINVAL;
    }
}

int sys_poll(pollfd_t* fds, int nfds, int timeout) {
    (void)timeout;
    if (!fds || nfds <= 0) return SYSCALL_EINVAL;
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return SYSCALL_ERROR;
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        int pfd = fds[i].fd;
        if (pfd < 0 || pfd >= PROC_MAX_FDS || !proc_fds[slot][pfd].in_use) {
            fds[i].revents = POLLNVAL; continue;
        }
        int vfd = proc_fds[slot][pfd].vfs_fd;
        if (vfd & PIPE_FD_FLAG) {
            int avail = pipe_available(vfd & ~PIPE_FD_FLAG);
            if ((fds[i].events & POLLIN) && avail > 0) fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
        } else {
            if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
        }
        if (fds[i].revents) ready++;
    }
    return ready;
}

// ============ Signals ============

int sys_sigaction(int sig, const sigaction_t* act, sigaction_t* oldact) {
    if (sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) return SYSCALL_EINVAL;
    return signal_sigaction(process_get_pid(), sig, act, oldact);
}

int sys_sigreturn(void) { return signal_return(process_get_pid()); }

int sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset) {
    return signal_procmask(process_get_pid(), how, set, oldset);
}

// ============ Shared Memory ============

int sys_shmget(uint64_t key, uint64_t size, int flags) { return shm_get(key, size, flags); }
uint64_t sys_shmat(int shmid, uint64_t addr, int flags) {
    (void)flags; return shm_attach(shmid, process_get_pid(), addr);
}
int sys_shmdt(uint64_t addr) { return shm_detach(process_get_pid(), addr); }

// ============ Misc ============

int sys_getuid(void) { return 0; }
int sys_getgid(void) { return 0; }
int sys_isatty(int fd) {
    int slot = proc_slot_for_pid(process_get_pid());
    if (slot < 0) return 0;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc_fds[slot][fd].in_use) return 0;
    return (fd <= 2) ? 1 : 0;
}
uint64_t sys_clock(void) { scheduler_stats_t st = scheduler_get_stats(); return st.total_ticks; }

// ============ Dispatcher ============

int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (num) {
        case SYS_EXIT:       return (int64_t)sys_exit((int)a1);
        case SYS_GETPID:     return (int64_t)sys_getpid();
        case SYS_YIELD:      return (int64_t)sys_yield();
        case SYS_SLEEP:      return (int64_t)sys_sleep(a1);
        case SYS_FORK:       return (int64_t)sys_fork();
        case SYS_WAIT:       return (int64_t)sys_wait((int)a1);
        case SYS_EXECVE:     return (int64_t)sys_execve((const char*)a1, (const char**)a2, (const char**)a3);
        case SYS_KILL:       return (int64_t)sys_kill((int)a1, (int)a2);
        case SYS_GETPPID:    return (int64_t)sys_getppid();
        case SYS_UPTIME:     return (int64_t)sys_uptime();
        case SYS_GETPRIO:    return (int64_t)sys_getprio((int)a1);
        case SYS_SETPRIO:    return (int64_t)sys_setprio((int)a1, (int)a2);
        case SYS_PROCINFO:   return (int64_t)sys_procinfo((int)a1, (procinfo_t*)a2);
        case SYS_MEMINFO:    return (int64_t)sys_meminfo((meminfo_t*)a1);
        case SYS_WRITE:      return sys_write((int)a1, (const void*)a2, a3);
        case SYS_READ:       return sys_read((int)a1, (void*)a2, a3);
        case SYS_OPEN:       return (int64_t)sys_open((const char*)a1, (int)a2);
        case SYS_CLOSE:      return (int64_t)sys_close((int)a1);
        case SYS_LSEEK:      return sys_lseek((int)a1, (int64_t)a2, (int)a3);
        case SYS_STAT:       return (int64_t)sys_stat((const char*)a1, (stat_t*)a2);
        case SYS_FSTAT:      return (int64_t)sys_fstat((int)a1, (stat_t*)a2);
        case SYS_DUP:        return (int64_t)sys_dup((int)a1);
        case SYS_DUP2:       return (int64_t)sys_dup2((int)a1, (int)a2);
        case SYS_PIPE:       return (int64_t)sys_pipe((int*)a1);
        case SYS_GETCWD:     return (int64_t)sys_getcwd((char*)a1, a2);
        case SYS_CHDIR:      return (int64_t)sys_chdir((const char*)a1);
        case SYS_MKDIR:      return (int64_t)sys_mkdir((const char*)a1, (uint32_t)a2);
        case SYS_RMDIR:      return (int64_t)sys_rmdir((const char*)a1);
        case SYS_UNLINK:     return (int64_t)sys_unlink((const char*)a1);
        case SYS_READDIR:    return (int64_t)sys_readdir((int)a1, (sys_dirent_t*)a2);
        case SYS_MMAP:       return (int64_t)sys_mmap((mmap_args_t*)a1);
        case SYS_MUNMAP:     return (int64_t)sys_munmap(a1, a2);
        case SYS_BRK:        return (int64_t)sys_brk(a1);
        case SYS_IOCTL:      return (int64_t)sys_ioctl((int)a1, a2, a3);
        case SYS_FCNTL:      return (int64_t)sys_fcntl((int)a1, (int)a2, a3);
        case SYS_POLL:       return (int64_t)sys_poll((pollfd_t*)a1, (int)a2, (int)a3);
        case SYS_SIGACTION:  return (int64_t)sys_sigaction((int)a1, (const sigaction_t*)a2, (sigaction_t*)a3);
        case SYS_SIGRETURN:  return (int64_t)sys_sigreturn();
        case SYS_SIGPROCMASK:return (int64_t)sys_sigprocmask((int)a1, (const uint64_t*)a2, (uint64_t*)a3);
        case SYS_SHMGET:     return (int64_t)sys_shmget(a1, a2, (int)a3);
        case SYS_SHMAT:      return (int64_t)sys_shmat((int)a1, a2, (int)a3);
        case SYS_SHMDT:      return (int64_t)sys_shmdt(a1);
        case SYS_GETUID:     return (int64_t)sys_getuid();
        case SYS_GETGID:     return (int64_t)sys_getgid();
        case SYS_ISATTY:     return (int64_t)sys_isatty((int)a1);
        case SYS_CLOCK:      return (int64_t)sys_clock();
        default:             return (int64_t)SYSCALL_ENOSYS;
    }
}
