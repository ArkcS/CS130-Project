#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "filesys/cache.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format(void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format)
{
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  inode_init();
  free_map_init();
  cache_init();

  if (format)
    do_format();

  /* Set current dir to root. */
  free_map_open();
  struct dir *root = dir_open_root();
  thread_current()->cur_dir = root;
  /* Init . and .. of root dir. */
  dir_add(root, ".", inode_get_sector(dir_get_inode(root)));
  dir_add(root, "..", inode_get_sector(dir_get_inode(root)));
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void)
{
  free_map_close();
  flush_cache();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;

  /* Find the name of the file to be created and the dir of it. */
  struct inode *inode = NULL;
  char *find_name = malloc(NAME_MAX + 1);
  if (!find_dir(name, &inode, find_name))
  {
    free(find_name);
    return false;
  }

  struct dir *dir = dir_open(inode);
  /* Create the file. */
  bool success = (dir != NULL && free_map_allocate(1, &inode_sector) && inode_create(inode_sector, initial_size, false) && dir_add(dir, find_name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);

  free(find_name);
  dir_close(dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open(const char *name)
{
  /* Find the name of the file to be opened and the dir of it. */
  struct inode *inode = NULL;
  char *find_name = malloc(NAME_MAX + 1);
  if (!find_dir(name, &inode, find_name))
  {
    free(find_name);
    return false;
  }

  struct dir *dir = dir_open(inode);
  struct inode *inode_new = NULL;
  /* Check whether the file is indeed in the dir. */
  if (dir == NULL || !dir_lookup(dir, find_name, &inode_new))
  {
    free(find_name);
    dir_close(dir);
    return false;
  }

  free(find_name);
  dir_close(dir);
  /* If it's a dir, return dir. */
  if (inode_isdir(inode_new))
    return (struct file *)dir_open(inode_new);

  return file_open(inode_new);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char *name)
{

  /* Find the name of the file to be removed and the dir of it. */
  struct inode *inode = NULL;
  char *find_name = malloc(NAME_MAX + 1);
  if (!find_dir(name, &inode, find_name))
  {
    free(find_name);
    return false;
  }
  struct dir *dir = dir_open(inode);

  struct inode *inode_new = NULL;
  /* Check the file or dir is in the directory. */
  if (dir_lookup(dir, find_name, &inode_new))
  {
    /* If the thing to be removed is a dir. */
    if (inode_isdir(inode_new))
    {
      struct dir *dir_new = dir_open(inode_new);
      /* Remove the dir when it's empty. */
      if (dir_is_empty(dir_new))
      {
        /* If there are processes opening the dir, we deny removing it. */
        struct list_elem *e;
        for (e = list_begin(&thread_current()->files); e != list_end(&thread_current()->files);
             e = list_next(e))
        {
          struct my_file_struct *myfile = elem_to_myfile(e);
          struct file *file = myfile_get_file(myfile);
          if (inode_isdir(file_get_inode(file)))
          {
            if (inode_get_sector(file_get_inode(file)) == inode_get_sector(dir_get_inode(thread_current()->cur_dir)))
            {
              dir_close(dir_new);
              dir_close(dir);
              free(find_name);
              return false;
            }
          }
        }
        /* Remove the dir. */
        dir_remove(dir, find_name);
        free(find_name);
        dir_close(dir_new);
        dir_close(dir);
        return true;
        /* The dir is not empty. */
      }
      else
      {
        free(find_name);
        dir_close(dir_new);
        dir_close(dir);
        return false;
      }
    }
  }

  bool success = dir != NULL && dir_remove(dir, find_name);
  free(find_name);
  dir_close(dir);

  return success;
}

/* Formats the file system. */
static void
do_format(void)
{
  printf("Formatting file system...");
  free_map_create();

  if (!dir_create(ROOT_DIR_SECTOR, 100))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}

/* Create a new directory. */
bool filesys_mkdir(const char *path_name, off_t initial_size)
{

  block_sector_t inode_sector = 0;
  struct inode *inode;
  char *dir_name = malloc(NAME_MAX + 1);
  /* Find the name to be created and it's parent dir. */
  if (!find_dir(path_name, &inode, dir_name))
  {
    free(dir_name);
    return false;
  }
  /* If the name is empty, illegal. */
  if (strcmp(dir_name, "") == 0)
  {
    free(dir_name);
    return false;
  }

  /* Create the dir. */
  struct dir *dir = dir_open(inode);
  bool success = (dir != NULL && free_map_allocate(1, &inode_sector) && inode_create(inode_sector, initial_size, true) && dir_add(dir, dir_name, inode_sector));

  if (success)
  {
    /* Init the dir if success. */
    struct inode *inode_new;
    dir_lookup(dir, dir_name, &inode_new);
    struct dir *dir_new = dir_open(inode_new);
    dir_add(dir_new, ".", inode_get_sector(inode_new));
    dir_add(dir_new, "..", inode_get_sector(inode));
    inode_set_parent(inode_get_sector(inode), inode_get_sector(inode_new));
    dir_close(dir_new);
  }

  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);

  free(dir_name);
  dir_close(dir);

  return success;
}

/* Change to the directory to path_name. */
bool filesys_chdir(const char *path_name)
{
  struct inode *inode;
  /* Find the dir we should change to. */
  char *dir_name = malloc(NAME_MAX + 1);
  if (!find_dir(path_name, &inode, dir_name))
  {
    free(dir_name);
    return false;
  }
  struct dir *dir = dir_open(inode);
  struct inode *inode_new = NULL;
  /* Check whether the dir exists. */
  if (dir == NULL || !dir_lookup(dir, dir_name, &inode_new))
  {
    free(dir_name);
    dir_close(dir);
    return false;
  }

  free(dir_name);
  dir_close(dir);
  /* Change to the directory we find. */
  if (inode_isdir(inode_new))
  {
    dir_close(thread_current()->cur_dir);
    thread_current()->cur_dir = dir_open(inode_new);
    if (thread_current()->cur_dir)
    {
      return true;
    }
  }
  return false;
}
