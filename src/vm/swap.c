#include "threads/synch.h"
#include "threads/malloc.h"
#include "vm/swap.h"
#include "vm/frame.h"
#include <bitmap.h>

static struct bitmap * used_map;
static struct lock lock;
static struct lock swap_disk_lock;

void swap_table_init() {
  struct disk * swap_disk = disk_get(1,1);
  disk_sector_t swap_disk_size = disk_size(swap_disk);
  used_map = bitmap_create(swap_disk_size / SECTORS_PER_SWAP);
  lock_init(&lock);
  lock_init(&swap_disk_lock);
}

struct swap * allocate_swap() {
  lock_acquire(&lock);
  struct swap * s = malloc(sizeof (struct swap));
  size_t idx = bitmap_scan_and_flip (used_map, 0, 1, false);
  if (idx == BITMAP_ERROR)
    PANIC("get_free_swap: out of swapping slots");
  s->base = SECTORS_PER_SWAP * idx;
  lock_release(&lock);
  return s;
}


/* Swap out the content of a page. Assume that the content of
   the page is currently stored in some frame */
void swap_out(struct page *p) {
  sema_down(&p->page_sema);
  int j;
  
  ASSERT(p->frame != NULL);
  struct frame * f = p->frame;
  struct thread *t = f->thread;
  pagedir_set_dirty(t->pagedir, p->base, false);
  pagedir_clear_page(t->pagedir, p->base);
  struct disk * sdisk = disk_get(1,1);
  struct swap * s = allocate_swap();
  
  lock_acquire(&swap_disk_lock);
  for (j = 0; j < SECTORS_PER_SWAP; j++)
    disk_write(sdisk, s->base + j, f->base + j * DISK_SECTOR_SIZE);
  lock_release(&swap_disk_lock);
  p->frame = NULL;
  p->swap = s;
  sema_up(&p->page_sema);
}


/* Swap in the content of a page. Assume that the content of 
   the page is currently stored in some swapping slot */
void swap_in(struct page *p) {
  sema_down(&p->page_sema);
  int j;
  ASSERT(p->swap != NULL);
  struct disk * sdisk = disk_get(1, 1);
  struct swap * s = p->swap;
  struct frame * f = allocate_frame(p, PAL_USER | PAL_ZERO);
  install_page(p->base, p->frame->base, true);
  lock_acquire(&swap_disk_lock);
  for (j = 0; j < SECTORS_PER_SWAP; j++)
    disk_read(sdisk, s->base + j, p->base + j * DISK_SECTOR_SIZE);
  lock_release(&swap_disk_lock);
  free_swap(s);
  install_page(p->base, p->frame->base, p->writable);
  sema_up(&p->page_sema);
}

void free_swap(struct swap * s) {
  lock_acquire(&lock);
  bitmap_set(used_map, s->base / SECTORS_PER_SWAP, false);
  free(s);
  lock_release(&lock);
}
