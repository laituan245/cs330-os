#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "threads/thread.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include <hash.h>

struct page {
  struct hash_elem hash_elem; 
  tid_t pid;
  void * base;
  bool writable;
  struct swap * swap;
  struct frame * frame;
  struct sema * page_sema;
};

struct hash * new_pt();
struct page * new_page(void * base);
struct page * find_page(void * base);
void stack_growth(void * addr);
void free_page(struct hash *, struct page *);

#endif /* vm/page.h */
