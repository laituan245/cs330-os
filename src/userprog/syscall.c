#include "userprog/process.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "list.h"

struct file_info {
  int fd;
  tid_t tid;
  struct file * file_ptr;
  struct list_elem elem;
};

static struct list file_info_list;

struct lock fd_lock;

struct semaphore filesys_sema;

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  lock_init(&fd_lock);
  list_init(&file_info_list);
  sema_init(&filesys_sema, 1);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static int
allocate_fd (void)
{
  static int next_fd = 2;
  int fd;

  lock_acquire (&fd_lock);
  fd = next_fd++;
  lock_release (&fd_lock);

  return fd;
}


void terminate_process() {
  thread_current()->exit_status = -1;
  printf("%s: exit(%d)\n", thread_current()->name, -1);
  sema_down(&filesys_sema);
  struct list_elem * e;
  struct file_info * tmp_info;
  bool freedsth;
  while(true) {
    freedsth = false;
    for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
      tmp_info = list_entry(e, struct file_info, elem);
      if (tmp_info->tid == thread_current()->tid) {
        list_remove(&tmp_info->elem);
        file_close(tmp_info->file_ptr);   
        tmp_info->file_ptr = NULL;
        free(tmp_info);
        freedsth = true;
        break;
     }
    }
    if (!freedsth)
      break;
  }
 
  sema_up(&filesys_sema);
  thread_exit();
}

bool is_valid (void * esp, void * pointer) {
  if (pointer == NULL)
    return false;
  if (!is_user_vaddr(pointer))
    return false;
  struct page * p = find_page( pg_round_down(pointer));
  if (p == NULL) {
    uintptr_t a = (uintptr_t) esp;
    uintptr_t b = (uintptr_t) pointer;
    uintptr_t c = (uintptr_t) thread_current()->data_segment_end;
    if (b >= a && b >= c && * (uint32_t *) esp == SYS_READ) {
      // Seem like a stack access
      // Let our page fault handler takes care of it
      return true;
    }
    return false;
  }
  return true;
}

bool are_args_locations_valid (void * esp, int argc) {
  void * tmpptr = esp;
  int i;
  for (i = 0; i < argc; i++) {
    tmpptr = tmpptr + 4;
    if (!is_valid(esp, tmpptr))
      return false;
  }
  return true;
}

int write (void * esp) {
  int argc = 3;
  
  if (!are_args_locations_valid(esp, argc))
    terminate_process(); 

  int fd = * (int *) (esp + 4);
  void * buffer = * (void **) (esp + 8);
  unsigned size = * (unsigned *) (esp + 12);
 
  int i;
  for (i = 0; i < size; i++)
    if (!is_valid(esp, buffer + i))
      terminate_process();
 
  if (fd == 0)
    terminate_process();

  int result;
  void * tmp_buffer = malloc(size);
  memcpy(tmp_buffer, buffer,size);

  if (fd == 1) {
    putbuf(tmp_buffer, size);
    result = size;
  }
  else  {
    sema_down(&filesys_sema);
    struct file * myfile = NULL;
    struct list_elem * e;
    for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
      struct file_info * tmp_info = list_entry(e, struct file_info, elem);
      if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
        myfile = tmp_info -> file_ptr;
        break;
      }
    }
    if (myfile == NULL) {
      sema_up(&filesys_sema);
      terminate_process();
    }
    result = file_write (myfile, tmp_buffer, size);
    sema_up(&filesys_sema);
  }
  free(tmp_buffer);
  return result;
}

int read (void * esp) {
  int argc = 3;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);
  void * buffer = * (void **) (esp + 8);
  unsigned size = * (unsigned *) (esp + 12);

  int i;
  for (i = 0; i < size; i++)
    if (!is_valid(esp, buffer + i))
      terminate_process();

  if (fd == 0) {
    return 0;
  }
  else if (fd == 1)
    return 0;
  else {
    sema_down(&filesys_sema);
    struct file * myfile = NULL;
    struct list_elem * e;
    for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
      struct file_info * tmp_info = list_entry(e, struct file_info, elem);
      if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) { 
        myfile = tmp_info -> file_ptr;
        break;
      } 
    }
    if (myfile == NULL) {
      sema_up(&filesys_sema);
      return -1;
    }
    void * tmp_buffer = malloc(size);
    int result = file_read (myfile, tmp_buffer, size);
    sema_up(&filesys_sema);
    memcpy(buffer, tmp_buffer,size);
    free(tmp_buffer);
    return result;
  }
}

void seek(void * esp) {
  int argc = 2;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd  = * (int *) (esp + 4);
  unsigned position = * (unsigned *) (esp + 8);
  sema_down(&filesys_sema);
  unsigned result;
  struct file * myfile = NULL;
  struct list_elem * e;
  for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
    struct file_info * tmp_info = list_entry(e, struct file_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
      myfile = tmp_info -> file_ptr;
      break;
    }
  }
  file_seek(myfile, position);
  sema_up(&filesys_sema);
}

unsigned tell(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd  = * (int *) (esp + 4);
  sema_down(&filesys_sema);
  unsigned result;
  struct file * myfile = NULL;
  struct list_elem * e;
  for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
    struct file_info * tmp_info = list_entry(e, struct file_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
      myfile = tmp_info -> file_ptr;
      break;
    }
  }
  result = file_tell(myfile);
  sema_up(&filesys_sema);
  return result;
}

void close(void * esp) {
  int argc = 1;
  
  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);
  sema_down(&filesys_sema);
  struct file * myfile = NULL;
  struct list_elem * e;
  for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
    struct file_info * tmp_info = list_entry(e, struct file_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
      myfile = tmp_info -> file_ptr;
      list_remove(&tmp_info->elem);
      free(tmp_info);
      break;
    }
  }
  file_close(myfile);
  sema_up(&filesys_sema);
}

tid_t exec(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * cmd_line = * (char * *) (esp + 4);

  if (!is_valid(esp, cmd_line))
    terminate_process();
  
  char * argv_copy = palloc_get_page (0);
  if (argv_copy == NULL)
    return TID_ERROR;
  strlcpy (argv_copy, cmd_line, PGSIZE);

  tid_t result = process_execute(argv_copy);
  palloc_free_page(argv_copy);
  return result;
}

int filesize (void * esp) {
  int argc = 1;
  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);
  sema_down(&filesys_sema);
  struct file * myfile = NULL;
  struct list_elem * e;
  
  for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
    struct file_info * tmp_info = list_entry(e, struct file_info, elem);
    
    if (tmp_info->fd == fd) {
      myfile = tmp_info -> file_ptr;
      break;
    }
    
  }
  if (myfile == NULL) {
    sema_up(&filesys_sema);
    return -1;
  }
  int result = file_length(myfile);
  sema_up(&filesys_sema);
  return result;
}

void exit (void * esp) {
  int argc = 1;
 
 if (!are_args_locations_valid(esp, argc))
    terminate_process(); 

  int status = * (int *) (esp + 4);
  thread_current()->exit_status = status;
  printf("%s: exit(%d)\n", thread_current()->name, status);
  sema_down(&filesys_sema);  
  struct list_elem * e;
  struct file_info * tmp_info;
  bool freedsth;
  while(true) {
    freedsth = false;
    for (e = list_begin(&file_info_list); e != list_end(&file_info_list); e = list_next(e)) {
      tmp_info = list_entry(e, struct file_info, elem);
      if (tmp_info->tid == thread_current()->tid) {
        list_remove(&tmp_info->elem);
        file_close(tmp_info->file_ptr);
        tmp_info->file_ptr = NULL;
        free(tmp_info);
        freedsth = true;
        break;
     }
    }
    if (!freedsth)
      break;
  }

  sema_up(&filesys_sema);

  thread_exit();
}

int wait (void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();
  
  tid_t tid = * (tid_t *) (esp + 4);
  
  return process_wait(tid);
}

bool create(void * esp) {
  int argc = 2;
  
  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * file  = * (char * *) (esp + 4);

  if (!is_valid(esp, file))
    terminate_process();

  char * file_copy  = palloc_get_page (0);
  if (file_copy == NULL)
    return false;
  strlcpy (file_copy, file, PGSIZE);
  
  unsigned initial_size = * (unsigned *) (esp + 8);
  sema_down(&filesys_sema);
  bool result = filesys_create (file_copy, initial_size);
  sema_up(&filesys_sema);

  palloc_free_page(file_copy);
  return result;
}

bool remove(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * file  = * (char * *) (esp + 4);

  if (!is_valid(esp, file))
    terminate_process();

  char * file_copy  = palloc_get_page (0);
  if (file_copy == NULL)
    return false;
  strlcpy (file_copy, file, PGSIZE);

  sema_down(&filesys_sema);
  bool result = filesys_remove (file_copy);
  sema_up(&filesys_sema);

  palloc_free_page(file_copy);
  return result;
}

int open(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * file  = * (char * *) (esp + 4);

  if (!is_valid(esp, file))
    terminate_process();

  char * file_copy  = palloc_get_page (0);
  if (file_copy == NULL)
    return -1;
  strlcpy (file_copy, file, PGSIZE);

  sema_down(&filesys_sema);
  struct file * file_ptr = filesys_open (file_copy);
  if (file_ptr == NULL) {
    sema_up(&filesys_sema);
    return -1;
  }
  int new_fd = allocate_fd();
  struct file_info tmp_info;
  struct file_info *  new_info = malloc(sizeof (tmp_info));
  if (new_info == NULL) {
    file_close(file_ptr);
    sema_up(&filesys_sema);
    return -1;
  }
  new_info->fd = new_fd;
  new_info->file_ptr = file_ptr;
  new_info->tid = thread_current()->tid;
  list_push_back(&file_info_list, &new_info->elem);

  sema_up(&filesys_sema);
  if (strcmp(file_copy, thread_current()->name) == 0)
    file_deny_write(file_ptr);
  palloc_free_page(file_copy);
  return new_fd;
}



static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t syscall_nb;
  if (!is_valid(f->esp, f->esp))
    terminate_process();
  syscall_nb = * (uint32_t *) f->esp;
  switch(syscall_nb) {
    case SYS_HALT:                   /* Halt the operating system. */
      power_off();
      break;
    case SYS_EXIT:                   /* Terminate this process. */
      exit(f->esp);
      break;
    case SYS_EXEC:                   /* Start another process. */
      f->eax = exec(f->esp);
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      f->eax = wait(f->esp);
      break;
    case SYS_CREATE:                 /* Create a file. */
      f->eax = create(f->esp);
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      f->eax = remove(f->esp);
      break;
    case SYS_OPEN:                   /* Open a file. */
      f->eax = open(f->esp);
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      f->eax = filesize(f->esp);
      break;
    case SYS_READ:                   /* Read from a file. */
      f->eax = read(f->esp);
      break;
    case SYS_WRITE:                  /* Write to a file. */
      f->eax = write(f->esp);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      seek(f->esp);
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      f->eax = tell(f->esp);
      break;
    case SYS_CLOSE:                  /* Close a file. */
      close(f->esp);
      break;
    default:
      terminate_process();
      break;
  }
}
