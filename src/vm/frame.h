#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include <stdint.h>
#include <list.h>
#include <hash.h>

struct lock frame_lock;

struct list frame_table;

struct frame_entry {
  void *frame;
  struct thread *thread;
  uint32_t *pte;
  struct list_elem elem;
};

void frame_table_init (void);
void* frame_alloc (enum palloc_flags flags);
void frame_free (void *frame);

#endif /* vm/frame.h */