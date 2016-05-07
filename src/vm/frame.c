#include "vm/frame.h"
#include "threads/pte.h"

static struct list frames_list;
static struct list_elem * cur;
static struct semaphore sema;

void frame_table_init() {
  sema_init(&sema,1);
  list_init(&frames_list);
  cur = NULL;
}

struct frame * allocate_frame(struct page * p, enum palloc_flags flags){
  ASSERT(flags & PAL_USER);
  int j;
  sema_down(&sema);
  struct frame * f = malloc(36);
  f->base = palloc_get_page(flags);
  if (f->base == NULL) {
    // Need to evict some page from its frame
    uint32_t *pd = thread_current()->pagedir;
    while (true) {
      struct frame * cur_frame = list_entry(cur, struct frame, elem);
      if (cur == list_end(&frames_list))
        cur = list_begin(&frames_list);
      else
        cur = list_next(cur);
      if (!cur_frame->pinned) {
        uint32_t * pte = lookup_page(pd, cur_frame->page->base ,false);
        if (*pte & PTE_A)
          *pte = *pte & ~PTE_A;
        else {
          // For project 3-1, whether the dirty bit is set or not
          // We will still write the page to some swap slot
          free(f);
          f = cur_frame;
          struct swap * s = allocate_swap();
          struct disk * swap_disk = disk_get(1, 1);
          *pte = *pte & ~PTE_P;
          *pte = *pte & ~PTE_D;
          for (j = 0; j < SECTORS_PER_SWAP; j++)
            disk_write(swap_disk, s->base + j, f->base + j * DISK_SECTOR_SIZE);
          f->page->frame = NULL;
          f->page->swap = s;
          f->page = p;
          f->pinned = true;
          palloc_free_page(f->base);
          f->base = palloc_get_page(flags);
        }
      }
    }
  }
  else {
    f->page = p;
    f->pinned = true;
    list_push_front(&frames_list, &f->elem);
    if (cur == NULL)
      cur = list_begin(&frames_list);
  }
  sema_up(&sema);
  return f;
}

void free_frame(struct frame * f) {
  ASSERT(f != NULL);
  sema_down(&sema);
  if (!f->pinned) {
    struct frame * cur_frame = list_entry(cur, struct frame, elem);
    if (cur_frame == f) {
      if (list_size(&frames_list) == 1)
        cur = NULL;
      else {
        if (cur == list_end(&frames_list))
          cur = list_begin(&frames_list);
        else
          cur = list_next(cur);
      }
    }
    list_remove(&f->elem);
    palloc_free_page(f->base);
    free(f);
  }
  sema_up(&sema);
}
