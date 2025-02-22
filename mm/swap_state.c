// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/mempolicy.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"
#include "swap.h"



/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_folio_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};

struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;
static bool enable_vma_readahead __read_mostly = true;

#define SWAP_RA_ORDER_CEILING	5

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);
static int io_flush_readahead __read_mostly = SWAP_RA_ORDER_CEILING;

/* Leap start */
unsigned long is_custom_prefetch = 0;

void set_custom_prefetch(unsigned long val){
        is_custom_prefetch = val;
        printk("custom prefetch: %s\n", (is_custom_prefetch != 0) ? "set" : "clear" );
}

unsigned long get_custom_prefetch(){
        return is_custom_prefetch;
}
EXPORT_SYMBOL(set_custom_prefetch);
EXPORT_SYMBOL(get_custom_prefetch);

struct swap_entry {
	long delta;
	unsigned long entry;
};

struct swap_trend {
	atomic_t head;
	atomic_t size;
	atomic_t max_size;
	struct swap_entry *history;
};

static struct swap_trend trend_history;

int get_prev_index(int index){
    return ((index > 0) ? (index-1) : (atomic_read(&trend_history.max_size) - 1));
}

void inc_head(void) {
    int current_head = atomic_read(&trend_history.head);
    int max_size = atomic_read(&trend_history.max_size);
    atomic_set(&trend_history.head, (( current_head + 1 ) % max_size));
}

void inc_size(void) {
    int current_size = atomic_read(&trend_history.size);
    int max_size = atomic_read(&trend_history.max_size);
    
    if(current_size < max_size) 
        atomic_inc(&trend_history.size);
}


void init_swap_trend(int size) {
	
	trend_history.history = (struct swap_entry *) kzalloc(size * sizeof(struct swap_entry), GFP_KERNEL);
	atomic_set(&trend_history.head, 0);
	atomic_set(&trend_history.size, 0);
	atomic_set(&trend_history.max_size , size);
	
}
EXPORT_SYMBOL(init_swap_trend);

void log_swap_trend(unsigned long entry) {
	
	long offset_delta;
	int prev_index;
	struct swap_entry se;
	if(atomic_read(&trend_history.size)) {
		prev_index = get_prev_index(atomic_read(&trend_history.head));
		offset_delta = entry - trend_history.history[prev_index].entry;
		
		//printk("prev_index:%ld, offset_delta:%ld\n", prev_index, offset_delta);
		
		se.delta = offset_delta;
		se.entry = entry;
	}
	else {
	    se.delta = 0;
	    se.entry = entry;
	}
	
	trend_history.history[atomic_read(&trend_history.head)] = se;
	inc_head();
	inc_size();
}

int find_trend_in_region(int size, long *major_delta, int *major_count) {
    int maj_index = get_prev_index(atomic_read(&trend_history.head)), count, i, j;
    long candidate;
    
    for (i = get_prev_index(maj_index), j = 1, count = 1; j < size; i = get_prev_index(i), j++) {
        if (trend_history.history[maj_index].delta == trend_history.history[i].delta)
            count++;
        else
            count--;
        if (count == 0) {
            maj_index = i;
            count = 1;
        }
    }
    
    candidate = trend_history.history[maj_index].delta;
    for (i = get_prev_index(atomic_read(&trend_history.head)), j = 0, count = 0; j < size; i = get_prev_index(i), j++) {
        if(trend_history.history[i].delta == candidate)
            count++;
    }
    
    //printk("majority index: %d, candidate: %ld, count:%d\n", maj_index, candidate, count);
    *major_delta = candidate;
    *major_count = count;
    return count > (size/2);
}

int find_trend (int *depth, long *major_delta, int *major_count) {
	int has_trend = 0, size = (int) atomic_read(&trend_history.max_size)/4, max_size;
	max_size = size * 4;
	
	while(has_trend == 0 && size <= max_size) {
		has_trend = find_trend_in_region(size, major_delta, major_count);
		//printk( "at size: %d, trend found? %s\n", size, (has_trend == 0) ? "false" : "true" );
		size *= 2;
	}
	*depth = size;
	return has_trend;
}
/* Leap end */

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Free swap  = %ldkB\n", K(get_nr_swap_pages()));
	printk("Total swap = %lukB\n", K(total_swap_pages));
}

void *get_shadow_from_swap_cache(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swap_cache_index(entry);
	void *shadow;

	shadow = xa_load(&address_space->i_pages, idx);
	if (xa_is_value(shadow))
		return shadow;
	return NULL;
}

/*
 * add_to_swap_cache resembles filemap_add_folio on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache(struct folio *folio, swp_entry_t entry,
			gfp_t gfp, void **shadowp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swap_cache_index(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);
	void *old;

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	folio->swap = entry;

	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			if (shadowp) {
				old = xas_load(&xas);
				if (xa_is_value(old))
					*shadowp = old;
			}
			xas_store(&xas, folio);
			xas_next(&xas);
		}
		address_space->nrpages += nr;
		__node_stat_mod_folio(folio, NR_FILE_PAGES, nr);
		__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr);
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	folio_ref_sub(folio, nr);
	return xas_error(&xas);
}

/*
 * This must be called only on folios that have
 * been verified to be in the swap cache.
 */
void __delete_from_swap_cache(struct folio *folio,
			swp_entry_t entry, void *shadow)
{
	struct address_space *address_space = swap_address_space(entry);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swap_cache_index(entry);
	XA_STATE(xas, &address_space->i_pages, idx);

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, shadow);
		VM_BUG_ON_PAGE(entry != folio, entry);
		xas_next(&xas);
	}
	folio->swap.val = 0;
	folio_clear_swapcache(folio);
	address_space->nrpages -= nr;
	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

/**
 * add_to_swap - allocate swap space for a folio
 * @folio: folio we want to move to swap
 *
 * Allocate swap space for the folio and add the folio to the
 * swap cache.
 *
 * Context: Caller needs to hold the folio lock.
 * Return: Whether the folio was added to the swap cache.
 */
bool add_to_swap(struct folio *folio)
{
	swp_entry_t entry;
	int err;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_uptodate(folio), folio);

	entry = folio_alloc_swap(folio);
	if (!entry.val)
		return false;

	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	err = add_to_swap_cache(folio, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, NULL);
	if (err)
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	/*
	 * Normally the folio will be dirtied in unmap because its
	 * pte should be dirty. A special case is MADV_FREE page. The
	 * page's pte could have dirty bit cleared but the folio's
	 * SwapBacked flag is still set because clearing the dirty bit
	 * and SwapBacked flag has no lock protected. For such folio,
	 * unmap will not set dirty bit for it, so folio reclaim will
	 * not write the folio out. This can cause data corruption when
	 * the folio is swapped in later. Always setting the dirty flag
	 * for the folio solves the problem.
	 */
	folio_mark_dirty(folio);

	return true;

fail:
	put_swap_folio(folio, entry);
	return false;
}

/*
 * This must be called only on folios that have
 * been verified to be in the swap cache and locked.
 * It will never put the folio into the free list,
 * the caller has a reference on the folio.
 */
void delete_from_swap_cache(struct folio *folio)
{
	swp_entry_t entry = folio->swap;
	struct address_space *address_space = swap_address_space(entry);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(folio, entry, NULL);
	xa_unlock_irq(&address_space->i_pages);

	put_swap_folio(folio, entry);
	folio_ref_sub(folio, folio_nr_pages(folio));
}

void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end)
{
	unsigned long curr = begin;
	void *old;

	for (;;) {
		swp_entry_t entry = swp_entry(type, curr);
		unsigned long index = curr & SWAP_ADDRESS_SPACE_MASK;
		struct address_space *address_space = swap_address_space(entry);
		XA_STATE(xas, &address_space->i_pages, index);

		xas_set_update(&xas, workingset_update_node);

		xa_lock_irq(&address_space->i_pages);
		xas_for_each(&xas, old, min(index + (end - curr), SWAP_ADDRESS_SPACE_PAGES)) {
			if (!xa_is_value(old))
				continue;
			xas_store(&xas, NULL);
		}
		xa_unlock_irq(&address_space->i_pages);

		/* search the next swapcache until we meet end */
		curr >>= SWAP_ADDRESS_SPACE_SHIFT;
		curr++;
		curr <<= SWAP_ADDRESS_SPACE_SHIFT;
		if (curr > end)
			break;
	}
}

/*
 * If we are the only user, then try to free up the swap cache.
 *
 * Its ok to check the swapcache flag without the folio lock
 * here because we are going to recheck again inside
 * folio_free_swap() _with_ the lock.
 * 					- Marcelo
 */
void free_swap_cache(struct folio *folio)
{
	if (folio_test_swapcache(folio) && !folio_mapped(folio) &&
	    folio_trylock(folio)) {
		folio_free_swap(folio);
		folio_unlock(folio);
	}
}

/*
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	struct folio *folio = page_folio(page);

	free_swap_cache(folio);
	if (!is_huge_zero_folio(folio))
		folio_put(folio);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct encoded_page **pages, int nr)
{
	struct folio_batch folios;
	unsigned int refs[PAGEVEC_SIZE];

	lru_add_drain();
	folio_batch_init(&folios);
	for (int i = 0; i < nr; i++) {
		struct folio *folio = page_folio(encoded_page_ptr(pages[i]));

		free_swap_cache(folio);
		refs[folios.nr] = 1;
		if (unlikely(encoded_page_flags(pages[i]) &
			     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
			refs[folios.nr] = encoded_nr_pages(pages[++i]);

		if (folio_batch_add(&folios, folio) == 0)
			folios_put_refs(&folios, refs);
	}
	if (folios.nr)
		folios_put_refs(&folios, refs);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

static inline int swap_get_io_flush_order(void)
{
	return READ_ONCE(io_flush_readahead);
}

/*
 * Lookup a swap entry in the swap cache. A found folio will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the folio
 * lock before returning.
 *
 * Caller must lock the swap device or hold a reference to keep it valid.
 */
struct folio *swap_cache_get_folio(swp_entry_t entry,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct folio *folio;

	folio = filemap_get_folio(swap_address_space(entry), swap_cache_index(entry));

    /* Leap */
    if (get_custom_prefetch()) {
        if (swap_use_vma_readahead()) {
            log_swap_trend(PFN_DOWN(addr));
        } else {
            log_swap_trend(swp_offset(entry));
        }
    }
    /* Leap end*/

	if (!IS_ERR(folio)) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;

		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(folio_test_large(folio)))
			return folio;

		readahead = folio_test_clear_readahead(folio);
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead)
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
        }

		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra || get_custom_prefetch()) {
				atomic_inc(&swapin_readahead_hits);
			}
		}
	} else {
		folio = NULL;
	}

	return folio;
}

/**
 * filemap_get_incore_folio - Find and get a folio from the page or swap caches.
 * @mapping: The address_space to search.
 * @index: The page cache index.
 *
 * This differs from filemap_get_folio() in that it will also look for the
 * folio in the swap cache.
 *
 * Return: The found folio or %NULL.
 */
struct folio *filemap_get_incore_folio(struct address_space *mapping,
		pgoff_t index)
{
	swp_entry_t swp;
	struct swap_info_struct *si;
	struct folio *folio = filemap_get_entry(mapping, index);

	if (!folio)
		return ERR_PTR(-ENOENT);
	if (!xa_is_value(folio))
		return folio;
	if (!shmem_mapping(mapping))
		return ERR_PTR(-ENOENT);

	swp = radix_to_swp_entry(folio);
	/* There might be swapin error entries in shmem mapping. */
	if (non_swap_entry(swp))
		return ERR_PTR(-ENOENT);
	/* Prevent swapoff from happening to us */
	si = get_swap_device(swp);
	if (!si)
		return ERR_PTR(-ENOENT);
	index = swap_cache_index(swp);
	folio = filemap_get_folio(swap_address_space(swp), index);
	put_swap_device(si);
	return folio;
}

struct folio *__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct mempolicy *mpol, pgoff_t ilx, bool *new_page_allocated,
		bool skip_if_exists)
{
	struct swap_info_struct *si;
	struct folio *folio;
	struct folio *new_folio = NULL;
	struct folio *result = NULL;
	void *shadow = NULL;

	*new_page_allocated = false;
	si = get_swap_device(entry);
	if (!si)
		return NULL;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after swap_cache_get_folio() failed, re-calling
		 * that would confuse statistics.
		 */
		folio = filemap_get_folio(swap_address_space(entry),
					  swap_cache_index(entry));
		if (!IS_ERR(folio))
			goto got_folio;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!swap_swapcount(si, entry) && swap_slot_cache_enabled)
			goto put_and_return;

		/*
		 * Get a new folio to read into from swap.  Allocate it now if
		 * new_folio not exist, before marking swap_map SWAP_HAS_CACHE,
		 * when -EEXIST will cause any racers to loop around until we
		 * add it to cache.
		 */
		if (!new_folio) {
			new_folio = folio_alloc_mpol(gfp_mask, 0, mpol, ilx, numa_node_id());
			if (!new_folio)
				goto put_and_return;
		}

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry, 1);
		if (!err)
			break;
		else if (err != -EEXIST)
			goto put_and_return;

		/*
		 * Protect against a recursive call to __read_swap_cache_async()
		 * on the same entry waiting forever here because SWAP_HAS_CACHE
		 * is set but the folio is not the swap cache yet. This can
		 * happen today if mem_cgroup_swapin_charge_folio() below
		 * triggers reclaim through zswap, which may call
		 * __read_swap_cache_async() in the writeback path.
		 */
		if (skip_if_exists)
			goto put_and_return;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its folio to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new folio.
	 */
	__folio_set_locked(new_folio);
	__folio_set_swapbacked(new_folio);

	if (mem_cgroup_swapin_charge_folio(new_folio, NULL, gfp_mask, entry))
		goto fail_unlock;

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(new_folio, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow))
		goto fail_unlock;

	mem_cgroup_swapin_uncharge_swap(entry, 1);

	if (shadow)
		workingset_refault(new_folio, shadow);

	/* Caller will initiate read into locked new_folio */
	folio_add_lru(new_folio);
	*new_page_allocated = true;
	folio = new_folio;
got_folio:
	result = folio;
	goto put_and_return;

fail_unlock:
	put_swap_folio(new_folio, entry);
	folio_unlock(new_folio);
put_and_return:
	put_swap_device(si);
	if (!(*new_page_allocated) && new_folio)
		folio_put(new_folio);
	return result;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 *
 * get/put_swap_device() aren't needed to call this function, because
 * __read_swap_cache_async() call them and swap_read_folio() holds the
 * swap cache folio lock.
 */
struct folio *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr,
		struct swap_iocb **plug)
{
	bool page_allocated;
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	mpol = get_vma_policy(vma, addr, 0, &ilx);
	folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
					&page_allocated, false);
	mpol_cond_put(mpol);

	if (page_allocated)
		swap_read_folio(folio, plug);
	return folio;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2 && get_custom_prefetch() == 0) { /* Leap condition added */
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
        prev_offset = offset;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * Note: it is intentional that the same NUMA policy and interleave index
 * are used for every page of the readahead: neighbouring pages on swap
 * are fairly likely to have been swapped out from the same node.
 */
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				    struct mempolicy *mpol, pgoff_t ilx)
{
	struct folio *folio;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	int io_flush_order;
	struct swap_info_struct *si = swp_swap_info(entry);
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	bool page_allocated;

	mask = swapin_nr_pages(offset) - 1;
	/* Leap start */

	if (get_custom_prefetch() != 0) {
		int has_trend = 0, depth, major_count;
		long major_delta;
		has_trend = find_trend(&depth, &major_delta, &major_count);
		if(has_trend) {
			int count = 0;
			start_offset = offset;

			io_flush_order = swap_get_io_flush_order();
			if (io_flush_order)
				blk_start_plug(&plug);

			for (offset = start_offset; count <= mask; offset += major_delta, count++) {
				/* Ok, do the async read-ahead now */
				folio = __read_swap_cache_async(
						swp_entry(swp_type(entry), offset),
						gfp_mask, mpol, ilx, &page_allocated, false);
				if (!folio)
					continue;
				if (page_allocated) {
					swap_read_folio(folio, &splug);
					if (offset != entry_offset) {
						folio_set_readahead(folio);
						count_vm_event(SWAP_RA);
					}
				}
				folio_put(folio);
				if (io_flush_order && io_flush_order < SWAP_RA_ORDER_CEILING) {
					if ((count & ((1 << io_flush_order) - 1)) == 0 &&
							count < mask) {
						blk_finish_plug(&plug);
						blk_start_plug(&plug);
					}
				}
			}
			if (io_flush_order)
				blk_finish_plug(&plug);
			swap_read_unplug(splug);
			lru_add_drain();	/* Push any new pages onto the LRU now */
			goto skip;
		} else {
			goto usual;
		}
	}
usual:
	if (!mask)
		goto skip;

	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset ; offset++) {
		/* Ok, do the async read-ahead now */
		folio = __read_swap_cache_async(
				swp_entry(swp_type(entry), offset),
				gfp_mask, mpol, ilx, &page_allocated, false);
		if (!folio)
			continue;
		if (page_allocated) {
			swap_read_folio(folio, &splug);
			if (offset != entry_offset) {
				folio_set_readahead(folio);
				count_vm_event(SWAP_RA);
			}
		}
		folio_put(folio);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	/* The page was likely read above, so no need for plugging here */
	folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
					&page_allocated, false);
	if (unlikely(page_allocated))
		swap_read_folio(folio, NULL);
	return folio;
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	/* Init leap */
	if (get_custom_prefetch() == 0) {
		init_swap_trend(32);
		set_custom_prefetch(1);
	}

	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	int i;
	struct address_space *spaces = swapper_spaces[type];

	for (i = 0; i < nr_swapper_spaces[type]; i++)
		VM_WARN_ON_ONCE(!mapping_empty(&spaces[i]));
	kvfree(spaces);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
}

static int swap_vma_ra_win(struct vm_fault *vmf, unsigned long *start,
			   unsigned long *end)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, prev_faddr, left, right;
	unsigned int max_win, hits, prev_win, win;

	max_win = 1 << min(READ_ONCE(page_cluster), SWAP_RA_ORDER_CEILING);
	if (max_win == 1)
		return 1;

	faddr = vmf->address;
	ra_val = GET_SWAP_RA_VAL(vma);
	prev_faddr = SWAP_RA_ADDR(ra_val);
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	win = __swapin_nr_pages(PFN_DOWN(prev_faddr), PFN_DOWN(faddr), hits,
				max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info, SWAP_RA_VAL(faddr, win, 0));
	if (win == 1) {
		return 1;
	}
	
	if (faddr == prev_faddr + PAGE_SIZE) {
		left = faddr;
	}
	else if (prev_faddr == faddr + PAGE_SIZE) {
		left = faddr - (win << PAGE_SHIFT) + PAGE_SIZE;
	}
	else {
		left = faddr - (((win - 1) / 2) << PAGE_SHIFT);
	}
	right = left + (win << PAGE_SHIFT);
	if ((long)left < 0) {
		left = 0;
	}
	*start = max3(left, vma->vm_start, faddr & PMD_MASK);
	*end = min3(right, vma->vm_end, (faddr & PMD_MASK) + PMD_SIZE);

	return win;
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @targ_entry: swap entry of the targeted memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @targ_ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whose
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct folio *swap_vma_readahead(swp_entry_t targ_entry, gfp_t gfp_mask,
		struct mempolicy *mpol, pgoff_t targ_ilx, struct vm_fault *vmf)
{
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	struct folio *folio;
	pte_t *pte = NULL, pentry;
	int win, io_flush_order;
	int count = 1;
	unsigned long start, end, addr;
	swp_entry_t entry;
	pgoff_t ilx;
	bool page_allocated;

	if (get_custom_prefetch()) {
		int has_trend = 0, depth, major_count;
		long major_delta;
		has_trend = find_trend(&depth, &major_delta, &major_count);
		if (has_trend) {
			struct vm_area_struct *vma = vmf->vma;
			
			
			win = swapin_nr_pages(PFN_DOWN(vmf->address));
			if (major_delta > 0) {
				start = max3(vmf->address, vma->vm_start, vmf->address & PMD_MASK);
				end = min(vma->vm_end, (vmf->address & PMD_MASK) + PMD_SIZE);
				end = min(start + ((win * major_delta) << PAGE_SHIFT), end);
			} else {
				major_delta = (-1) * major_delta;
				end = min3(vmf->address, vma->vm_end, (vmf->address & PMD_MASK) + PMD_SIZE);
				start = max(vma->vm_start, vmf->address & PMD_MASK);
				start = max(end - ((win * major_delta) << PAGE_SHIFT), start);
			}
			
			ilx = targ_ilx - PFN_DOWN(vmf->address - start);
			
			io_flush_order = swap_get_io_flush_order();
			if (io_flush_order)
				blk_start_plug(&plug);
			
			for (addr = start; addr < end; ilx += major_delta,
					addr += PAGE_SIZE * major_delta) {
				if (!pte++) {
					pte = pte_offset_map(vmf->pmd, addr);
					if (!pte)
						break;
				}
				pentry = ptep_get_lockless(pte);
				if (!is_swap_pte(pentry))
					continue;
				entry = pte_to_swp_entry(pentry);
				if (unlikely(non_swap_entry(entry)))
					continue;
				pte_unmap(pte);
				pte = NULL;
				folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
								&page_allocated, false);
				if (!folio)
					continue;
				if (page_allocated) {
					swap_read_folio(folio, &splug);
					if (addr != vmf->address) {
						folio_set_readahead(folio);
						count_vm_event(SWAP_RA);
					}
				}
				folio_put(folio);
				if (io_flush_order && io_flush_order < SWAP_RA_ORDER_CEILING) {
					if ((count & ((1 << io_flush_order) - 1)) == 0 &&
							count < win) {
						blk_finish_plug(&plug);
						blk_start_plug(&plug);
					}
					count++;
				}
			}
			if (pte)
				pte_unmap(pte);
			if (io_flush_order)
				blk_finish_plug(&plug);
			swap_read_unplug(splug);
			lru_add_drain();
			goto skip;
		} else {
			goto usual;
		}
usual:
	win = swap_vma_ra_win(vmf, &start, &end);
	if (win == 1)
		goto skip;
	ilx = targ_ilx - PFN_DOWN(vmf->address - start);

	blk_start_plug(&plug);
	for (addr = start; addr < end; ilx++, addr += PAGE_SIZE) {
		if (!pte++) {
			pte = pte_offset_map(vmf->pmd, addr);
			if (!pte)
				break;
		}
		pentry = ptep_get_lockless(pte);
		if (!is_swap_pte(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		pte_unmap(pte);
		pte = NULL;
		folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
						&page_allocated, false);
		if (!folio)
			continue;
		if (page_allocated) {
			swap_read_folio(folio, &splug);
			if (addr != vmf->address) {
				folio_set_readahead(folio);
				count_vm_event(SWAP_RA);
			}
		}
		folio_put(folio);
		}
		if (pte)
			pte_unmap(pte);
		blk_finish_plug(&plug);
		swap_read_unplug(splug);
		lru_add_drain();
	}
skip:
	/* The folio was likely read above, so no need for plugging here */
	folio = __read_swap_cache_async(targ_entry, gfp_mask, mpol, targ_ilx,
					&page_allocated, false);
	if (unlikely(page_allocated))
		swap_read_folio(folio, NULL);
	return folio;
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct folio *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf)
{
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	mpol = get_vma_policy(vmf->vma, vmf->address, 0, &ilx);
	folio = swap_use_vma_readahead() ?
		swap_vma_readahead(entry, gfp_mask, mpol, ilx, vmf) :
		swap_cluster_readahead(entry, gfp_mask, mpol, ilx);
	mpol_cond_put(mpol);

	return folio;
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  enable_vma_readahead ? "true" : "false");
}
static ssize_t io_flush_ra_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  io_flush_readahead);
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &enable_vma_readahead);
	if (ret)
		return ret;

	return count;
}
static ssize_t io_flush_ra_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtoint(buf, 10, &io_flush_readahead);
	if (ret)
		return ret;

	return count;
}
static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);
static struct kobj_attribute io_flush_ra_attr = __ATTR_RW(io_flush_ra);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	&io_flush_ra_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif
