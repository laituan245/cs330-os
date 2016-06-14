#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  flush();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (struct dir * dir, const char *name, off_t initial_size, int is_dir)
{
  disk_sector_t inode_sector = 0;
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, is_dir, dir_get_inode(dir)->sector)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, name, true);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

bool traverse_path(char * path, disk_sector_t * sector, char ** name) {
  char * token, *saved, *path_copy, save_ptr;
  struct inode * inode;
  struct dir * cur;

  if (strlen(path) == 0)
    return false;

  if (path[0] == '/')
    cur = dir_open_root(); // Absolute path
  else
    cur = dir_reopen(thread_current()->cur_dir); // Relative path

  path_copy  = palloc_get_page (0);
  if (path_copy == NULL)
    return false;
  strlcpy (path_copy, path, PGSIZE);

  saved = path_copy;
  for (token = strtok_r (path_copy, "/", &save_ptr); token != NULL; token = strtok_r (NULL, "/", &save_ptr)) {
    saved = token;
    if (!dir_lookup(cur, token, &inode)) {
      dir_close(cur);
      palloc_free_page(path_copy);
      return false;
    }
    else {
      dir_close(cur);
      if (inode->data.is_dir)
        cur = dir_open(inode);
      else {
        token = strtok_r (NULL, "/", &save_ptr);
        if (token != NULL) {
          inode_close(inode);
          palloc_free_page(path_copy);
          return false;
        }
        else {
          *sector = inode->sector;
          inode_close(inode);
          if (name != NULL) {
            * name = palloc_get_page(0);
            if (* name == NULL) {
              palloc_free_page(path_copy);
              return false;
            }
            strlcpy(* name, saved, PGSIZE);
          }
          palloc_free_page(path_copy);
          return true;
        }
        break;
      }
    }
  }
  * sector = dir_get_inode(cur)->sector;
  dir_close(cur);
  if (name != NULL) {
   * name = palloc_get_page(0);                                     
   if (* name == NULL) {
     palloc_free_page(path_copy);                                 
     return false;                                                
   }
   strlcpy(* name, saved, PGSIZE);                                  
  }
  palloc_free_page(path_copy);
  return true;
}
