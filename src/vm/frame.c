#include "vm/frame.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/pte.h"

static struct list frames_list;
static struct list_elem * cur;
static struct semaphore sema;

void frame_table_init() {
  sema_init(&sema,1);
  list_init(&frames_list);
  cur = NULL;
}

void move_cur_ptr(){
  if (cur == list_end(&frames_list))
        cur = list_begin(&frames_list);
      else
        cur = list_next(cur);
}

struct frame * allocate_frame(struct page * p, enum palloc_flags flags){
  ASSERT(flags & PAL_USER);
  int j;
  sema_down(&sema);
  struct frame * f = malloc(sizeof (struct frame));
  f->base = palloc_get_page(flags);
  if (f->base == NULL) {
    // Need to evict some page from its frame
    free(f);
    while (true) {
      f = list_entry(cur, struct frame, elem);
      move_cur_ptr();
      if (!f->pinned) {
        struct thread *t = f->thread;
        if (pagedir_is_accessed(t->pagedir, f->page->base))
	         pagedir_set_accessed(t->pagedir, f->page->base, false);
	else {
          sema_down(&f->page->page_sema);
          if (f->page->from_executable && !pagedir_is_dirty(t->pagedir, f->page->base)) {
            pagedir_clear_page(t->pagedir, f->page->base);
            f->page->swap = NULL;
            f->page->frame = NULL;
            f->page->loc = EXECUTABLE;
          }
          else if (f->page->is_mmapped && pagedir_is_dirty(t->pagedir, f->page->base)) {
            pagedir_set_dirty(t->pagedir, f->page->base, false);       
            pagedir_clear_page(t->pagedir, f->page->base);
            lock_acquire(get_filesys_lock());
            struct file * file = f->page->mmappedfile;
            off_t offset = f->page->mmapped_ofs;
            size_t page_read_bytes = PGSIZE;
            if (page_read_bytes < file_length(file) - offset)
              page_read_bytes = file_length(file) - offset;
            if (page_read_bytes != 0) {
              file_seek(file, offset);
              file_read(file, f->base, page_read_bytes);
            }
            lock_release(get_filesys_lock()); 
	    f->page->swap = NULL;
            f->page->frame = NULL;
            f->page->loc = MMAP;
          }
          else
            swap_out(f->page);
          sema_up(&f->page->page_sema);
          f->page = p;
          p->frame = f;
          p->swap = NULL;
          f->thread = thread_current();
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
  p->loc = MEMORY;
  sema_up(&sema);
  return f;
}

void free_frame(struct frame * f) {
  ASSERT(f != NULL);
  sema_down(&sema);
  if (f->thread == thread_current()) {
    struct frame * cur_frame = list_entry(cur, struct frame, elem);
    if (cur_frame == f) {
      if (list_size(&frames_list) == 1)
        cur = NULL;
      else
        move_cur_ptr();
    }
    list_remove(&f->elem);
    palloc_free_page(f->base);
    free(f);
  }
  sema_up(&sema);
}
