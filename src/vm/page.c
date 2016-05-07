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
  struct hash * pages = malloc(24);
  hash_init(pages, page_hash, page_less, NULL);
  return pages;
}

struct page * new_page(void * base) {
  struct hash * pages = thread_current()->pt;
  struct page * p = malloc(28);
  p->pid = thread_current()->tid;
  p->base = pg_round_down(base);
  p->swap = NULL;
  p->frame = NULL;
  hash_insert(pages, &p->hash_elem);
  return p;
}

void free_page(struct page * p) {
  ASSERT(p != NULL);

  struct hash * pages = thread_current()->pt;
  if (p->swap != NULL)
    free_swap(p->swap);
  if (p->frame != NULL)
    free_frame(p->frame);
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
