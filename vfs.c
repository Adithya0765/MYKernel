// vfs.c - Virtual File System Layer for Alteo OS
// Provides an in-memory filesystem with directories, files, and permissions
#include "vfs.h"

// ---- String helpers (no libc) ----
static int vfs_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void vfs_strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
static int vfs_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static int vfs_strncpy(char* d, const char* s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
    return i;
}
static void vfs_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}
static void vfs_memcpy(void* dst, const void* src, int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

// ---- Node storage ----
static vfs_node_t nodes[VFS_MAX_FILES];
static vfs_fd_t fds[VFS_MAX_OPEN];
static char cwd[VFS_MAX_PATH] = "/";
static int vfs_initialized = 0;
static vfs_mount_t mounts[VFS_MAX_MOUNTS];

// ---- Internal helpers ----
static int alloc_node(void) {
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!nodes[i].in_use) return i;
    }
    return -1;
}

static int alloc_fd(void) {
    for (int i = 0; i < VFS_MAX_OPEN; i++) {
        if (!fds[i].in_use) return i;
    }
    return -1;
}

// Parse path into components, resolve node
// Returns node index or -1
static int resolve_path(const char* path) {
    if (!path || !path[0]) return -1;

    int cur = 0; // start at root

    // Absolute vs relative
    const char* p = path;
    if (p[0] == '/') {
        cur = 0;
        p++;
    } else {
        // Resolve from cwd
        cur = resolve_path(cwd);
        if (cur < 0) cur = 0;
    }

    if (!*p) return cur; // path was just "/"

    // Walk each component
    char component[VFS_MAX_NAME];
    while (*p) {
        // Skip slashes
        while (*p == '/') p++;
        if (!*p) break;

        // Extract component
        int ci = 0;
        while (*p && *p != '/' && ci < VFS_MAX_NAME - 1) {
            component[ci++] = *p++;
        }
        component[ci] = 0;

        if (vfs_strcmp(component, ".") == 0) continue;
        if (vfs_strcmp(component, "..") == 0) {
            if (nodes[cur].parent >= 0) cur = nodes[cur].parent;
            continue;
        }

        // Find child named component
        int found = -1;
        for (int i = 0; i < nodes[cur].child_count; i++) {
            int child = nodes[cur].children[i];
            if (child >= 0 && nodes[child].in_use &&
                vfs_strcmp(nodes[child].name, component) == 0) {
                found = child;
                break;
            }
        }
        if (found < 0) return -1; // not found
        cur = found;
    }
    return cur;
}

// Get parent directory and last component from a path
static int resolve_parent(const char* path, char* last_component) {
    if (!path || !path[0]) return -1;

    // Copy path to work with
    char buf[VFS_MAX_PATH];
    vfs_strncpy(buf, path, VFS_MAX_PATH);

    // Remove trailing slashes
    int len = vfs_strlen(buf);
    while (len > 1 && buf[len - 1] == '/') { buf[--len] = 0; }

    // Find last slash
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '/') last_slash = i;
    }

    if (last_slash < 0) {
        // Relative path, parent is cwd
        vfs_strcpy(last_component, buf);
        return resolve_path(cwd);
    }

    // Extract last component
    vfs_strcpy(last_component, buf + last_slash + 1);

    // Truncate to get parent path
    if (last_slash == 0) {
        return 0; // parent is root
    }
    buf[last_slash] = 0;
    return resolve_path(buf);
}

// ---- Public API ----

void vfs_init(void) {
    vfs_memset(nodes, 0, sizeof(nodes));
    vfs_memset(fds, 0, sizeof(fds));
    vfs_memset(mounts, 0, sizeof(mounts));
    vfs_strcpy(cwd, "/");

    // Create root directory (node 0)
    nodes[0].in_use = 1;
    nodes[0].node_id = 0;
    nodes[0].type = VFS_DIRECTORY;
    nodes[0].perms = VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC;
    nodes[0].parent = -1;
    nodes[0].child_count = 0;
    vfs_strcpy(nodes[0].name, "/");

    vfs_initialized = 1;

    // Create default directory structure
    vfs_mkdir("/home");
    vfs_mkdir("/home/user");
    vfs_mkdir("/home/user/Documents");
    vfs_mkdir("/home/user/Downloads");
    vfs_mkdir("/home/user/Pictures");
    vfs_mkdir("/home/user/Music");
    vfs_mkdir("/boot");
    vfs_mkdir("/dev");
    vfs_mkdir("/etc");
    vfs_mkdir("/tmp");
    vfs_mkdir("/usr");
    vfs_mkdir("/var");

    // Create some default files
    vfs_create("/home/user/readme.txt", VFS_FILE, VFS_PERM_READ | VFS_PERM_WRITE);
    {
        int fd = vfs_open("/home/user/readme.txt", VFS_O_WRONLY);
        if (fd >= 0) {
            const char* txt = "Welcome to Alteo OS v5.0!\nA minimal 64-bit operating system.\nBuilt from scratch in C and Assembly.\n";
            vfs_write(fd, txt, vfs_strlen(txt));
            vfs_close(fd);
        }
    }
    vfs_create("/home/user/notes.txt", VFS_FILE, VFS_PERM_READ | VFS_PERM_WRITE);
    {
        int fd = vfs_open("/home/user/notes.txt", VFS_O_WRONLY);
        if (fd >= 0) {
            const char* txt = "My Notes\n--------\nTODO: Build more apps\n";
            vfs_write(fd, txt, vfs_strlen(txt));
            vfs_close(fd);
        }
    }
    vfs_create("/etc/hostname", VFS_FILE, VFS_PERM_READ);
    {
        int fd = vfs_open("/etc/hostname", VFS_O_WRONLY);
        if (fd >= 0) { const char* t = "alteo"; vfs_write(fd, t, vfs_strlen(t)); vfs_close(fd); }
    }
    vfs_create("/etc/alteo.cfg", VFS_FILE, VFS_PERM_READ | VFS_PERM_WRITE);
    {
        int fd = vfs_open("/etc/alteo.cfg", VFS_O_WRONLY);
        if (fd >= 0) {
            const char* t = "resolution=1024x768\ntheme=nebula\nfont=8x16\n";
            vfs_write(fd, t, vfs_strlen(t)); vfs_close(fd);
        }
    }
    vfs_create("/home/user/hello.c", VFS_FILE, VFS_PERM_READ | VFS_PERM_WRITE);
    {
        int fd = vfs_open("/home/user/hello.c", VFS_O_WRONLY);
        if (fd >= 0) {
            const char* t = "// Hello World for Alteo OS\nvoid main() {\n    print(\"Hello, Alteo!\");\n}\n";
            vfs_write(fd, t, vfs_strlen(t)); vfs_close(fd);
        }
    }

    vfs_chdir("/home/user");
}

int vfs_open(const char* path, int flags) {
    if (!vfs_initialized) return -1;

    int nid = resolve_path(path);

    // If not found and O_CREAT, create it
    if (nid < 0 && (flags & VFS_O_CREAT)) {
        vfs_create(path, VFS_FILE, VFS_PERM_READ | VFS_PERM_WRITE);
        nid = resolve_path(path);
    }
    if (nid < 0) return -1;
    if (nodes[nid].type == VFS_DIRECTORY) return -1; // can't open dir as file

    int fd = alloc_fd();
    if (fd < 0) return -1;

    fds[fd].in_use = 1;
    fds[fd].node_id = nid;
    fds[fd].flags = flags;
    fds[fd].offset = 0;

    // Truncate if requested
    if (flags & VFS_O_TRUNC) {
        nodes[nid].size = 0;
        vfs_memset(nodes[nid].data, 0, VFS_MAX_DATA);
    }

    // Append mode: start at end
    if (flags & VFS_O_APPEND) {
        fds[fd].offset = nodes[nid].size;
    }

    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fds[fd].in_use) return -1;
    fds[fd].in_use = 0;
    return 0;
}

int vfs_read(int fd, void* buf, uint32_t count) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fds[fd].in_use) return -1;
    vfs_node_t* node = &nodes[fds[fd].node_id];
    if (!node->in_use) return -1;

    uint32_t available = node->size - fds[fd].offset;
    if (count > available) count = available;
    if (count == 0) return 0;

    vfs_memcpy(buf, node->data + fds[fd].offset, count);
    fds[fd].offset += count;
    return (int)count;
}

int vfs_write(int fd, const void* buf, uint32_t count) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fds[fd].in_use) return -1;
    vfs_node_t* node = &nodes[fds[fd].node_id];
    if (!node->in_use) return -1;

    uint32_t space = VFS_MAX_DATA - fds[fd].offset;
    if (count > space) count = space;
    if (count == 0) return 0;

    vfs_memcpy(node->data + fds[fd].offset, buf, count);
    fds[fd].offset += count;
    if (fds[fd].offset > node->size) {
        node->size = fds[fd].offset;
    }
    return (int)count;
}

int vfs_seek(int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fds[fd].in_use) return -1;
    vfs_node_t* node = &nodes[fds[fd].node_id];
    int32_t new_off = 0;
    switch (whence) {
        case VFS_SEEK_SET: new_off = offset; break;
        case VFS_SEEK_CUR: new_off = (int32_t)fds[fd].offset + offset; break;
        case VFS_SEEK_END: new_off = (int32_t)node->size + offset; break;
        default: return -1;
    }
    if (new_off < 0) new_off = 0;
    if (new_off > (int32_t)VFS_MAX_DATA) new_off = VFS_MAX_DATA;
    fds[fd].offset = (uint32_t)new_off;
    return 0;
}

int vfs_tell(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fds[fd].in_use) return -1;
    return (int)fds[fd].offset;
}

int vfs_create(const char* path, uint8_t type, uint8_t perms) {
    if (!vfs_initialized) return -1;

    // Check if already exists
    if (resolve_path(path) >= 0) return -1;

    char last[VFS_MAX_NAME];
    int parent_id = resolve_parent(path, last);
    if (parent_id < 0) return -1;
    if (nodes[parent_id].type != VFS_DIRECTORY) return -1;
    if (nodes[parent_id].child_count >= VFS_MAX_CHILDREN) return -1;

    int nid = alloc_node();
    if (nid < 0) return -1;

    vfs_memset(&nodes[nid], 0, sizeof(vfs_node_t));
    nodes[nid].in_use = 1;
    nodes[nid].node_id = nid;
    nodes[nid].type = type;
    nodes[nid].perms = perms;
    nodes[nid].parent = parent_id;
    nodes[nid].child_count = 0;
    vfs_strncpy(nodes[nid].name, last, VFS_MAX_NAME);

    // Add to parent
    nodes[parent_id].children[nodes[parent_id].child_count++] = nid;

    return nid;
}

int vfs_delete(const char* path) {
    int nid = resolve_path(path);
    if (nid <= 0) return -1; // can't delete root
    if (nodes[nid].type == VFS_DIRECTORY && nodes[nid].child_count > 0) return -1;

    // Remove from parent
    int pid = nodes[nid].parent;
    if (pid >= 0) {
        for (int i = 0; i < nodes[pid].child_count; i++) {
            if (nodes[pid].children[i] == nid) {
                // Shift remaining
                for (int j = i; j < nodes[pid].child_count - 1; j++)
                    nodes[pid].children[j] = nodes[pid].children[j + 1];
                nodes[pid].child_count--;
                break;
            }
        }
    }

    nodes[nid].in_use = 0;
    return 0;
}

int vfs_rename(const char* oldpath, const char* newpath) {
    int nid = resolve_path(oldpath);
    if (nid < 0) return -1;

    // Extract new name from newpath
    char newname[VFS_MAX_NAME];
    int new_parent = resolve_parent(newpath, newname);
    if (new_parent < 0) return -1;

    // Simple rename (same directory)
    vfs_strncpy(nodes[nid].name, newname, VFS_MAX_NAME);
    return 0;
}

int vfs_stat(const char* path, vfs_dirent_t* out) {
    int nid = resolve_path(path);
    if (nid < 0) return -1;

    vfs_strncpy(out->name, nodes[nid].name, VFS_MAX_NAME);
    out->type = nodes[nid].type;
    out->size = nodes[nid].size;
    out->perms = nodes[nid].perms;
    out->created = nodes[nid].created;
    out->modified = nodes[nid].modified;
    return 0;
}

int vfs_mkdir(const char* path) {
    return vfs_create(path, VFS_DIRECTORY, VFS_PERM_READ | VFS_PERM_WRITE | VFS_PERM_EXEC);
}

int vfs_rmdir(const char* path) {
    int nid = resolve_path(path);
    if (nid < 0) return -1;
    if (nodes[nid].type != VFS_DIRECTORY) return -1;
    if (nodes[nid].child_count > 0) return -1;
    return vfs_delete(path);
}

int vfs_readdir(const char* path, vfs_dirent_t* entries, int max_entries) {
    int nid = resolve_path(path);
    if (nid < 0) return -1;
    if (nodes[nid].type != VFS_DIRECTORY) return -1;

    int count = 0;
    for (int i = 0; i < nodes[nid].child_count && count < max_entries; i++) {
        int child = nodes[nid].children[i];
        if (child >= 0 && nodes[child].in_use) {
            vfs_strncpy(entries[count].name, nodes[child].name, VFS_MAX_NAME);
            entries[count].type = nodes[child].type;
            entries[count].size = nodes[child].size;
            entries[count].perms = nodes[child].perms;
            entries[count].created = nodes[child].created;
            entries[count].modified = nodes[child].modified;
            count++;
        }
    }
    return count;
}

int vfs_chdir(const char* path) {
    int nid = resolve_path(path);
    if (nid < 0) return -1;
    if (nodes[nid].type != VFS_DIRECTORY) return -1;

    // Build absolute path by walking up
    char parts[16][VFS_MAX_NAME];
    int depth = 0;
    int cur = nid;
    while (cur > 0 && depth < 16) {
        vfs_strcpy(parts[depth++], nodes[cur].name);
        cur = nodes[cur].parent;
    }

    cwd[0] = '/';
    cwd[1] = 0;
    for (int i = depth - 1; i >= 0; i--) {
        int l = vfs_strlen(cwd);
        if (l > 1) { cwd[l] = '/'; cwd[l + 1] = 0; }
        int cl = vfs_strlen(cwd);
        vfs_strcpy(cwd + cl, parts[i]);
    }

    return 0;
}

const char* vfs_getcwd(void) {
    return cwd;
}

int vfs_exists(const char* path) {
    return resolve_path(path) >= 0;
}

int vfs_is_dir(const char* path) {
    int nid = resolve_path(path);
    if (nid < 0) return 0;
    return nodes[nid].type == VFS_DIRECTORY;
}

vfs_node_t* vfs_resolve(const char* path) {
    int nid = resolve_path(path);
    if (nid < 0) return (vfs_node_t*)0;
    return &nodes[nid];
}

int vfs_get_node_count(void) {
    int count = 0;
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (nodes[i].in_use) count++;
    }
    return count;
}

// ---- Mount Support ----

// String helper for mount checking
static int vfs_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    // Check that str continues with '/' or ends (exact match)
    return (*str == '/' || *str == '\0');
}

int vfs_mount(const char* mount_point, const char* fs_type,
              vfs_fs_ops_t* ops, void* fs_data) {
    if (!mount_point || !fs_type || !ops) return -1;

    // Find a free mount slot
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1; // No free slots

    // Check if mount point already in use
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && vfs_strcmp(mounts[i].mount_point, mount_point) == 0) {
            return -1; // Already mounted
        }
    }

    // Ensure the mount point directory exists in the VFS
    if (!vfs_exists(mount_point)) {
        vfs_mkdir(mount_point);
    }

    // Set up the mount entry
    vfs_strncpy(mounts[slot].mount_point, mount_point, VFS_MAX_PATH);
    vfs_strncpy(mounts[slot].fs_type, fs_type, VFS_FS_NAME_MAX);
    mounts[slot].ops = ops;
    mounts[slot].fs_data = fs_data;
    mounts[slot].active = 1;

    return 0;
}

int vfs_umount(const char* mount_point) {
    if (!mount_point) return -1;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && vfs_strcmp(mounts[i].mount_point, mount_point) == 0) {
            mounts[i].active = 0;
            mounts[i].ops = (vfs_fs_ops_t*)0;
            mounts[i].fs_data = (void*)0;
            return 0;
        }
    }
    return -1; // Not found
}

int vfs_find_mount(const char* path) {
    if (!path) return -1;

    int best = -1;
    int best_len = 0;

    // Find the longest matching mount point (most specific mount)
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) continue;
        int mlen = vfs_strlen(mounts[i].mount_point);
        if (vfs_starts_with(path, mounts[i].mount_point)) {
            if (mlen > best_len) {
                best_len = mlen;
                best = i;
            }
        }
    }
    return best;
}

const vfs_mount_t* vfs_get_mounts(void) {
    return mounts;
}

