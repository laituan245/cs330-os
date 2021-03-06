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
#include "filesys/file.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
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

struct file * get_file (int fd) {
  struct file * myfile = NULL;
  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
      myfile = tmp_info -> file_ptr;
      break;
    }
  }
  return myfile;
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
    struct file * myfile = get_file(fd);
    sema_up(&filesys_sema);
    if (myfile == NULL)
      terminate_process();

    void * tmp_buffer = malloc(size);
    memcpy(tmp_buffer, buffer,size);
    int result = file_write (myfile, tmp_buffer, size);
    free(tmp_buffer);
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
    struct file * myfile = get_file(fd);
    sema_up(&filesys_sema);
    if (myfile == NULL)
      return -1;
    void * tmp_buffer = malloc(size);
    int result = file_read (myfile, tmp_buffer, size);
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
  unsigned result;
  sema_down(&filesys_sema);
  struct file * myfile = get_file(fd);
  sema_up(&filesys_sema);
  file_seek(myfile, position);
}

unsigned tell(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd  = * (int *) (esp + 4);
  sema_down(&filesys_sema);
  struct file * myfile = get_file(fd);
  sema_up(&filesys_sema);
  return file_tell(myfile);
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

  char * path  = * (char * *) (esp + 4);

  if (!is_valid(path))
    terminate_process();
  
  unsigned initial_size = * (unsigned *) (esp + 8);
  sema_down(&filesys_sema);
  disk_sector_t sector;
  bool result = traverse_path(path, &sector, NULL, 1, &initial_size);
  sema_up(&filesys_sema);

  return result;
}

bool remove(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * path  = * (char * *) (esp + 4);

  if (!is_valid(path))
    terminate_process();

  sema_down(&filesys_sema);
  char * name;
  disk_sector_t sector;
  if (!traverse_path(path, &sector, &name, 0, NULL)) {
    sema_up(&filesys_sema);
    return false;
  }
  bool result = false;
  struct inode * cur_inode = NULL, * parent_inode;
  struct dir * cur_dir, * parent_dir;
  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
     struct open_info * tmp_info = list_entry(e, struct open_info, elem);
     if (tmp_info->file_ptr != NULL && file_get_inode(tmp_info->file_ptr)->sector == sector) {
       cur_inode = inode_reopen(file_get_inode(tmp_info->file_ptr));
       break;
     }
     if (tmp_info->dir_ptr != NULL && dir_get_inode(tmp_info->dir_ptr)->sector == sector) {
       cur_inode = inode_reopen(dir_get_inode(tmp_info->dir_ptr));
       break;
     }
  }
  if (cur_inode == NULL)
    cur_inode = inode_open(sector);
  parent_inode = inode_open(cur_inode->data.parent);
  parent_dir = dir_open(parent_inode);

  if (cur_inode->data.is_dir) {
    cur_dir = dir_open(cur_inode);
    char tmp_buffer[20];
    if (dir_readdir (cur_dir, tmp_buffer)) {
      result = false;
      dir_close(cur_dir);
    }
    else {
      result = dir_remove (parent_dir, name, false);
      inode_remove(cur_inode);
      dir_close(cur_dir);
    }
  }
  else {
    result = dir_remove (parent_dir, name, false);
    if (result)
      inode_remove(cur_inode);
    inode_close(cur_inode);
  }

  dir_close(parent_dir);
  palloc_free_page(name);
  sema_up(&filesys_sema);

  return result;
}

int open(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * path  = * (char * *) (esp + 4);

  if (!is_valid(path))
    terminate_process();

  sema_down(&filesys_sema);
  disk_sector_t sector;
  char * name;
  if (!traverse_path(path, &sector, &name, 0, NULL)) {
    sema_up(&filesys_sema);
    return -1;
  }

  struct open_info *  new_info = malloc(sizeof (struct open_info));
  if (new_info == NULL) {
    sema_up(&filesys_sema);
    palloc_free_page(name);
    return -1;
  }

  int new_fd = allocate_fd();
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
  if (new_info->file_ptr != NULL) {
    if (strcmp(name, thread_current()->name) == 0)
      file_deny_write(new_info->file_ptr);
  }
  palloc_free_page(name);
  sema_up(&filesys_sema);
  return new_fd;
}

bool isdir(void * esp) {
  int argc = 1;
  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);

  sema_down(&filesys_sema);
  bool result;
  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
      if (tmp_info->file_ptr != NULL)
        result = false;
      else
        result = true;
    }
  }
  sema_up(&filesys_sema);

  return result;
}

int inumber(void * esp) {
  int argc = 1;
  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);

  sema_down(&filesys_sema);
  int result;
  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid) {
      if (tmp_info->dir_ptr)
        result = dir_get_inode(tmp_info->dir_ptr)->sector;
      else
        result = file_get_inode(tmp_info->file_ptr)->sector;
    }
  }
  sema_up(&filesys_sema);

  return result;
}

bool readdir(void * esp) {
  int argc = 2;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  int fd = * (int *) (esp + 4);
  char * name = * (char **) (esp + 8);

  if (!is_valid(name))
    terminate_process();

  bool result = false;
  sema_down(&filesys_sema);
  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    if (tmp_info->fd == fd && tmp_info->tid == thread_current()->tid && tmp_info->dir_ptr != NULL) {
      result = dir_readdir(tmp_info->dir_ptr, name);
      break;
    }
  }
  sema_up(&filesys_sema);
  return result;
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
   disk_sector_t sector;
   bool rs = traverse_path(dir_copy, sector, NULL, 2, NULL);
   sema_up(&filesys_sema);
   palloc_free_page(dir_copy);
   return rs;
}

bool chdir(void * esp) {
  int argc = 1;

  if (!are_args_locations_valid(esp, argc))
    terminate_process();

  char * dir  = * (char * *) (esp + 4);

  if (!is_valid(dir))
    terminate_process();

  sema_down(&filesys_sema);
  disk_sector_t sector;
  if (!traverse_path(dir, &sector, NULL, 0, NULL)) {
    sema_up(&filesys_sema);
    return false;
  }
  bool rs = false;
  struct list_elem * e;
  for (e = list_begin(&open_info_list); e != list_end(&open_info_list); e = list_next(e)) {
    struct open_info * tmp_info = list_entry(e, struct open_info, elem);
    if (tmp_info->dir_ptr != NULL && dir_get_inode(tmp_info->dir_ptr)->sector == sector) {
      thread_current()->cur_dir = dir_reopen(tmp_info->dir_ptr);
      rs = true;
      break;
    }
  }
  if (!rs) {
    struct inode * inode = inode_open(sector);
    thread_current()->cur_dir = dir_open(inode);
    rs = true;
  }
  sema_up(&filesys_sema);
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
      f->eax = chdir(f->esp);
      break;
    case SYS_MKDIR:                  /* Create a directory. */
      f->eax = mkdir(f->esp);
      break;
    case SYS_READDIR:                /* Reads a directory entry. */
      f->eax = readdir(f->esp);
      break;
    case SYS_ISDIR:                  /* Tests if a fd represents a directory. */
      f->eax = isdir(f->esp);
      break;
    case SYS_INUMBER:                /* Returns the inode number for a fd. */
      f->eax = inumber(f->esp);
      break;
    default:
      terminate_process();
      break;
  }
}
