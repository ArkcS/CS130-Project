#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  // block_sector_t start;             /* First data sector. */
  block_sector_t blocks[12];     /* Block array for extending files. */
  uint32_t direct_usage;         /* Record the number of used direct blocks. */
  uint32_t indirect_used;        /* Record whether the indirect block is used. */
  uint32_t indirect_block_usage; /* Record the number of used indirect blocks. */
  uint32_t double_used;          /* Record whether the doubly-indirect block is used. */
  uint32_t double_l1_usage;      /* Record the number of used level-1 doubly-indirect blocks. */
  uint32_t double_l2_usage;      /* Record the number of used level-2 doubly-indirect blocks. */
  uint32_t sector_usage;         /* Record the number of total sectors used. */
  off_t length;                  /* File size in bytes. */
  unsigned magic;                /* Magic number. */
  bool is_dir;                   /* Determine whether the inode is file or directory. */
  block_sector_t parent;         /* Record the parent directory of this inode_disk. */
  uint32_t unused[105];          /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors(off_t size)
{
  return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector(const struct inode *inode, off_t pos)
{
  ASSERT(inode != NULL);

  if (pos < inode->data.length)
  {
    /* If the pos is in the direct block area. */
    if (pos < 10 * BLOCK_SECTOR_SIZE)
    {
      return inode->data.blocks[pos / BLOCK_SECTOR_SIZE];
      /* If the pos is in the indirect block area. */
    }
    else if (pos - 10 * BLOCK_SECTOR_SIZE < 128 * BLOCK_SECTOR_SIZE)
    {
      block_sector_t temp_blocks[128];
      block_read(fs_device, inode->data.blocks[10], &temp_blocks);
      return temp_blocks[(pos - 10 * BLOCK_SECTOR_SIZE) / BLOCK_SECTOR_SIZE];
    }
    else
    {
      /* If the pos is in the doubly-indirect block area. */
      block_sector_t temp_l1_blocks[128];
      block_read(fs_device, inode->data.blocks[11], &temp_l1_blocks);
      size_t l1_index = (pos - (10 + 128) * BLOCK_SECTOR_SIZE) / (128 * BLOCK_SECTOR_SIZE);
      block_sector_t temp_l2_blocks[128];
      block_read(fs_device, temp_l1_blocks[l1_index], &temp_l2_blocks);
      return temp_l2_blocks[((pos - (10 + 128) * BLOCK_SECTOR_SIZE) % (128 * BLOCK_SECTOR_SIZE)) / BLOCK_SECTOR_SIZE];
    }
    /* If the pos is out of range. */
  }
  else
  {
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void)
{
  list_init(&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {

    /* Init the disk_inode. */
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->direct_usage = 0;
    disk_inode->indirect_used = 0;
    disk_inode->indirect_block_usage = 0;
    disk_inode->double_used = 0;
    disk_inode->double_l1_usage = 0;
    disk_inode->double_l2_usage = 0;
    disk_inode->sector_usage = 0;
    disk_inode->is_dir = is_dir;
    disk_inode->parent = ROOT_DIR_SECTOR;

    /* Grow the file according to the length. */
    inode_disk_grow(disk_inode);
    /* Write the metadata to the disk. */
    cache_write(fs_device, sector, disk_inode);
    success = true;

    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e))
  {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector)
    {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  cache_read(fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber(const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    // add
    cache_write(fs_device, inode->sector, &inode->data);

    /* Deallocate blocks if removed. */
    if (inode->removed)
    {
      free_map_release(inode->sector, 1);
      free_inode(inode);
      // free_map_release (inode->data.start,
      //                   bytes_to_sectors (inode->data.length));
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode)
{
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
  {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      /* Read full sector directly into caller's buffer. */
      cache_read(fs_device, sector_idx, buffer + bytes_read);
    }
    else
    {
      /* Read sector into bounce buffer, then partially copy
         into caller's buffer. */
      if (bounce == NULL)
      {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      cache_read(fs_device, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
                     off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  /* If the final length is larger than current length, then grow the file. */
  if (offset + size > inode_length(inode))
  {
    inode->data.length = offset + size;
    inode_disk_grow(&inode->data);
  }

  while (size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);

    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      /* Write full sector directly to disk. */
      cache_write(fs_device, sector_idx, buffer + bytes_written);
    }
    else
    {
      /* We need a bounce buffer. */
      if (bounce == NULL)
      {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }

      /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        cache_read(fs_device, sector_idx, bounce);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      cache_write(fs_device, sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode)
{
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode)
{
  return inode->data.length;
}

/* Grow the file according to the current length. */
void inode_disk_grow(struct inode_disk *disk_inode)
{

  /* Compute how many sectors to extend. */
  off_t remain_sectors = bytes_to_sectors(disk_inode->length);
  remain_sectors -= disk_inode->sector_usage;
  static char zeros[BLOCK_SECTOR_SIZE];
  /* If no more sectors should be extend, return. */
  if (remain_sectors == 0)
  {
    return;
  }

  while (remain_sectors > 0)
  {
    /* Extend the file with direct blocks. */
    if (disk_inode->direct_usage < 10)
    {
      free_map_allocate(1, &disk_inode->blocks[disk_inode->direct_usage]);
      block_write(fs_device, disk_inode->blocks[disk_inode->direct_usage], zeros);
      disk_inode->direct_usage++;
      disk_inode->sector_usage++;
      remain_sectors--;
      /* Extend the file with indirect blocks. */
    }
    else if (disk_inode->indirect_block_usage < 128)
    {
      block_sector_t blocks_indirect[128];
      /* If already used, read data from disk, elsewise create new block. */
      if (disk_inode->indirect_block_usage > 0)
      {
        block_read(fs_device, disk_inode->blocks[10], &blocks_indirect);
      }
      else
      {
        free_map_allocate(1, &disk_inode->blocks[10]);
      }
      /* Use indirect block to extend the file. */
      for (size_t i = disk_inode->indirect_block_usage; i < 128 && remain_sectors > 0; i++)
      {
        free_map_allocate(1, &blocks_indirect[i]);
        block_write(fs_device, blocks_indirect[i], zeros);
        disk_inode->indirect_block_usage++;
        disk_inode->sector_usage++;
        remain_sectors--;
      }
      /* Finally write the data back. */
      block_write(fs_device, disk_inode->blocks[10], &blocks_indirect);
      disk_inode->indirect_used = 1;
      /* Extend the file with doubly-indirect blocks. */
    }
    else
    {

      block_sector_t blocks_l1[128];
      /* If already used, read data from disk, elsewise create new block. */
      if (disk_inode->double_used == 1)
      {
        block_read(fs_device, disk_inode->blocks[11], &blocks_l1);
      }
      else
      {
        free_map_allocate(1, &disk_inode->blocks[11]);
      }

      for (size_t i = disk_inode->double_l1_usage; i < 128 && remain_sectors > 0; i++)
      {
        block_sector_t blocks_l2[128];
        /* If already used, read data from disk, elsewise create new block. */
        if (disk_inode->double_l2_usage > 0)
        {
          block_read(fs_device, blocks_l1[i], &blocks_l2);
        }
        else
        {
          free_map_allocate(1, &blocks_l1[i]);
        }
        /* Use doubly-indirect block to extend the file. */
        for (size_t j = disk_inode->double_l2_usage; j < 128 && remain_sectors > 0; j++)
        {
          free_map_allocate(1, &blocks_l2[j]);
          block_write(fs_device, blocks_l2[j], zeros);
          disk_inode->double_l2_usage++;
          disk_inode->sector_usage++;
          remain_sectors--;
          /* After using up an indirect block, create a new one. */
          if (j == 127)
          {
            disk_inode->double_l2_usage = 0;
            disk_inode->double_l1_usage++;
          }
        }
        /* Finally write the data back. */
        block_write(fs_device, blocks_l1[i], &blocks_l2);
      }
      /* Finally write the data back. */
      block_write(fs_device, disk_inode->blocks[11], &blocks_l1);
      disk_inode->double_used = 1;
    }
  }
  return;
}

/* Free the space and map when file is terminated. */
void free_inode(struct inode *inode)
{
  struct inode_disk *disk_inode = &inode->data;
  /* Compute how many sectors to free. */
  off_t remain_sectors = bytes_to_sectors(inode->data.length);
  if (remain_sectors == 0)
  {
    return;
  }
  while (remain_sectors > 0)
  {
    /* Free all the direct blocks. */
    if (disk_inode->direct_usage > 0)
    {
      free_map_release(disk_inode->blocks[disk_inode->direct_usage - 1], 1);
      disk_inode->direct_usage--;
      remain_sectors--;
      /* Free all the indirect blocks. */
    }
    else if (disk_inode->indirect_used == 1)
    {
      block_sector_t blocks_indirect[128];
      block_read(fs_device, disk_inode->blocks[10], &blocks_indirect);
      for (size_t i = 0; i < disk_inode->indirect_block_usage && remain_sectors > 0; i++)
      {
        free_map_release(blocks_indirect[i], 1);
        remain_sectors--;
      }
      free_map_release(disk_inode->blocks[10], 1);
      disk_inode->indirect_used = 0;
      /* Free all the doubly-direct blocks. */
    }
    else if (disk_inode->double_used == 1)
    {
      block_sector_t blocks_l1[128];
      block_read(fs_device, disk_inode->blocks[11], &blocks_l1);

      for (size_t i = 0; i < disk_inode->double_l1_usage && remain_sectors > 0; i++)
      {
        block_sector_t blocks_l2[128];
        block_read(fs_device, blocks_l1[i], &blocks_l2);

        for (size_t j = 0; j < 128 && remain_sectors > 0; j++)
        {
          free_map_release(blocks_l2[j], 1);
          remain_sectors--;
        }
        free_map_release(blocks_l1[i], 1);
      }
      free_map_release(disk_inode->blocks[11], 1);
      disk_inode->double_used = 0;
    }
  }
  return;
}

/* Return whether the inode is a directory. */
bool inode_isdir(const struct inode *inode)
{
  return inode->data.is_dir;
}

/* Return the parent of the inode. */
block_sector_t
inode_get_parent(const struct inode *inode)
{
  return inode->data.parent;
}

/* Return the sector of the inode. */
block_sector_t
inode_get_sector(const struct inode *inode)
{
  return inode->sector;
}

/* Set the parent of the inode,return true if succeed. */
bool inode_set_parent(block_sector_t parent, block_sector_t child)
{
  struct inode *child_inode = inode_open(child);
  if (child_inode)
  {
    child_inode->data.parent = parent;
    inode_close(child_inode);
    return true;
  }

  return false;
}