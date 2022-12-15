#include "page.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <list.h>
#include <hash.h>
#include "devices/timer.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/pte.h"
#include "userprog/pagedir.h"
#include "swap.h"
#include "threads/vaddr.h"

unsigned sup_page_hash (const struct hash_elem *e, void *aux){
    struct sup_pt_elem *page = hash_entry (e, struct sup_pt_elem, hash_elem);
    unsigned result = hash_int (page->vaddr);
    return result;
}


bool sup_page_less (const struct hash_elem *a,const struct hash_elem *b,void *aux){
    const struct sup_pt_elem *left = hash_entry (a, struct sup_pt_elem, hash_elem);
    const struct sup_pt_elem *right = hash_entry (b, struct sup_pt_elem, hash_elem);
    bool result = left->vaddr<right->vaddr;
    return result;

}


void sup_page_delete(struct hash_elem *e, void *aux){
    struct sup_pt_elem *page = hash_entry (e, struct sup_pt_elem, hash_elem);
    free (page);
}

struct sup_pt_elem* find_pt_elem(struct thread* t, const uint32_t* vaddr){
    struct sup_pt_elem page;
    struct hash_elem *e;
    page.vaddr=(uint32_t*)pg_round_down(vaddr);
    e = hash_find (&t->sup_pages, &page.hash_elem);
    if (e!=NULL)
    {
        return hash_entry(e,struct sup_pt_elem,hash_elem);
    }
    else{
        return NULL;
    }

}
struct sup_pt_elem* sup_page_alloc(const uint32_t* vaddr){
    struct sup_pt_elem* page=malloc(sizeof(struct sup_pt_elem));
    
    if (page!=NULL)
    {
        page->vaddr=(uint32_t*)pg_round_down(vaddr);
        page->owner=thread_current();
        page->writable = true;

        if (hash_insert (&thread_current()->sup_pages, &page->hash_elem) != NULL)
        {
        
          //printf("\n=%d\n",hash_entry(hash_insert (&thread_current()->sup_pages, &page->hash_elem),struct sup_pt_elem,hash_elem)->status);
          free (page);
          page = NULL;
        }
    }

    return page;
    
}