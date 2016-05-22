#include "vm/page.h"
#include "vm/frame.h"
#include "userprog/syscall.h"
#include "filesys/file.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include <debug.h>

unsigned page_hash(const struct hash_elem *p_, void *aux UNUSED) {
  const struct page * p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p -> base, sizeof p->base);
}

bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
  const struct page * a = hash_entry (a_, struct page, hash_elem);
  const struct page * b = hash_entry (b_, struct page, hash_elem);
  return a -> base < b -> base;
}

struct hash * new_pt() {
  struct hash * pages = malloc(sizeof (struct hash));
  hash_init(pages, page_hash, page_less, NULL);
  return pages;
}

struct page * new_page(void * base) {
  enum intr_level old_level = intr_disable();
  struct hash * pages = thread_current()->pt;
  struct page * p = malloc(sizeof (struct page));
  p->pid = thread_current()->tid;
  p->base = pg_round_down(base);
  p->swap = NULL;
  p->frame = NULL;
  p->from_executable = false;
  p->is_mmapped = false;
  p->loc = NONE;
  sema_init(&p->page_sema, 0);
  hash_insert(pages, &p->hash_elem);
  intr_set_level(old_level);
  return p;
}

void free_page(struct hash * pages, struct page * p) {
  ASSERT(p != NULL);
  sema_down(&p->page_sema);
  if (p->loc == SWAP)
    free_swap(p->swap);
  if (p->loc == MEMORY)
    free_frame(p->frame);
  sema_up(&p->page_sema);
  hash_delete(pages, &p->hash_elem);
  free(p);
}

struct page * find_page(void * base) {
  struct page p;
  struct hash_elem *e;
  struct hash * pages = thread_current()->pt;

  p.base = base;
  e = hash_find(pages, &p.hash_elem);
  return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

void stack_growth(void * addr) {
   struct page * p = new_page(pg_round_down(addr));
   sema_down(get_frame_table_sema());
   struct frame * f = allocate_frame(p, PAL_USER | PAL_ZERO);
   sema_up(get_frame_table_sema());
   install_page(p->base, p->frame->base, true);
   p->loc = MEMORY;
   sema_up(&p->page_sema);
}

void load_page(struct page * p) {
  ASSERT(p->page_sema.value == 0);
  if (p->loc == SWAP)
    swap_in(p);
  else if (p->loc == EXECUTABLE) {
    lock_acquire(get_filesys_lock());
    sema_down(get_frame_table_sema());
    struct frame  * frame = allocate_frame(p, PAL_USER | PAL_ZERO);
    sema_up(get_frame_table_sema());
    uint8_t *kpage = frame->base;

    if (p->page_read_bytes != 0)
      file_read_at (thread_current()->executable, kpage, p->page_read_bytes, p->ofs);

    install_page (p->base, kpage, p->writable);
    p->loc = MEMORY;
    lock_release(get_filesys_lock());
  }
  else if (p->loc == MMAP) {
    lock_acquire(get_filesys_lock());
    sema_down(get_frame_table_sema());
    struct frame  * frame = allocate_frame(p, PAL_USER | PAL_ZERO);
    sema_up(get_frame_table_sema());
    uint8_t *kpage = frame->base;
    struct file * file = p->mmappedfile;
    off_t offset = p->mmapped_ofs;
    size_t page_read_bytes = PGSIZE;
    if (page_read_bytes > file_length(file) - offset)
      page_read_bytes = file_length(file) - offset;
    if (page_read_bytes != 0)
      file_read_at (file, kpage, page_read_bytes, offset);

    install_page (p->base, kpage, p->writable);
    p->loc = MEMORY;
    lock_release(get_filesys_lock());
  }
}

void remove_mapping (struct page * p) {
  ASSERT (p->is_mmapped);
  ASSERT (p->page_sema.value == 0);
  if (p->loc == MEMORY) {
    if (pagedir_is_dirty(thread_current()->pagedir, p->base)) {
      void * tmp_addr = p->base;
      lock_acquire(get_filesys_lock());
      struct file * file = p->mmappedfile;
      off_t offset = p->mmapped_ofs;
      size_t page_read_bytes = PGSIZE;
      if (page_read_bytes < file_length(file) - offset)
        page_read_bytes = file_length(file) - offset;
      if (page_read_bytes != 0)
        file_write_at(file, p->frame->base, page_read_bytes, offset);
      lock_release(get_filesys_lock());
    }
  }
  p->swap = NULL;
  p->frame = NULL;
  p->loc = MMAP;
}
