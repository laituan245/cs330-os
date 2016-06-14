#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Disk used for file system. */
extern struct disk *filesys_disk;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (struct dir * dir, const char *name, off_t initial_size, int is_dir);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);
bool traverse_path(char * path, disk_sector_t * sector);

#endif /* filesys/filesys.h */
