// procfs.c - Process Filesystem for Alteo OS
// Implements /proc with meminfo, cpuinfo, uptime, and per-process status
#include "procfs.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "pmm.h"

// ---- String helpers ----
static int pfs_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void pfs_strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
static int pfs_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static void pfs_strncpy(char* d, const char* s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void pfs_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}
static void pfs_memcpy(void* d, const void* s, int n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
}

// Integer to string
static int pfs_itoa(int64_t val, char* buf, int bufsize) {
    if (bufsize <= 0) return 0;
    char tmp[24];
    int neg = 0;
    uint64_t v;
    if (val < 0) { neg = 1; v = (uint64_t)(-val); }
    else { v = (uint64_t)val; }

    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v > 0 && i < 22) { tmp[i++] = '0' + (int)(v % 10); v /= 10; } }
    if (neg && i < 22) tmp[i++] = '-';

    int len = 0;
    for (int j = i - 1; j >= 0 && len < bufsize - 1; j--) {
        buf[len++] = tmp[j];
    }
    buf[len] = 0;
    return len;
}

// Append string to buffer
static int pfs_append(char* buf, int pos, int max, const char* str) {
    while (*str && pos < max - 1) {
        buf[pos++] = *str++;
    }
    buf[pos] = 0;
    return pos;
}

// Append number to buffer
static int pfs_append_num(char* buf, int pos, int max, int64_t val) {
    char tmp[24];
    pfs_itoa(val, tmp, sizeof(tmp));
    return pfs_append(buf, pos, max, tmp);
}

// ---- Procfs content generators ----

// Generate /proc/meminfo content
static int generate_meminfo(char* buf, int bufsize) {
    uint64_t total = 512 * 1024; // KB (512MB)
    uint64_t used  = 24 * 1024;  // KB (approximate)
    uint64_t free_mem = total - used;

    int pos = 0;
    pos = pfs_append(buf, pos, bufsize, "MemTotal:       ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)total);
    pos = pfs_append(buf, pos, bufsize, " kB\n");
    pos = pfs_append(buf, pos, bufsize, "MemFree:        ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)free_mem);
    pos = pfs_append(buf, pos, bufsize, " kB\n");
    pos = pfs_append(buf, pos, bufsize, "MemUsed:        ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)used);
    pos = pfs_append(buf, pos, bufsize, " kB\n");
    pos = pfs_append(buf, pos, bufsize, "Buffers:        0 kB\n");
    pos = pfs_append(buf, pos, bufsize, "Cached:         0 kB\n");
    return pos;
}

// Generate /proc/cpuinfo content
static int generate_cpuinfo(char* buf, int bufsize) {
    int pos = 0;
    pos = pfs_append(buf, pos, bufsize, "processor\t: 0\n");
    pos = pfs_append(buf, pos, bufsize, "vendor_id\t: AlteoOS\n");
    pos = pfs_append(buf, pos, bufsize, "model name\t: Alteo Virtual CPU\n");
    pos = pfs_append(buf, pos, bufsize, "cpu MHz\t\t: 3000.000\n");
    pos = pfs_append(buf, pos, bufsize, "cache size\t: 4096 KB\n");
    pos = pfs_append(buf, pos, bufsize, "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic\n");
    pos = pfs_append(buf, pos, bufsize, "bogomips\t: 6000.00\n");
    pos = pfs_append(buf, pos, bufsize, "address sizes\t: 48 bits virtual, 40 bits physical\n");
    return pos;
}

// Generate /proc/uptime content
static int generate_uptime(char* buf, int bufsize) {
    scheduler_stats_t st = scheduler_get_stats();
    uint64_t seconds = st.total_ticks / 100; // Assuming 100Hz timer
    uint64_t idle_sec = st.idle_ticks / 100;

    int pos = 0;
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)seconds);
    pos = pfs_append(buf, pos, bufsize, ".");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)(st.total_ticks % 100));
    pos = pfs_append(buf, pos, bufsize, " ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)idle_sec);
    pos = pfs_append(buf, pos, bufsize, ".");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)(st.idle_ticks % 100));
    pos = pfs_append(buf, pos, bufsize, "\n");
    return pos;
}

// Generate /proc/version content
static int generate_version(char* buf, int bufsize) {
    return pfs_append(buf, 0, bufsize, "Alteo OS v5.0 (x86_64) #1 SMP\n");
}

// Generate /proc/stat content
static int generate_stat(char* buf, int bufsize) {
    scheduler_stats_t st = scheduler_get_stats();
    int pos = 0;
    pos = pfs_append(buf, pos, bufsize, "cpu  ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)(st.total_ticks - st.idle_ticks));
    pos = pfs_append(buf, pos, bufsize, " 0 0 ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)st.idle_ticks);
    pos = pfs_append(buf, pos, bufsize, " 0 0 0 0 0 0\n");
    pos = pfs_append(buf, pos, bufsize, "processes ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)process_count());
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "procs_running ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)process_count_by_state(PROC_STATE_RUNNING));
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "ctxt ");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)st.total_switches);
    pos = pfs_append(buf, pos, bufsize, "\n");
    return pos;
}

// Generate /proc/<pid>/status content
static int generate_pid_status(int pid, char* buf, int bufsize) {
    process_t* p = process_get(pid);
    if (!p) return 0;

    int pos = 0;
    pos = pfs_append(buf, pos, bufsize, "Name:\t");
    pos = pfs_append(buf, pos, bufsize, p->name);
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "State:\t");
    pos = pfs_append(buf, pos, bufsize, process_state_name(p->state));
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "Pid:\t");
    pos = pfs_append_num(buf, pos, bufsize, p->pid);
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "PPid:\t");
    pos = pfs_append_num(buf, pos, bufsize, p->ppid);
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "Priority:\t");
    pos = pfs_append_num(buf, pos, bufsize, p->priority);
    pos = pfs_append(buf, pos, bufsize, "\n");
    pos = pfs_append(buf, pos, bufsize, "CpuTime:\t");
    pos = pfs_append_num(buf, pos, bufsize, (int64_t)p->cpu_time);
    pos = pfs_append(buf, pos, bufsize, "\n");
    return pos;
}

// ---- procfs VFS integration ----

// Content cache for procfs files (regenerated on open)
#define PROCFS_MAX_FDS     16
#define PROCFS_BUF_SIZE    2048

typedef struct {
    char     buf[PROCFS_BUF_SIZE];
    int      size;
    uint32_t offset;
    int      in_use;
} procfs_fd_t;

static procfs_fd_t procfs_fds[PROCFS_MAX_FDS];

// Known procfs entry types
enum {
    PROCFS_MEMINFO = 1,
    PROCFS_CPUINFO,
    PROCFS_UPTIME,
    PROCFS_VERSION,
    PROCFS_STAT,
    PROCFS_PID_STATUS,
};

static int identify_proc_file(const char* path) {
    // Strip leading "/proc/" or "/"
    const char* p = path;
    if (p[0] == '/') p++;
    if (pfs_strcmp(p, "meminfo") == 0 || pfs_strcmp(p, "proc/meminfo") == 0) return PROCFS_MEMINFO;
    if (pfs_strcmp(p, "cpuinfo") == 0 || pfs_strcmp(p, "proc/cpuinfo") == 0) return PROCFS_CPUINFO;
    if (pfs_strcmp(p, "uptime") == 0 || pfs_strcmp(p, "proc/uptime") == 0)   return PROCFS_UPTIME;
    if (pfs_strcmp(p, "version") == 0 || pfs_strcmp(p, "proc/version") == 0) return PROCFS_VERSION;
    if (pfs_strcmp(p, "stat") == 0 || pfs_strcmp(p, "proc/stat") == 0)       return PROCFS_STAT;
    return 0;
}

static int procfs_open(void* fs_data, const char* path, int flags) {
    (void)fs_data; (void)flags;

    int type = identify_proc_file(path);
    if (type == 0) return -1;

    // Find a free fd
    int fd = -1;
    for (int i = 0; i < PROCFS_MAX_FDS; i++) {
        if (!procfs_fds[i].in_use) { fd = i; break; }
    }
    if (fd < 0) return -1;

    procfs_fds[fd].in_use = 1;
    procfs_fds[fd].offset = 0;

    // Generate content
    switch (type) {
        case PROCFS_MEMINFO:
            procfs_fds[fd].size = generate_meminfo(procfs_fds[fd].buf, PROCFS_BUF_SIZE);
            break;
        case PROCFS_CPUINFO:
            procfs_fds[fd].size = generate_cpuinfo(procfs_fds[fd].buf, PROCFS_BUF_SIZE);
            break;
        case PROCFS_UPTIME:
            procfs_fds[fd].size = generate_uptime(procfs_fds[fd].buf, PROCFS_BUF_SIZE);
            break;
        case PROCFS_VERSION:
            procfs_fds[fd].size = generate_version(procfs_fds[fd].buf, PROCFS_BUF_SIZE);
            break;
        case PROCFS_STAT:
            procfs_fds[fd].size = generate_stat(procfs_fds[fd].buf, PROCFS_BUF_SIZE);
            break;
        default:
            procfs_fds[fd].in_use = 0;
            return -1;
    }

    return fd;
}

static int procfs_close(void* fs_data, int fd) {
    (void)fs_data;
    if (fd < 0 || fd >= PROCFS_MAX_FDS || !procfs_fds[fd].in_use) return -1;
    procfs_fds[fd].in_use = 0;
    return 0;
}

static int procfs_read(void* fs_data, int fd, void* buf, uint32_t count) {
    (void)fs_data;
    if (fd < 0 || fd >= PROCFS_MAX_FDS || !procfs_fds[fd].in_use) return -1;

    procfs_fd_t* pfd = &procfs_fds[fd];
    uint32_t avail = (uint32_t)pfd->size - pfd->offset;
    if (count > avail) count = avail;
    if (count == 0) return 0;

    pfs_memcpy(buf, pfd->buf + pfd->offset, (int)count);
    pfd->offset += count;
    return (int)count;
}

static int procfs_write(void* fs_data, int fd, const void* buf, uint32_t count) {
    (void)fs_data; (void)fd; (void)buf; (void)count;
    return -1; // procfs is read-only
}

static int procfs_readdir(void* fs_data, const char* path, vfs_dirent_t* entries, int max) {
    (void)fs_data; (void)path;
    int count = 0;

    // Static entries
    const char* names[] = {"meminfo", "cpuinfo", "uptime", "version", "stat"};
    for (int i = 0; i < 5 && count < max; i++) {
        pfs_strncpy(entries[count].name, names[i], VFS_MAX_NAME);
        entries[count].type = VFS_FILE;
        entries[count].size = 0;
        entries[count].perms = VFS_PERM_READ;
        count++;
    }

    // Per-process directories
    process_t* table = process_get_table();
    for (int i = 0; i < process_get_max() && count < max; i++) {
        if (table[i].state != PROC_STATE_UNUSED && table[i].pid >= 0) {
            char name[16];
            pfs_itoa(table[i].pid, name, sizeof(name));
            pfs_strncpy(entries[count].name, name, VFS_MAX_NAME);
            entries[count].type = VFS_DIRECTORY;
            entries[count].size = 0;
            entries[count].perms = VFS_PERM_READ | VFS_PERM_EXEC;
            count++;
        }
    }

    return count;
}

static int procfs_stat_op(void* fs_data, const char* path, vfs_dirent_t* out) {
    (void)fs_data;
    int type = identify_proc_file(path);
    if (type > 0) {
        const char* p = path;
        while (*p == '/') p++;
        // Find last component
        const char* last = p;
        while (*p) { if (*p == '/' && *(p+1)) last = p+1; p++; }
        pfs_strncpy(out->name, last, VFS_MAX_NAME);
        out->type = VFS_FILE;
        out->size = 0;
        out->perms = VFS_PERM_READ;
        return 0;
    }
    return -1;
}

static int procfs_mkdir(void* fs_data, const char* path) {
    (void)fs_data; (void)path;
    return -1;
}

static int procfs_create(void* fs_data, const char* path, uint8_t type, uint8_t perms) {
    (void)fs_data; (void)path; (void)type; (void)perms;
    return -1;
}

static int procfs_delete(void* fs_data, const char* path) {
    (void)fs_data; (void)path;
    return -1;
}

static vfs_fs_ops_t procfs_ops = {
    .open    = procfs_open,
    .close   = procfs_close,
    .read    = procfs_read,
    .write   = procfs_write,
    .readdir = procfs_readdir,
    .mkdir   = procfs_mkdir,
    .stat    = procfs_stat_op,
    .create  = procfs_create,
    .delete  = procfs_delete,
};

// ---- Public API ----

vfs_fs_ops_t* procfs_get_ops(void) {
    return &procfs_ops;
}

void procfs_init(void) {
    pfs_memset(procfs_fds, 0, sizeof(procfs_fds));

    // Ensure /proc exists in VFS
    if (!vfs_exists("/proc")) {
        vfs_mkdir("/proc");
    }

    // Create well-known /proc files
    vfs_create("/proc/meminfo", VFS_FILE, VFS_PERM_READ);
    vfs_create("/proc/cpuinfo", VFS_FILE, VFS_PERM_READ);
    vfs_create("/proc/uptime", VFS_FILE, VFS_PERM_READ);
    vfs_create("/proc/version", VFS_FILE, VFS_PERM_READ);
    vfs_create("/proc/stat", VFS_FILE, VFS_PERM_READ);

    // Pre-populate /proc/version content into VFS node
    {
        int fd = vfs_open("/proc/version", VFS_O_WRONLY);
        if (fd >= 0) {
            const char* ver = "Alteo OS v5.0 (x86_64) #1 SMP\n";
            vfs_write(fd, ver, pfs_strlen(ver));
            vfs_close(fd);
        }
    }

    // Mount procfs
    vfs_mount("/proc", "procfs", &procfs_ops, 0);
}

void procfs_refresh(void) {
    // Update /proc/meminfo content in VFS
    {
        char buf[PROCFS_BUF_SIZE];
        int len = generate_meminfo(buf, PROCFS_BUF_SIZE);
        int fd = vfs_open("/proc/meminfo", VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            vfs_write(fd, buf, (uint32_t)len);
            vfs_close(fd);
        }
    }

    // Update /proc/uptime
    {
        char buf[PROCFS_BUF_SIZE];
        int len = generate_uptime(buf, PROCFS_BUF_SIZE);
        int fd = vfs_open("/proc/uptime", VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            vfs_write(fd, buf, (uint32_t)len);
            vfs_close(fd);
        }
    }

    // Update /proc/stat
    {
        char buf[PROCFS_BUF_SIZE];
        int len = generate_stat(buf, PROCFS_BUF_SIZE);
        int fd = vfs_open("/proc/stat", VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            vfs_write(fd, buf, (uint32_t)len);
            vfs_close(fd);
        }
    }
}
