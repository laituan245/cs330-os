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
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "list.h"

struct open_info {
  int fd;
  tid_t tid;
  struct file * file_ptr;
  struct dir * dir_ptr;
  struct list_elem elem;
};

static struct list open_info_list;

struct lock fd_lock;

struct semaphore filesys_sema;

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  lock_init(&fd_lock);
  list_init(&open_info_list);
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
  struct open_info * tmp_info;
  bool freedsth;
  while(true) {
    freedsth = false;
    for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
      tmp_info = list_entry(e, struct open_info, elem);
      if (tmp_info->tid == thread_current()->tid) {
        list_remove(&tmp_info->elem);
        file_close(tmp_info->file_ptr);
        dir_close(tmp_info->dir_ptr);
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
 
  if (fd == 0)
    terminate_process();
  else if (fd == 1) {
    putbuf(buffer, size);
    return size;
  }
  else  {
    sema_down(&filesys_sema);
    struct file * myfile = NULL;
    struct list_elem * e;
    for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
      struct open_info * tmp_info = list_entry(e, struct open_info, elem);
      if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
        myfile = tmp_info -> file_ptr;
        break;
      }
    }
    if (myfile == NULL) {
      sema_up(&filesys_sema);
      terminate_process();
    }
    
    void * tmp_buffer = malloc(size);
    memcpy(tmp_buffer, buffer,size);
    int result = file_write (myfile, tmp_buffer, size);
    free(tmp_buffer);
    sema_up(&filesys_sema);
    return result;
  }
}

int read (void * esp) {
  int argc = 3;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);
  void * buffer = * (void **) (esp + 8);
  unsigned size = * (unsigned *) (esp + 12);

  if (!is_valid(buffer) || !is_valid(buffer + size))
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
    for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
      struct open_info * tmp_info = list_entry(e, struct open_info, elem);
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
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
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
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
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
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
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

int filesize (void * esp) {
  int argc = 1;
  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);
  sema_down(&filesys_sema);
  struct file * myfile = NULL;
  struct list_elem * e;
  
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    
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
  struct open_info * tmp_info;
  bool freedsth;
  while(true) {
    freedsth = false;
    for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
      tmp_info = list_entry(e, struct open_info, elem);
      if (tmp_info->tid == thread_current()->tid) {
        list_remove(&tmp_info->elem);
        file_close(tmp_info->file_ptr);
        dir_close(tmp_info->dir_ptr);
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

  if (!is_valid(file))
    terminate_process();

  char * file_copy  = palloc_get_page (0);
  if (file_copy == NULL)
    return false;
  strlcpy (file_copy, file, PGSIZE);
  
  unsigned initial_size = * (unsigned *) (esp + 8);
  sema_down(&filesys_sema);
  bool result = traverse_path(file_copy, 0, &initial_size, NULL);
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
  disk_sector_t sector;
  if (!traverse_path(file_copy, 2, NULL, &sector)) {
    sema_up(&filesys_sema);
    return -1;
  }
  int new_fd = allocate_fd();
  struct open_info *  new_info = malloc(sizeof (struct open_info));
  if (new_info == NULL) {
    sema_up(&filesys_sema);
    return -1;
  }
  new_info->fd = new_fd;
  new_info->file_ptr = NULL;
  new_info->dir_ptr = NULL;
  new_info->tid = thread_current()->tid;
  bool reopened = false;

  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    if (tmp_info->file_ptr != NULL && file_get_inode(tmp_info->file_ptr)->sector == sector) {
      new_info->file_ptr = file_reopen(tmp_info->file_ptr);
      reopened = true;
      break;
    }
    if (tmp_info->dir_ptr != NULL && dir_get_inode(tmp_info->dir_ptr)->sector == sector) {
      new_info->dir_ptr = dir_reopen(tmp_info->dir_ptr);
      reopened = true;
      break;
    }
  }

  if (!reopened) {
    struct inode * inode = inode_open(sector);
    if (inode->data.is_dir)
      new_info->dir_ptr = dir_open(inode);
    else
      new_info->file_ptr = file_open(inode);
  }
  list_push_back(&open_info_list, &new_info->elem);

  sema_up(&filesys_sema);
  if (new_info->file_ptr != NULL) {
    if (strcmp(file_copy, thread_current()->name) == 0)
      file_deny_write(new_info->file_ptr);
  }
  palloc_free_page(file_copy);
  return new_fd;
}

bool mkdir(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * dir  = * (char * *) (esp + 4);

  if (!is_valid(dir))
    terminate_process();

  char * dir_copy  = palloc_get_page (0);
  if (dir_copy == NULL)
    return -1;
  strlcpy (dir_copy, dir, PGSIZE);
  sema_down(&filesys_sema);
  bool rs = traverse_path(dir_copy, 1, NULL, NULL);
  sema_up(&filesys_sema);
  palloc_free_page(dir_copy);
  return rs;
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
    case SYS_CHDIR:                  /* Change the current directory. */
      break;
    case SYS_MKDIR:                  /* Create a directory. */
      f->eax = mkdir(f->esp);
      break;
    case SYS_READDIR:                /* Reads a directory entry. */
      break;
    case SYS_ISDIR:                  /* Tests if a fd represents a directory. */
      break;
    case SYS_INUMBER:                /* Returns the inode number for a fd. */
      break;
    default:
      terminate_process();
      break;
  }
}
