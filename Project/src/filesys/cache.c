#include "filesys/cache.h"
#include "devices/block.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/filesys.h"

struct cache_block
  {
    bool dirty;                         /* Whether the cache line is dirty.*/
    bool valid;                         /* Whether the cache line is valid.*/
    block_sector_t disk_sector;         /* The cooresponding block sector on disk. */
    int64_t time;                       /* Most recently use time. */
    struct semaphore sema;              /* Semaphore for this cache line. */
    struct block *block;                /* Which block(device), in this project, always fs_device. */
    char disk_data[BLOCK_SECTOR_SIZE];  /* The cached BLOCK_SECTOR_SIZE size date. */
  };

/* Pointer to the filesystem cache. */
static struct cache_block *cache;

/* Information for a single read-head operation. */
struct read_head_elem
  {
    block_sector_t sector;
    struct block *block;
  };

/* We use "producer" and "consumer" pattern to handle read-head. */
/* All variable for read-head function. */
static struct read_head_elem *read_head_buffer;
static struct condition read_head_buffer_not_full;
static struct condition read_head_buffer_not_empty;
static struct lock read_head_lock;
static uint32_t read_head_buffer_n;

static struct cache_block * choose_evict ();
static struct cache_block * find_cacheline (struct block *block, block_sector_t disk_sector);
static void cache_flusher ();

static void read_ahead ();

void cache_init ()
{
  /* Cache initialization. */
  cache = calloc (CACHE_SIZE, sizeof (struct cache_block));
  read_head_buffer = calloc (READ_AHEAD_BUFFER_SIZE, sizeof (struct read_head_elem));
  for (struct cache_block *cache_line = cache; cache_line < cache + CACHE_SIZE; cache_line++)
  {
    sema_init (&cache_line -> sema, 1);
    cache_line -> valid = 0;
  }
  cond_init (&read_head_buffer_not_full);
  cond_init (&read_head_buffer_not_empty);
  lock_init (&read_head_lock);
  read_head_buffer_n = 0;
  /* Create a thread to flush the thread periodically. */
  thread_create ("flusher", PRI_DEFAULT, cache_flusher, NULL);
  /* Create a thread for asynchronously read-ahead. */
  thread_create ("read-ahead", PRI_DEFAULT, read_ahead, NULL);
}

void
cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  /* Find the sector in cache. */
  struct cache_block *cache_line = find_cacheline (block, sector);
  /* The sector not in cache or not valid. */
  if (cache_line == NULL)
  {
    /* Evict a cache line and prepare it for the new sector. */
    cache_line = choose_evict ();
    cache_line -> dirty = false;
    cache_line -> valid = true;
    cache_line -> disk_sector = sector;
    cache_line -> block = block;
    /* Read from disk to cache. */
    block_read (block, sector, cache_line -> disk_data);
  }
  /* Copy data to destination buffer. */
  memcpy (buffer, cache_line -> disk_data, BLOCK_SECTOR_SIZE);
  /* Update the most recent access time. */
  cache_line -> time = timer_ticks ();
  /* Finish usage of the cache line. */
  sema_up (&cache_line -> sema);
  /* Produce a read-ahead operation and push it to read-ahead buffer. */
  put_read_ahead_buffer (block, sector + 1);
}

void
cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  /* Find the sector in cache. */
  struct cache_block *cache_line = find_cacheline (block, sector);
  /* The sector not in cache or not valid. */
  if (cache_line == NULL)
  {
    /* Evict a cache line and prepare it for the new sector. */
    cache_line = choose_evict ();
    cache_line -> dirty = true;
    cache_line -> valid = true;
    cache_line -> disk_sector = sector;
    cache_line -> block = block;
  }
  /* Write to cache. */
  memcpy (cache_line -> disk_data, buffer, BLOCK_SECTOR_SIZE);
  /* Update the most recently access time. */
  cache_line -> time = timer_ticks ();
  /* Write operation, the cache line is dirty. */
  cache_line -> dirty = true;
  /* Finish usage of the cache line. */
  sema_up (&cache_line -> sema);
}


struct cache_block *
find_cacheline (struct block *block, block_sector_t disk_sector)
{
  for (struct cache_block *cache_line = cache; cache_line < cache + CACHE_SIZE; cache_line++)
  {
    /* We set sema_down in find_cacheline, the caller should sema_up the cache line after usage. */
    sema_down (&cache_line -> sema);
    if (cache_line -> valid && cache_line -> block == block && cache_line -> disk_sector == disk_sector)
      return cache_line;
    else
      sema_up (&cache_line -> sema);
  }
  return NULL;
}

struct cache_block *
choose_evict ()
{
  struct cache_block *evict = NULL;
  /* LRU is used to evict cache line. */
  int64_t earlist_time = timer_ticks ();
  for (struct cache_block *cache_line = cache; cache_line < cache + CACHE_SIZE; cache_line++)
  {
    /* We set sema_down in choose_evict, the caller should sema_up the cache line after usage. */
    sema_down (&cache_line -> sema);
    /* If find a invalid cache line, choose as evict. */
    if (!cache_line -> valid)
    {
      if (evict != NULL)
        sema_up (&evict -> sema);
      return cache_line;
    }
    /* Find the least recently used cache line. */
    if (cache_line -> time < earlist_time)
    {
      if (evict != NULL)
        sema_up (&evict -> sema);
      evict = cache_line;
      earlist_time = cache_line -> time;
    }
    else
      sema_up (&cache_line -> sema);
  }
  /* Write back the evict cache line to disk if it is dirty. */
  if (evict -> dirty)
    block_write (evict -> block, evict -> disk_sector, evict -> disk_data);
  return evict;
}

/* We use "producer" and "consumer" pattern to handle read-head. */
/* The "producer". */
void
put_read_ahead_buffer (struct block *block, block_sector_t sector)
{
  lock_acquire (&read_head_lock);
  while (read_head_buffer_n == READ_AHEAD_BUFFER_SIZE)
    cond_wait (&read_head_buffer_not_full, &read_head_lock);
  
  /* Push a read-ahead operation to read-ahead buffer. */
  struct read_head_elem *elem = read_head_buffer + (read_head_buffer_n++);
  elem -> block = block;
  elem -> sector = sector;
  
  cond_signal (&read_head_buffer_not_empty, &read_head_lock);
  lock_release (&read_head_lock);
}

/* The "consumer". */
void
read_ahead ()
{
  while (true)
  {
    lock_acquire (&read_head_lock);
    while (read_head_buffer_n == 0)
      cond_wait (&read_head_buffer_not_empty, &read_head_lock);
    
    /* Pop a read-ahead operation from read-ahead buffer. */
    struct read_head_elem *elem = read_head_buffer + (--read_head_buffer_n);
    if (elem -> sector < block_size (elem -> block))
    {
      /* Read the disk sector to cache. */
      struct cache_block *cache_line = find_cacheline (elem -> block, elem -> sector);
      if (!cache_line)
      {
        cache_line = choose_evict ();
        cache_line -> dirty = false;
        cache_line -> valid = true;
        cache_line -> disk_sector = elem -> sector;
        cache_line -> block = elem -> block;
        block_read (elem -> block, elem -> sector, cache_line -> disk_data);
      }
      /* Finish the operation about the cache line. */
      sema_up (&cache_line -> sema);
    }
    cond_signal (&read_head_buffer_not_full, &read_head_lock);
    lock_release (&read_head_lock);
  }
}


void
flush_cache ()
{
  /* For each cache line. */
  for (struct cache_block *cache_line = cache; cache_line < cache + CACHE_SIZE; cache_line++)
  {
    sema_down (&cache_line -> sema);
    /* If it is dirty, write it back to disk. */
    if (cache_line -> dirty)
    {
      block_write (cache_line -> block, cache_line -> disk_sector, cache_line -> disk_data);
      cache_line -> dirty = false;
    }
    sema_up (&cache_line -> sema);
  }
}

/* Periodically flush the cache to enhance reliability. */
void
cache_flusher ()
{
  
  while (true)
  {
    flush_cache ();
    timer_msleep (5000);
  }
}