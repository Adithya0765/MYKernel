// procfs.h - Process Filesystem for Alteo OS
// Provides /proc pseudo-filesystem with system and process information
#ifndef PROCFS_H
#define PROCFS_H

#include "stdint.h"
#include "vfs.h"

// Initialize procfs (creates /proc entries and mounts)
void procfs_init(void);

// Get the VFS filesystem operations for procfs
vfs_fs_ops_t* procfs_get_ops(void);

// Refresh procfs entries (call periodically or on process changes)
void procfs_refresh(void);

#endif
