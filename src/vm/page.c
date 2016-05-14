#include "vm/page.h"
#include "threads/thread.h"
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
  struct hash * pages = thread_current()->pt;
  struct page * p = malloc(sizeof (struct page));
  p->pid = thread_current()->tid;
  p->base = pg_round_down(base);
  p->swap = NULL;
  p->frame = NULL;
  p->from_executable = false;
  p->loc = NONE;
  sema_init(&p->page_sema, 0);
  hash_insert(pages, &p->hash_elem);
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
   struct frame * f = allocate_frame(p, PAL_USER | PAL_ZERO);     
   install_page(p->base, p->frame->base, true);
   p->loc = MEMORY;
   sema_up(&p->page_sema);
}

void load_page(struct page * p) {
  ASSERT(p->page_sema.value == 0);
  if (p->loc == SWAP)
    swap_in(p);
  else if (p->loc == EXECUTABLE) {
    struct frame  * frame = allocate_frame(p, PAL_USER | PAL_ZERO);
    uint8_t *kpage = frame->base;
    struct file * file = thread_current()->executable;
    size_t page_read_bytes = p->page_read_bytes;
    if (page_read_bytes != 0) {
      file_seek (file, p->ofs);
      file_read (file, kpage, page_read_bytes);
    }
    install_page (p->base, kpage, p->writable);
    p->loc = MEMORY;
  }
}
