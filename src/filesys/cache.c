#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "devices/timer.h"

static struct lock cache_lock;
static struct list sectors_list;
static int count;

struct cached_sector * load_sector(disk_sector_t);
static void flush_periodically();
void flush();

void buffer_cache_init() {
  list_init(&sectors_list);
  lock_init(&cache_lock);
  count = 0;
  thread_create("flush_periodically", PRI_DEFAULT, flush_periodically, NULL);
}

static void flush_periodically(){
  while(true) {
    timer_sleep(10);
    flush();
  }
}

void flush() {
  lock_acquire(&cache_lock);
  struct list_elem *e;
  for (e = list_begin(&sectors_list); e != list_end (&sectors_list); e = list_next (e)) {
    struct cached_sector * tmp = list_entry(e, struct cached_sector, elem);
    sema_down(&tmp->sema);
    if (tmp->dirty) {
      disk_write(filesys_disk, tmp->sector_idx, tmp->data);
      tmp->dirty = false;
    }
    sema_up(&tmp->sema);
  }
  lock_release(&cache_lock);
}

struct cached_sector * get_sector(disk_sector_t sector_idx) {
  lock_acquire(&cache_lock);
  struct cached_sector * rs = NULL;
  struct list_elem *e;
  for (e = list_begin(&sectors_list); e != list_end (&sectors_list); e = list_next (e)) {
    struct cached_sector * tmp = list_entry(e, struct cached_sector, elem);
    if (tmp->sector_idx == sector_idx) {
      rs = tmp;
      sema_down(&rs->sema);
      break;
    }
  }
  if (rs == NULL)
    rs = load_sector(sector_idx);
  lock_release(&cache_lock);
  return rs;
}

struct cached_sector * load_sector(disk_sector_t sector_idx) {
  struct cached_sector * rs = NULL;
  if (count < 64) {
    rs = malloc(sizeof (struct cached_sector));
    if (rs != NULL) {
      rs->data = malloc(DISK_SECTOR_SIZE);
      if (rs->data) {
        rs->sector_idx = sector_idx;
        rs->dirty = false;
        sema_init(&rs->sema, 0);
        disk_read(filesys_disk, sector_idx, rs->data);
        list_push_back(&sectors_list, &rs->elem);
        count++;
      }
      else {
        free(rs);
        rs = NULL;
      }
    }
  }
  else {
    struct list_elem *e = list_begin(&sectors_list);
    rs = list_entry(e, struct cached_sector, elem);
    list_remove(&rs->elem);
    sema_down(&rs->sema);
    if (rs->dirty)
      disk_write(filesys_disk, rs->sector_idx, rs->data);
    rs->sector_idx = sector_idx;
    rs->dirty = false;
    disk_read(filesys_disk, rs->sector_idx, rs->data);
    list_push_back(&sectors_list, &rs->elem);
  }
  return rs;
}

bool write_sector(disk_sector_t sector_idx, off_t sector_ofs, void * buffer, off_t size) {
  struct cached_sector * s = get_sector(sector_idx);
  if (s == NULL)
    return false;
  memcpy (s->data + sector_ofs, buffer, size);
  sema_up(&s->sema);
  s->dirty = true;
  return true;
}

bool read_sector(disk_sector_t sector_idx, off_t sector_ofs, void * buffer, off_t size) {
  struct cached_sector * s = get_sector(sector_idx);
  if (s == NULL)
    return false;
  memcpy (buffer, s->data + sector_ofs, size);
  sema_up(&s->sema);
  return true;
}
