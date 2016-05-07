#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/synch.h"

struct frame {
  void * base;
  struct page * page;
  struct list_elem elem;
  bool pinned;
};

void frame_table_init();
struct frame * allocate_frame(struct page *, enum palloc_flags);
void free_frame(struct frame *);

#endif
