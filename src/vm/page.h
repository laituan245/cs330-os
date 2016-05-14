#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include <hash.h>

enum data_location {
  NONE,
  MEMORY,
  SWAP,
  EXECUTABLE
};

struct page {
  struct hash_elem hash_elem; 
  tid_t pid;
  void * base;
  bool writable;
  bool from_executable;
  enum data_location loc;
  struct swap * swap;
  struct frame * frame;
  off_t ofs;
  size_t page_read_bytes;
  struct semaphore page_sema;
};

struct hash * new_pt();
struct page * new_page(void * base);
struct page * find_page(void * base);
void stack_growth(void * addr);
void free_page(struct hash *, struct page *);
void load_page();

#endif /* vm/page.h */
