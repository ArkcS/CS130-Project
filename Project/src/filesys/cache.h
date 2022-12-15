#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

#define CACHE_SIZE 64
#define READ_AHEAD_BUFFER_SIZE 64

void cache_init ();
void cache_read (struct block *block, block_sector_t sector, void *buffer);
void cache_write (struct block *block, block_sector_t sector, const void *buffer);
void flush_cache ();