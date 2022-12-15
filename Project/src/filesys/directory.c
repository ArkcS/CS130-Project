#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir
{
  struct inode *inode; /* Backing store. */
  off_t pos;           /* Current position. */
};

/* A single directory entry. */
struct dir_entry
{
  block_sector_t inode_sector; /* Sector number of header. */
  char name[NAME_MAX + 1];     /* Null terminated file name. */
  bool in_use;                 /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool dir_create(block_sector_t sector, size_t entry_cnt)
{
  return inode_create(sector, entry_cnt * sizeof(struct dir_entry), true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open(struct inode *inode)
{
  struct dir *dir = calloc(1, sizeof *dir);
  if (inode != NULL && dir != NULL)
  {
    dir->inode = inode;
    dir->pos = 2 * sizeof(struct dir_entry);
    return dir;
  }
  else
  {
    inode_close(inode);
    free(dir);
    return NULL;
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root(void)
{
  return dir_open(inode_open(ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen(struct dir *dir)
{
  return dir_open(inode_reopen(dir->inode));
}

/* Destroys DIR and frees associated resources. */
void dir_close(struct dir *dir)
{
  if (dir != NULL)
  {
    inode_close(dir->inode);
    free(dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode(struct dir *dir)
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup(const struct dir *dir, const char *name,
       struct dir_entry *ep, off_t *ofsp)
{
  struct dir_entry e;
  size_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (e.in_use && !strcmp(name, e.name))
    {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool dir_lookup(const struct dir *dir, const char *name,
                struct inode **inode)
{
  struct dir_entry e;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  if (lookup(dir, name, &e, NULL))
    *inode = inode_open(e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool dir_add(struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen(name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup(dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy(e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool dir_remove(struct dir *dir, const char *name)
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* Find directory entry. */
  if (!lookup(dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open(e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  /* Remove inode. */
  inode_remove(inode);
  success = true;

done:
  inode_close(inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool dir_readdir(struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e)
  {
    dir->pos += sizeof e;
    if (e.in_use)
    {
      strlcpy(name, e.name, NAME_MAX + 1);
      return true;
    }
  }
  return false;
}

/* Return the parent of the dir. */
struct inode *dir_parent(struct dir *dir)
{
  return inode_open(inode_get_parent(dir->inode));
}

/* Check whether the dir is empty. */
bool dir_is_empty(struct dir *dir)
{
  struct dir_entry e;
  off_t cur_pos = 0;
  /* Check if there is any file except "." and ".." */
  while (inode_read_at(dir->inode, &e, sizeof e, cur_pos) == sizeof e)
  {
    cur_pos += sizeof e;
    if (e.in_use)
    {
      if (strcmp(e.name, ".") && strcmp(e.name, ".."))
      {
        return false;
      }
    }
  }
  return true;
}

/* Check if dir is root. */
bool dir_is_root(struct dir *dir)
{
  return inode_get_sector(dir->inode) == ROOT_DIR_SECTOR;
}

bool find_dir(const char *name_, struct inode **inode, char *last_name)
{
  /* Invalid pointer. */
  if (!name_ || !inode || !last_name)
    return false;
  /* Make a copy for strtoken and preprocess. */
  char *name = malloc(sizeof(char) * strnlen(name_, PATH_MAX) + 1);
  char *temp_name = malloc(sizeof(char) * strnlen(name_, PATH_MAX) + 1);
  /* Malloc failed. */
  if (name == NULL || temp_name == NULL)
  {
    return false;
  }
  strlcpy(temp_name, name_, PATH_MAX + 1);

  /* Deal with continous "/" */
  int flag[strlen(temp_name) + 1];
  for (size_t i = 0; i < strlen(temp_name) + 1; i++)
  {
    flag[i] = 0;
  }
  for (size_t i = 0; i < strlen(temp_name); i++)
  {

    if (temp_name[i] == temp_name[i + 1] && temp_name[i] == '/')
    {
      flag[i + 1] = 1;
    }
  }
  size_t pos = 0;
  for (size_t i = 0; i < strlen(temp_name) + 1; i++)
  {
    if (flag[i] == 0)
    {
      name[pos] = temp_name[i];
      pos++;
    }
  }

  free(temp_name);

  struct dir *dir;
  /* Absolute path. */
  if (*name == '/')
    dir = dir_open_root();
  /* Relative path. */
  else
    dir = dir_reopen(thread_current()->cur_dir);

  char *token, *save_ptr;
  struct inode *next_inode;
  bool not_found;
  /* Keep what directory we are currently in. */
  *inode = inode_reopen(dir_get_inode(dir));
  not_found = false;
  for (token = strtok_r(name, "/", &save_ptr); token != NULL;
       token = strtok_r(NULL, "/", &save_ptr))
  {
    /* Can't find next directory or file. */
    if (not_found || strlen(token) > NAME_MAX)
    {
      free(name);
      return false;
    }
    /* Save the next directory to return value. */
    inode_close(*inode);
    *inode = inode_reopen(dir_get_inode(dir));
    /* Keep tract of the last level file as return value, for later usage. */
    strlcpy(last_name, token, NAME_MAX + 1);

    /* Can't find next directory or file. */
    if (!dir_lookup(dir, token, &next_inode))
      not_found = true;
    /* Goto the next directory. */
    dir_close(dir);
    dir = dir_open(next_inode);
  }
  dir_close(dir);
  free(name);
  /* If the path is a directory. */
  if (name_[strlen(name_) - 1] == '/')
  {
    strlcpy(last_name, ".", NAME_MAX + 1);
  }
  return true;
}