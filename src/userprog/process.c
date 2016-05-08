#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/pte.h"
#include "vm/page.h"
#include "vm/frame.h"

struct exit_info {
  tid_t tid;
  int status;
  struct list_elem elem;
};

struct relationship_info {
  tid_t parent_tid;
  tid_t child_tid;
  struct list_elem elem;
  struct semaphore sema;
};

struct pack {
  char * argv;
  struct semaphore * sema;
  bool * loaded;
  tid_t parent_tid;
};

static struct list exit_info_list;
static struct list relationship_list;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

void process_init() {
  list_init(&exit_info_list);
  list_init(&relationship_list);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *argv) 
{
  struct pack my_pack;
  struct semaphore child_load_sema;
  bool loaded = false;
  char *argv_copy;
  tid_t tid;
  
  argv_copy = palloc_get_page (0);
  if (argv_copy == NULL)
    return TID_ERROR;
  strlcpy (argv_copy, argv, PGSIZE);

  char *save_ptr;
  char *file_name = strtok_r(argv, " ", &save_ptr);
  
  sema_init(&child_load_sema, 0);
  my_pack.sema = &child_load_sema;
  my_pack.argv = argv_copy;
  my_pack.loaded = &loaded;
  my_pack.parent_tid = thread_current()->tid;
  tid = thread_create (file_name, PRI_DEFAULT, start_process, &my_pack);
  if (tid == TID_ERROR)
    palloc_free_page (argv_copy);
  else {
   sema_down(&child_load_sema);
   if (!loaded)
     return -1;
  }
  return tid;
}

/* A thread function that loads a user process and makes it start
   running. */
static void
start_process (void * aux)
{
  struct pack * my_pack = (struct pack *) aux;
  void * argv = my_pack->argv;
  struct semaphore * sema = my_pack->sema;
  bool * loaded = my_pack->loaded;

  int argc, i;
  char *save_ptr, *token;
  char *file_name = strtok_r(argv, " ", &save_ptr);
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  if (!success) {
    palloc_free_page(argv);
    sema_up(sema);
    exit(-1);
  }

  /* Put the arguments on the stack */
  if_.esp -= (strlen(file_name)+1);
  strlcpy (if_.esp, file_name ,strlen(file_name) + 1);
  argc = 1;
  for (token = strtok_r (NULL, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr)) {
    if_.esp -= (strlen(token) + 1);
    strlcpy (if_.esp, token ,strlen(token) + 1);
    argc ++;
  }
  char * tmp_ptr = if_.esp;
  if_.esp -= (((uintptr_t) if_.esp) % 4);
  if_.esp -= 4;
  for (i = 0; i < argc; i++) {
   * (--(char **) if_.esp) = tmp_ptr;
   tmp_ptr += (strlen(tmp_ptr) + 1);
  }
  char ** tmp_ptr2 = if_.esp;
  * (--(char **) if_.esp) = tmp_ptr2;
  * (--(int *) if_.esp) = argc;
  if_.esp -= 4;
 
  palloc_free_page(argv);

  struct relationship_info * new_info = malloc(100);
  new_info->parent_tid = my_pack->parent_tid;
  new_info->child_tid = thread_current()->tid;
  sema_init(&new_info->sema,0);
  list_push_back(&relationship_list, &new_info->elem);

  sema_up(sema);
  *loaded = true;
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* This is 2016 spring cs330 skeleton code */

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  struct list_elem * e;
  struct relationship_info * rls_info;
  struct thread * curr = thread_current();
  bool found = false;
  if (!list_empty(&relationship_list)) {
    for (e = list_begin(&relationship_list); e != list_end(&relationship_list); e = list_next(e)) { 
      rls_info = list_entry(e, struct relationship_info, elem);
      if (rls_info->child_tid == child_tid && rls_info->parent_tid == thread_current()->tid) { 
        found = true;
        break;
      }
    }
  }
  if (!found)
    return -1;
  
  sema_down(&rls_info->sema); 
  for (e = list_begin(&exit_info_list); e != list_end(&exit_info_list); e = list_next(e)) {
    struct exit_info * info = list_entry(e, struct exit_info, elem);
    if (info->tid == child_tid) {
      list_remove(&info->elem);
      list_remove(&rls_info->elem);
      free(rls_info);
      int returned_status = info->status;
      free(info);
      return returned_status;
    }
  }
    
}

/* Free the current process's resources. */
void
process_exit (void)
{
  enum intr_level old_level = intr_disable();
  struct thread *curr = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = curr->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      curr->pagedir = NULL;
      pagedir_activate (NULL);

      // Let's free resources
      struct hash * pt = curr->pt;
      struct hash_iterator i;
      while (true) {
        hash_first (&i, pt);
        if (hash_next (&i)) {
          struct page *p = hash_entry (hash_cur (&i), struct page, hash_elem);
          free_page(p);
        }
        else
          break;
      }
      free(pt);

      uint32_t * pde;
      for (pde = pd; pde < pd + pd_no (PHYS_BASE); pde++)
        if (*pde & PTE_P) {
          uint32_t *pt = pde_get_pt (*pde);
          uint32_t *pte;
          palloc_free_page (pt);
        }
      palloc_free_page (pd);
    }

  struct exit_info * new_info = malloc(12);
  new_info->status = curr->exit_status;
  new_info->tid = curr->tid;
  list_push_back(&exit_info_list, &new_info->elem);

  bool parent_process_exited = true;
  struct list_elem * e; 
  for (e = list_begin(&relationship_list); e != list_end(&relationship_list); e = list_next(e)) {
    struct relationship_info * rls_info = list_entry(e, struct relationship_info, elem);
    if (rls_info->child_tid == thread_current()->tid) {
      parent_process_exited = false;
      sema_up(&rls_info->sema);
      break;
    }
  }

  if (parent_process_exited) {
    list_remove(&new_info->elem);
    free(new_info);
  }

  for (e = list_begin(&relationship_list); e != list_end(&relationship_list); e =list_next(e)) {
    struct relationship_info * rls_info = list_entry(e, struct relationship_info, elem);
    if (rls_info->parent_tid == thread_current()->tid) {
      list_remove(&rls_info->elem);
      free(rls_info);
      struct list_elem * tmp_e;
      for (tmp_e = list_begin(&exit_info_list); tmp_e != list_end(&exit_info_list); tmp_e = list_next(tmp_e)) {
        struct exit_info * exit_info = list_entry(tmp_e, struct exit_info, elem);
        if (exit_info->tid == rls_info->child_tid) {
          list_remove(&exit_info->elem);
          free(exit_info);
        }
      }
    }
  }
  intr_set_level(old_level);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Do calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      struct page * page = new_page(upage);
      struct frame * frame = allocate_frame(page, PAL_USER);
      uint8_t *kpage = frame->base;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          frame->pinned = false;
          free_page (page);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          frame->pinned = false;
          free_page(page);
          return false; 
        }
      page->writable = writable;
      sema_up(&page->loaded_sema);
      frame->pinned = false;

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      thread_current()->data_segment_end = upage;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;
  
  struct page * page = new_page(((uint8_t *) PHYS_BASE) - PGSIZE);
  struct frame * frame = allocate_frame(page, PAL_USER | PAL_ZERO);
  kpage = frame->base;
  success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
  if (success) {
    *esp = PHYS_BASE;
    sema_up(&page->loaded_sema);
    page->writable = true;
  }
  else {
    frame->pinned = false;
    free_page(page);
  }
  frame->pinned = false;
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
