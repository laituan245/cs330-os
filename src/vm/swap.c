#include "threads/synch.h"
#include "threads/malloc.h"
#include "vm/swap.h"
#include <bitmap.h>

static struct bitmap * used_map;
static struct lock lock;

void swap_table_init() {
  struct disk * swap_disk = disk_get(1,1);
  disk_sector_t swap_disk_size = disk_size(swap_disk);
  used_map = bitmap_create(swap_disk_size / SECTORS_PER_SWAP);
  lock_init(&lock);
}

struct swap * allocate_swap() {
  lock_acquire(&lock);
  struct swap * s = malloc(4);
  size_t idx = bitmap_scan_and_flip (used_map, 0, 1, false);
  if (idx == BITMAP_ERROR)
    PANIC("get_free_swap: out of swapping slots");
  s->base = SECTORS_PER_SWAP * idx;
  lock_release(&lock);
  return s;
}

void free_swap(struct swap * s) {
  lock_acquire(&lock);
  bitmap_set(used_map, s->base / SECTORS_PER_SWAP, false);
  free(s);
  lock_release(&lock);
}
