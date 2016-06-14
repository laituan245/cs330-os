#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length) {
    uint32_t n = pos / DISK_SECTOR_SIZE;
    disk_sector_t indirect, direct;
    read_sector(inode->data.doubly_indirect, 4 * (n / 128), &indirect, 4);
    read_sector(indirect, 4 * (n % 128), &direct, 4);
    return direct;
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length, int is_dir, disk_sector_t parent)
{
  uint32_t i, j;
  disk_sector_t tmp, indirect;
  static char zeros[DISK_SECTOR_SIZE];
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->is_dir = is_dir;
      disk_inode->parent = parent;
      disk_inode->magic = INODE_MAGIC;
      if (free_map_allocate(1, &disk_inode->doubly_indirect)) {
        for (i = 0; i < sectors; i++) {
          if (!free_map_allocate(1, &tmp))
            break;
          else {
            disk_write(filesys_disk, tmp, zeros);
            if (i % 128 == 0) {
              if (!free_map_allocate(1, &indirect)) {
                free_map_release(tmp, 1);
                break;
              }
              write_sector(disk_inode->doubly_indirect, 4 * (i / 128), &indirect, 4);
            }
            write_sector(indirect, 4 * (i % 128), &tmp, 4);
          }
        }
        if (i == sectors) {
          success = true;
          write_sector (sector, 0, disk_inode, DISK_SECTOR_SIZE);
        }
        else {
          for (j = 0; j < i; j++) {
            disk_sector_t indirect, direct;
            read_sector(disk_inode->doubly_indirect, 4 * (j / 128), &indirect, 4);
            read_sector(indirect, 4 * (j % 128), &direct, 4);
            free_map_release(direct, 1);
            if (j == i - 1 || j % 128 == 127)
              free_map_release(indirect, 1);
          }
          free_map_release(disk_inode->doubly_indirect, 1);
        }
      }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  read_sector (inode->sector, 0, &inode->data, DISK_SECTOR_SIZE);
  sema_init(&inode->file_growth_sema, 1);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  uint32_t i;
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          size_t sectors = bytes_to_sectors (inode->data.length);
          for (i = 0; i < sectors; i++) {
            disk_sector_t indirect, direct;
            read_sector(inode->data.doubly_indirect, 4 * (i / 128), &indirect, 4);
            read_sector(indirect, 4 * (i % 128), &direct, 4);
            free_map_release(direct, 1);
            if (i == sectors - 1 || i % 128 == 127)
              free_map_release(indirect, 1);
          }
          free_map_release (inode->data.doubly_indirect, 1);
          free_map_release (inode->sector, 1);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (!read_sector(sector_idx, sector_ofs, buffer + bytes_read, chunk_size))
        break;
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* File growth.
   Returns true if the operation is successful.
   Returns false otherwise */
bool file_growth(struct inode *inode, size_t old_nbsectors, size_t new_nbsectors) {
  uint32_t i, j;
  bool success = false;
  disk_sector_t tmp, direct, indirect;
  static char zeros[DISK_SECTOR_SIZE];
  if (old_nbsectors % 128)
    read_sector(inode->data.doubly_indirect, 4 * (old_nbsectors / 128), &indirect, 4);
  for (i = old_nbsectors; i < new_nbsectors; i++) {
    if (!free_map_allocate(1, &tmp))
      break;
    else {
      disk_write(filesys_disk, tmp, zeros);
      if (i % 128 == 0) {
        if (!free_map_allocate(1, &indirect)) {
          free_map_release(tmp, 1);
          break;
        }
        write_sector(inode->data.doubly_indirect, 4 * (i / 128), &indirect, 4);
      }
      write_sector(indirect, 4 * (i % 128), &tmp, 4);
    }
  }
  if (i == new_nbsectors)
    success = true;
  else {
    for (j = old_nbsectors; j < i; j++) {
      read_sector(inode->data.doubly_indirect, 4 * (j / 128), &indirect, 4);
      read_sector(indirect, 4 * (j % 128), &direct, 4);
      free_map_release(direct, 1);
      if (j == i - 1 || j % 128 == 127 || !(old_nbsectors % 128 > 0 && old_nbsectors / 128 == j / 128))
        free_map_release(indirect, 1);
    }
  }
  return success;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  sema_down(&inode->file_growth_sema);
  if (size + offset > inode->data.length) {
    // Need to grow the file
    size_t old_nbsectors = bytes_to_sectors(inode->data.length);
    size_t new_nbsectors = bytes_to_sectors(size + offset);
    bool success = file_growth(inode, old_nbsectors, new_nbsectors);
    if (!success) {
      sema_up(&inode->file_growth_sema);
      return 0;
    }
    inode->data.length = size + offset;
    write_sector(inode->sector, 0, &inode->data, DISK_SECTOR_SIZE);
  }
  sema_up(&inode->file_growth_sema);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      if (!write_sector(sector_idx, sector_ofs, buffer + bytes_written, chunk_size))
        break;
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
