#include "vm/frame.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/init.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/palloc.h"

static void frame_add (void *frame);
static void frame_delete (void *frame);
static bool frame_evict (void *frame);

void frame_table_init (){
  lock_init(&frame_lock);
  list_init(&frame_table);
}
void* frame_alloc (enum palloc_flags flags){
  void *frame = palloc_get_page(flags);

  if(frame!=NULL){
    frame_add(frame);
  }else{
    //eviction
    PANIC("evict evict evict");
  }

  return frame;
}
void frame_free (void *frame){
  frame_delete(frame);
  palloc_free_page(frame);
}
static void frame_add (void *frame){
  struct frame_entry *fte = malloc(sizeof(struct frame_entry));
  fte->frame = frame;
  fte->thread = thread_current();

  lock_acquire(&frame_lock);
  list_push_back(&frame_table, &fte->elem);
  lock_release(&frame_lock);
}
static void frame_delete (void *frame){
  struct list_elem *e;
  lock_acquire(&frame_lock);
  for(e=list_begin(&frame_table); e!=list_end(&frame_table); e = list_next(e)){
    struct frame_entry *fte = list_entry(e, struct frame_entry, elem);
    if(fte->frame = frame){
      list_remove(e);
      free(fte);
      break;
    }
  }
  lock_release(&frame_lock);
}
static bool frame_evict (void *frame){
  return true;
}

