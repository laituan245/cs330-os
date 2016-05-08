#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/disk.h"

#define SECTORS_PER_SWAP 8

struct swap {
  disk_sector_t base;
};

void swap_table_init();
struct swap * allocate_swap();
void free_swap(struct swap *);
void swap_in(struct page *p);
void swap_out(struct page *p);

#endif /* vm/swap.h */
