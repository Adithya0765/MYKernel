// syscall.h - System Call Interface for Alteo OS
// POSIX-compatible syscall layer for libc support
#ifndef SYSCALL_H
#define SYSCALL_H

#include "stdint.h"

// ---- System Call Numbers ----
// Process management
#define SYS_EXIT         0
#define SYS_GETPID       1
#define SYS_YIELD        2
#define SYS_SLEEP        3
#define SYS_FORK         4
#define SYS_WAIT         5
#define SYS_EXECVE       6
#define SYS_KILL         7
#define SYS_GETPPID      8
#define SYS_UPTIME       9
#define SYS_GETPRIO      10
#define SYS_SETPRIO      11
#define SYS_PROCINFO     12
#define SYS_MEMINFO      13

// File I/O (VFS-backed)
#define SYS_WRITE        14
#define SYS_READ         15
#define SYS_OPEN         16
#define SYS_CLOSE        17
#define SYS_LSEEK        18
#define SYS_STAT         19
#define SYS_FSTAT        20

// File descriptor manipulation
#define SYS_DUP          21
#define SYS_DUP2         22
#define SYS_PIPE         23

// Directory / path operations
#define SYS_GETCWD       24
#define SYS_CHDIR        25
#define SYS_MKDIR        26
#define SYS_RMDIR        27
#define SYS_UNLINK       28
#define SYS_READDIR      29

// Memory management
#define SYS_MMAP         30
#define SYS_MUNMAP       31
#define SYS_BRK          32

// Device / file control
#define SYS_IOCTL        33
#define SYS_FCNTL        34
#define SYS_POLL         35

// Signals
#define SYS_SIGACTION    36
#define SYS_SIGRETURN    37
#define SYS_SIGPROCMASK  38

// IPC - Shared memory
#define SYS_SHMGET       39
#define SYS_SHMAT        40
#define SYS_SHMDT        41

// Misc
#define SYS_GETUID       42
#define SYS_GETGID       43
#define SYS_ISATTY       44
#define SYS_CLOCK        45

#define NUM_SYSCALLS     46

// ---- Result Codes (POSIX-inspired errno values) ----
#define SYSCALL_OK        0
#define SYSCALL_ERROR    -1
#define SYSCALL_EPERM    -2    // Operation not permitted
#define SYSCALL_ENOENT   -3    // No such file or directory
#define SYSCALL_ENOMEM   -4    // Out of memory
#define SYSCALL_EINVAL   -5    // Invalid argument
#define SYSCALL_EBADF    -6    // Bad file descriptor
#define SYSCALL_EEXIST   -7    // File exists
#define SYSCALL_ENOTDIR  -8    // Not a directory
#define SYSCALL_EISDIR   -9    // Is a directory
#define SYSCALL_ENOSYS   -10   // Function not implemented
#define SYSCALL_EAGAIN   -11   // Resource temporarily unavailable
#define SYSCALL_EPIPE    -12   // Broken pipe
#define SYSCALL_EACCES   -13   // Permission denied
#define SYSCALL_EFAULT   -14   // Bad address
#define SYSCALL_ENODEV   -15   // No such device
#define SYSCALL_EMFILE   -16   // Too many open files
#define SYSCALL_ENOSPC   -17   // No space left on device
#define SYSCALL_ESPIPE   -18   // Illegal seek (on pipe)
#define SYSCALL_ECHILD   -19   // No child processes

// ---- mmap flags ----
#define MMAP_PROT_READ    0x1
#define MMAP_PROT_WRITE   0x2
#define MMAP_PROT_EXEC    0x4
#define MMAP_MAP_PRIVATE  0x02
#define MMAP_MAP_ANON     0x20
#define MMAP_MAP_FIXED    0x10

// ---- fcntl commands ----
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

// ---- poll events ----
#define POLLIN    0x001
#define POLLOUT   0x004
#define POLLERR   0x008
#define POLLHUP   0x010
#define POLLNVAL  0x020

typedef struct {
    int fd;
    short events;
    short revents;
} pollfd_t;

// ---- mmap arguments (packed into struct since we only have 4 args) ----
typedef struct {
    uint64_t addr;
    uint64_t length;
    int      prot;
    int      flags;
    int      fd;
    uint64_t offset;
} mmap_args_t;

// ---- stat structure ----
typedef struct {
    uint32_t st_mode;       // File type and permissions
    uint32_t st_nlink;      // Number of hard links
    uint32_t st_uid;        // Owner UID
    uint32_t st_gid;        // Owner GID
    uint64_t st_size;       // File size in bytes
    uint64_t st_atime;      // Last access time
    uint64_t st_mtime;      // Last modification time
    uint64_t st_ctime;      // Last status change time
    uint32_t st_dev;        // Device ID
    uint32_t st_ino;        // Inode number
    uint32_t st_blksize;    // Block size for I/O
    uint64_t st_blocks;     // Number of 512B blocks
} stat_t;

// stat st_mode bits
#define S_IFMT    0170000   // File type mask
#define S_IFREG   0100000   // Regular file
#define S_IFDIR   0040000   // Directory
#define S_IFCHR   0020000   // Character device
#define S_IFBLK   0060000   // Block device
#define S_IFIFO   0010000   // FIFO/pipe
#define S_IFLNK   0120000   // Symbolic link

#define S_IRUSR   00400     // User read
#define S_IWUSR   00200     // User write
#define S_IXUSR   00100     // User execute
#define S_IRGRP   00040     // Group read
#define S_IWGRP   00020     // Group write
#define S_IXGRP   00010     // Group execute
#define S_IROTH   00004     // Other read
#define S_IWOTH   00002     // Other write
#define S_IXOTH   00001     // Other execute

// ---- Memory info structure ----
typedef struct {
    uint64_t total_memory;
    uint64_t used_memory;
    uint64_t free_memory;
    int total_processes;
    int running_processes;
} meminfo_t;

// ---- Process info structure ----
typedef struct {
    int pid;
    int ppid;
    int state;
    int priority;
    uint64_t cpu_time;
    char name[32];
} procinfo_t;

// ---- Directory entry for SYS_READDIR ----
typedef struct {
    char     d_name[256];
    uint32_t d_type;
    uint64_t d_ino;
} sys_dirent_t;

// ---- Signal types (POSIX) ----
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define NSIG      32

#define SIG_DFL   ((void(*)(int))0)
#define SIG_IGN   ((void(*)(int))1)
#define SIG_ERR   ((void(*)(int))-1)

// Signal action structure
typedef struct {
    void (*sa_handler)(int);
    uint64_t sa_mask;        // Blocked signals during handler
    int      sa_flags;
} sigaction_t;

// ---- Function Prototypes ----

// Initialize system call interface
void syscall_init(void);

// System call dispatcher (called from switch.asm syscall_entry)
int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3);

// Process management syscalls
int      sys_exit(int exit_code);
int      sys_getpid(void);
int      sys_yield(void);
int      sys_sleep(uint64_t ticks);
int      sys_fork(void);
int      sys_wait(int pid);
int      sys_execve(const char* path, const char** argv, const char** envp);
int      sys_kill(int pid, int sig);
int      sys_getppid(void);
uint64_t sys_uptime(void);
int      sys_getprio(int pid);
int      sys_setprio(int pid, int priority);
int      sys_procinfo(int pid, procinfo_t* info);
int      sys_meminfo(meminfo_t* info);

// File I/O syscalls
int      sys_open(const char* path, int flags);
int      sys_close(int fd);
int64_t  sys_read(int fd, void* buf, uint64_t count);
int64_t  sys_write(int fd, const void* buf, uint64_t count);
int64_t  sys_lseek(int fd, int64_t offset, int whence);
int      sys_stat(const char* path, stat_t* buf);
int      sys_fstat(int fd, stat_t* buf);

// FD manipulation
int      sys_dup(int oldfd);
int      sys_dup2(int oldfd, int newfd);
int      sys_pipe(int pipefd[2]);

// Directory operations
int      sys_getcwd(char* buf, uint64_t size);
int      sys_chdir(const char* path);
int      sys_mkdir(const char* path, uint32_t mode);
int      sys_rmdir(const char* path);
int      sys_unlink(const char* path);
int      sys_readdir(int fd, sys_dirent_t* entry);

// Memory management
uint64_t sys_mmap(mmap_args_t* args);
int      sys_munmap(uint64_t addr, uint64_t length);
uint64_t sys_brk(uint64_t addr);

// Device / file control
int      sys_ioctl(int fd, uint64_t request, uint64_t arg);
int      sys_fcntl(int fd, int cmd, uint64_t arg);
int      sys_poll(pollfd_t* fds, int nfds, int timeout);

// Signals
int      sys_sigaction(int sig, const sigaction_t* act, sigaction_t* oldact);
int      sys_sigreturn(void);
int      sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset);

// IPC - Shared memory
int      sys_shmget(uint64_t key, uint64_t size, int flags);
uint64_t sys_shmat(int shmid, uint64_t addr, int flags);
int      sys_shmdt(uint64_t addr);

// Misc
int      sys_getuid(void);
int      sys_getgid(void);
int      sys_isatty(int fd);
uint64_t sys_clock(void);

#endif
