#include "userprog/process.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "filesys/filesys.h"
#include "list.h"

struct file_info {
  int fd;
  struct file * file_ptr;
  struct list_elem elem;
};

struct lock fd_lock;

struct list file_info_list;

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
  thread_exit();
}

bool is_valid (void * pointer) {
  if (pointer == NULL)
    return false;
  if (!is_user_vaddr(pointer))
    return false;
  if (pagedir_get_page(thread_current()->pagedir, pointer) == NULL)
    return false;
  return true;
}

bool are_args_locations_valid (void * esp, int argc) {
  void * tmpptr = esp;
  int i;
  for (i = 0; i < argc; i++) {
    tmpptr = tmpptr + 4;
    if (!is_valid(tmpptr))
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
  
  if (!is_valid(buffer) || !is_valid(buffer + size))
    terminate_process();

  if (fd == 1) {
    putbuf(buffer, size);
    return size;
  }
}

tid_t exec(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * cmd_line = * (char * *) (esp + 4);

  if (!is_valid(cmd_line))
    terminate_process();
  
  char * argv_copy = palloc_get_page (0);
  if (argv_copy == NULL)
    return TID_ERROR;
  strlcpy (argv_copy, cmd_line, PGSIZE);

  tid_t result = process_execute(argv_copy);
  palloc_free_page(argv_copy);
  return result;
}

void exit (void * esp) {
  int argc = 1;
 
 if (!are_args_locations_valid(esp, argc))
    terminate_process(); 

  int status = * (int *) (esp + 4);
  thread_current()->exit_status = status;
  printf("%s: exit(%d)\n", thread_current()->name, status);
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

  if (!is_valid(file))
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

  if (!is_valid(file))
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

  if (!is_valid(file))
    terminate_process();

  char * file_copy  = palloc_get_page (0);
  if (file_copy == NULL)
    return -1;
  strlcpy (file_copy, file, PGSIZE);

  sema_down(&filesys_sema);
  struct file * file_ptr = filesys_open (file_copy);
  if (file_ptr == NULL)
    return -1;
  int new_fd = allocate_fd();
  struct file_info new_info;
  new_info.fd = new_fd;
  new_info.file_ptr = file_ptr;
  list_push_back(&file_info_list, &new_info.elem);  
  sema_up(&filesys_sema);

  palloc_free_page(file_copy);
  return new_fd;
}



static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t syscall_nb;
  if (!is_valid(f->esp))
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
      break;
    case SYS_READ:                   /* Read from a file. */
      break;
    case SYS_WRITE:                  /* Write to a file. */
      f->eax = write(f->esp);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      break;
    case SYS_CLOSE:                  /* Close a file. */
      break;
    default:
      terminate_process();
      break;
  }
}
