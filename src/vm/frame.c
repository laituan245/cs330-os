#include "vm/frame.h"
#include "userprog/pagedir.h"
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
  struct frame * f = malloc(sizeof (struct frame));
  f->base = palloc_get_page(flags);
  if (f->base == NULL) {
    // Need to evict some page from its frame
    while (true) {
      struct frame * cur_frame = list_entry(cur, struct frame, elem);
      if (cur == list_end(&frames_list))
	cur = list_begin(&frames_list);
      else
	cur = list_next(cur);
      if (!cur_frame->pinned) {
        struct thread *t = cur_frame->thread;
        if (pagedir_is_accessed(t->pagedir, cur_frame->page->base))
	         pagedir_set_accessed(t->pagedir, cur_frame->page->base, false);
	else {
          /* For project 3-1, whether the dirty bit is set or not, 
             we will still write the page to some swap slot */
          free(f);
          f = cur_frame;
          swap_out(f->page);
          f->page = p;
          p->frame = f;
          p->swap = NULL;
          //f->thread = thread_current();
          palloc_free_page(f->base);
          f->base = palloc_get_page(flags);
          break;
        }
      }
    }
  }
  else {
    f->page = p;
    p->frame = f;
    f->pinned = false;
    f->thread = thread_current();
    if (cur == NULL) {
      list_push_front(&frames_list, &f->elem);
      cur = list_begin(&frames_list);
    }
    else
      list_insert(cur,&f->elem);
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
