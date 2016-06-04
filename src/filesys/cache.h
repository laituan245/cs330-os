#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"
#include "threads/synch.h"
#include <list.h>

struct cached_sector {
  struct list_elem elem;
  disk_sector_t sector_idx;
  void * data;
  bool dirty;
  struct semaphore sema;
};

void buffer_cache_init();
bool write_sector(disk_sector_t, off_t, void *, off_t);
bool read_sector(disk_sector_t, off_t, void *, off_t);
#endif
