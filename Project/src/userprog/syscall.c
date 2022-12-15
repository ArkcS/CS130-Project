#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

static void syscall_handler(struct intr_frame *);
static struct lock my_lock;

struct my_file_struct
{
  int fd;                /* File descriptor. */
  struct file *file;     /* File struct. */
  struct list_elem elem; /* Use for construct a list. */
};

static struct my_file_struct *find_file(const int fd)
{
  /* Traverse the list and find the file with certain fd we want. */
  for (struct list_elem *e = list_begin(&thread_current()->files); e != list_end(&thread_current()->files); e = list_next(e))
  {
    struct my_file_struct *each_file = list_entry(e, struct my_file_struct, elem);
    if (each_file->fd == fd)
    {
      return each_file;
    }
  }
  return NULL;
}

static bool check_valid_ptr(const void *ptr)
{
  /* Case that pointer is NULL. */
  if (ptr == NULL)
  {
    return false;
  }
  /* Check whether it's user vaddr. */
  if (!is_user_vaddr(ptr))
  {
    return false;
  }
  /* Check whether we get can get page from pagedir. */
  if (pagedir_get_page(thread_current()->pagedir, ptr) == NULL)
  {
    return false;
  }
  return true;
}

static void check_ptr(const void *ptr)
{

  /* Check the pointer itself whether it's valid. */
  if (!check_valid_ptr(ptr))
  {
    thread_exit_with_code(-1);
  }
}

static void check_ptr_2(const void *ptr, int size)
{

  /* Check the end of the buffer whether it's valid. */
  if (!check_valid_ptr(ptr + size - 1))
  {
    thread_exit_with_code(-1);
  }
  /* Check each page of the buffer whether it's valid. */
  for (const void *flag = ptr; flag < ptr + size; flag += PGSIZE)
  {
    if (!check_valid_ptr(flag))
      thread_exit_with_code(-1);
  }
}

void thread_exit_with_code(int code)
{
  /* A special exit we can call. */
  thread_current()->exit_state = code;
  thread_exit();
}

void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  /* Init the lock. */
  lock_init(&my_lock);
}

static void
syscall_handler(struct intr_frame *f)
{
  /* For each pointer and its content if it is a pointer to pointer,
     check whether it is valid and safe to do syscall. */
  check_ptr(f->esp);
  switch (*(int *)f->esp)
  {
  case SYS_HALT:
  {
    /* Simply power off. */
    shutdown_power_off();
    break;
  }
  case SYS_EXIT:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    int state = *((int *)f->esp + 1);
    /* Save the exit state in TCB. */
    thread_current()->exit_state = state;
    thread_exit();
    break;
  }
  case SYS_EXEC:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(*(ptr + 1));
    const char *cmd_line = *((char **)f->esp + 1);
    /* Excute the program. */
    acquire_l();
    tid_t tid = process_execute(cmd_line);
    release_l();
    /* The last child is the newly create process. */
    struct thread *child = list_entry(list_back(&thread_current()->children), struct thread, child_elem);
    /* Return load success or not. */
    if (!child->load_success)
      f->eax = -1;
    else
      f->eax = tid;
    break;
  }
  case SYS_WAIT:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    tid_t tid = *((int *)f->esp + 1);
    /* All done in process_wait, return the child process's exit state,
       or -1 if invalid call. */
    f->eax = process_wait(tid);
    break;
  }
  case SYS_CREATE:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(ptr + 2);
    check_ptr(*(ptr + 1));
    /* Creates a new file called file. */
    acquire_l();
    f->eax = filesys_create(*(const char **)(ptr + 1), *((int *)(ptr + 2)));
    release_l();
    break;
  }
  case SYS_REMOVE:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(*(ptr + 1));
    /* Deletes the file called file. */
    acquire_l();
    f->eax = filesys_remove(*(const char **)(ptr + 1));
    release_l();
    break;
  }
  case SYS_OPEN:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(*(ptr + 1));
    /* Opens the file called file. */
    acquire_l();
    struct file *my_file = filesys_open(*(const char **)(ptr + 1));
    /* Check if file exists. */
    if (!my_file)
    {
      f->eax = -1;
    }
    else
    {
      /* File exists, save infos into a struct. */
      struct thread *t = thread_current();
      struct my_file_struct *file_thread = malloc(sizeof(struct my_file_struct));
      file_thread->fd = t->fd++;
      file_thread->file = my_file;
      /* Push into the list. */
      list_push_back(&t->files, &file_thread->elem);
      f->eax = file_thread->fd;
    }
    release_l();
    break;
  }
  case SYS_FILESIZE:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    /* Find the file with certain fd. */
    struct my_file_struct *cur_file = find_file(*((int *)f->esp + 1));
    /* Get the filesize. */
    acquire_l();
    f->eax = file_length(cur_file->file);
    release_l();
    break;
  }
  case SYS_READ:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(ptr + 2);
    check_ptr(ptr + 3);
    /* Get all the input. */
    int fd = *((int *)f->esp + 1);
    const void *buffer = (const void *)(*((int *)f->esp + 2));
    unsigned size = *((unsigned *)f->esp + 3);
    check_ptr_2(*(ptr + 2), size);
    /* Fd 0 reads from the keyboard. */
    if (fd == 0)
    {
      char *temp = buffer;
      for (size_t i = 0; i < size; i++, temp++)
      {
        *temp = input_getc();
      }
      f->eax = size;
    }
    else
    {
      /* Reads size bytes from the file open as fd into buffer. */
      struct my_file_struct *cur_file = find_file(fd);
      /* Check if file exists. */
      if (cur_file)
      {
        acquire_l();
        f->eax = file_read(cur_file->file, buffer, size);
        release_l();
      }
      else
      {
        /* Case that file doesn't exist. */
        f->eax = -1;
      }
    }
    break;
  }
  case SYS_WRITE:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(ptr + 2);
    check_ptr(ptr + 3);
    /* Get all the input. */
    int fd = *((int *)f->esp + 1);
    const void *buffer = (const void *)(*((int *)f->esp + 2));
    unsigned size = *((unsigned *)f->esp + 3);
    check_ptr_2(*(ptr + 2), size);
    /* Fd 1 writes to the console. */
    if (fd == 1)
    {

      putbuf((const char *)buffer, size);
      f->eax = strlen(buffer);
    }
    else
    {
      /* Writes size bytes from buffer to the open file fd. */
      struct my_file_struct *cur_file = find_file(fd);
      if (cur_file)
      {
        acquire_l();
        f->eax = file_write(cur_file->file, buffer, size);
        release_l();
      }
      else
      {
        /* Case that file doesn't exist. */
        f->eax = 0;
      }
    }
    break;
  }
  case SYS_SEEK:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    struct my_file_struct *cur_file = find_file(*((int *)f->esp + 1));
    if (cur_file)
    {
      /* Changes the next byte to be read or written in open file fd to position,
      expressed in bytes from the beginning of the file.  */
      acquire_l();
      file_seek(cur_file->file, *((unsigned *)f->esp + 2));
      release_l();
    }

    break;
  }
  case SYS_TELL:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    struct my_file_struct *cur_file = find_file(*((int *)f->esp + 1));
    if (cur_file)
    {
      /* Returns the position of the next byte to be read or written in open file fd,
      expressed in bytes from the beginning of the file. */
      acquire_l();
      f->eax = file_tell(cur_file->file);
      release_l();
    }
    else
    {
      /* Case that file doesn't exist. */
      f->eax = -1;
    }
    break;
  }
  case SYS_CLOSE:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    struct my_file_struct *cur_file = find_file(*((int *)f->esp + 1));
    /* Case that file doesn't exist. */
    if (cur_file == NULL)
    {
      break;
    }
    /* Closes file descriptor fd.
    Exiting or terminating a process implicitly closes all its open file descriptors,
    as if by calling this function for each one. */
    acquire_l();
    file_close(cur_file->file);
    release_l();
    /* Remove elem from list, and free the space we allocated. */
    list_remove(&cur_file->elem);
    free(cur_file);
    break;
  }
  case SYS_CHDIR:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(*(ptr + 1));
    /* Change current directory. */
    f->eax = filesys_chdir(*(const char **)(ptr + 1));
    break;
  }
  case SYS_MKDIR:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(*(ptr + 1));
    /* Create a new directory. */
    acquire_l();
    f->eax = filesys_mkdir(*(const char **)(ptr + 1), 0);
    release_l();
    break;
  }
  case SYS_READDIR:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    check_ptr(ptr + 2);
    check_ptr(*(ptr + 2));
    int fd = *((int *)f->esp + 1);
    char *name = *(const char **)(ptr + 2);
    struct my_file_struct *cur_file = find_file(fd);
    /* If input is indeed a directory, read next entry. */
    if (inode_isdir(file_get_inode(cur_file->file)))
    {
      struct dir *dir = (struct dir *)(cur_file->file);
      f->eax = dir_readdir(dir, name);
    }
    else
    {
      f->eax = false;
      break;
    }
    break;
  }
  case SYS_ISDIR:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    struct my_file_struct *cur_file = find_file(*((int *)f->esp + 1));
    /* Determine whether input is a directory. */
    if (cur_file)
    {
      f->eax = inode_isdir(file_get_inode(cur_file->file));
    }

    break;
  }
  case SYS_INUMBER:
  {
    /* Check if the pointer is valid. */
    uint32_t *ptr = (uint32_t *)f->esp;
    check_ptr(ptr + 1);
    struct my_file_struct *cur_file = find_file(*((int *)f->esp + 1));
    /* Return the inode number of the input. */
    if (cur_file)
    {
      f->eax = inode_get_inumber(file_get_inode(cur_file->file));
    }
    break;
  }
  }
}

void close_all_files()
{
  /* Pop each elem from list, remove them and free the space. */
  while (!list_empty(&thread_current()->files))
  {
    struct my_file_struct *cur_file = list_entry(list_pop_back(&thread_current()->files), struct my_file_struct, elem);
    // file_allow_write(cur_file->file);
    /* Close each file. */
    file_close(cur_file->file);
    list_remove(&cur_file->elem);
    /* Free the space. */
    free(cur_file);
  }
}

void push_file(struct file *file)
{
  /* Push the file into the list in thread, save info. */
  struct thread *t = thread_current();
  struct my_file_struct *file_thread = malloc(sizeof(struct my_file_struct));
  file_thread->fd = t->fd++;
  file_thread->file = file;
  /* Push into the list. */
  list_push_back(&t->files, &file_thread->elem);
  return file;
}

/* Accquie the lock. */
void acquire_l()
{
  lock_acquire(&my_lock);
}

/* Release the lock. */
void release_l()
{
  lock_release(&my_lock);
}

/* Get file from my_file_struct. */
struct file *myfile_get_file(struct my_file_struct *my_file)
{
  return my_file->file;
}

/* Get list element from my_file_struct. */
struct my_file_struct *elem_to_myfile(struct list_elem *e)
{
  return list_entry(e, struct my_file_struct, elem);
}