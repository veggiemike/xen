/* SPDX-License-Identifier: GPL-2.0-or-later */
/******************************************************************************
 * arch/x86/mm/mm-locks.h
 *
 * Spinlocks used by the code in arch/x86/mm.
 *
 * Copyright (c) 2011 Citrix Systems, inc.
 * Copyright (c) 2007 Advanced Micro Devices (Wei Huang)
 * Copyright (c) 2006-2007 XenSource Inc.
 * Copyright (c) 2006 Michael A Fetterman
 */

#ifndef _MM_LOCKS_H
#define _MM_LOCKS_H

/* Per-CPU variable for enforcing the lock ordering */
DECLARE_PER_CPU(int, mm_lock_level);

DECLARE_PERCPU_RWLOCK_GLOBAL(p2m_percpu_rwlock);

static inline void mm_lock_init(mm_lock_t *l)
{
    spin_lock_init(&l->lock);
    l->locker = -1;
    l->locker_function = "nobody";
    l->unlock_level = 0;
}

static inline bool mm_locked_by_me(const mm_lock_t *l)
{
    return (l->lock.recurse_cpu == current->processor);
}

static inline int _get_lock_level(void)
{
    return this_cpu(mm_lock_level);
}

#define MM_LOCK_ORDER_MAX                    64
/*
 * Return the lock level taking the domain bias into account. If the domain is
 * privileged a bias of MM_LOCK_ORDER_MAX is applied to the lock level, so that
 * mm locks that belong to a control domain can be acquired after having
 * acquired mm locks of an unprivileged domain.
 *
 * This is required in order to use some hypercalls from a paging domain that
 * take locks of a subject domain and then attempt to copy data to/from the
 * caller domain.
 */
static inline int _lock_level(const struct domain *d, int l)
{
    ASSERT(l <= MM_LOCK_ORDER_MAX);

    return l + (d && is_control_domain(d) ? MM_LOCK_ORDER_MAX : 0);
}

/*
 * If you see this crash, the numbers printed are order levels defined
 * in this file.
 */
static inline void _check_lock_level(const struct domain *d, int l)
{
    int lvl = _lock_level(d, l);

    if ( unlikely(_get_lock_level() > lvl) )
    {
        printk("mm locking order violation: %i > %i\n", _get_lock_level(), lvl);
        BUG();
    }
}

static inline void _set_lock_level(int l)
{
    this_cpu(mm_lock_level) = l;
}

static always_inline void _mm_lock(const struct domain *d, mm_lock_t *l,
                                   const char *func, int level, int rec)
{
    if ( !((mm_locked_by_me(l)) && rec) )
        _check_lock_level(d, level);
    spin_lock_recursive(&l->lock);
    if ( l->lock.recurse_cnt == 1 )
    {
        l->locker_function = func;
        l->unlock_level = _get_lock_level();
    }
    else if ( (unlikely(!rec)) )
        panic("mm lock already held by %s\n", l->locker_function);
    _set_lock_level(_lock_level(d, level));
}

static inline void _mm_enforce_order_lock_pre(const struct domain *d, int level)
{
    _check_lock_level(d, level);
}

static inline void _mm_enforce_order_lock_post(const struct domain *d, int level,
                                               int *unlock_level,
                                               unsigned short *recurse_count)
{
    if ( recurse_count )
    {
        if ( (*recurse_count)++ == 0 )
        {
            *unlock_level = _get_lock_level();
        }
    } else {
        *unlock_level = _get_lock_level();
    }
    _set_lock_level(_lock_level(d, level));
}


static inline void mm_rwlock_init(mm_rwlock_t *l)
{
    percpu_rwlock_resource_init(&l->lock, p2m_percpu_rwlock);
    l->locker = -1;
    l->locker_function = "nobody";
    l->unlock_level = 0;
}

static inline int mm_write_locked_by_me(mm_rwlock_t *l)
{
    return (l->locker == get_processor_id());
}

static always_inline void _mm_write_lock(const struct domain *d, mm_rwlock_t *l,
                                         const char *func, int level)
{
    if ( !mm_write_locked_by_me(l) )
    {
        _check_lock_level(d, level);
        percpu_write_lock(p2m_percpu_rwlock, &l->lock);
        l->locker = get_processor_id();
        l->locker_function = func;
        l->unlock_level = _get_lock_level();
        _set_lock_level(_lock_level(d, level));
    }
    else
        block_lock_speculation();
    l->recurse_count++;
}

static inline void mm_write_unlock(mm_rwlock_t *l)
{
    if ( --(l->recurse_count) != 0 )
        return;
    l->locker = -1;
    l->locker_function = "nobody";
    _set_lock_level(l->unlock_level);
    percpu_write_unlock(p2m_percpu_rwlock, &l->lock);
}

static always_inline void _mm_read_lock(const struct domain *d, mm_rwlock_t *l,
                                        int level)
{
    _check_lock_level(d, level);
    percpu_read_lock(p2m_percpu_rwlock, &l->lock);
    /* There's nowhere to store the per-CPU unlock level so we can't
     * set the lock level. */
}

static inline void mm_read_unlock(mm_rwlock_t *l)
{
    percpu_read_unlock(p2m_percpu_rwlock, &l->lock);
}

/* This wrapper uses the line number to express the locking order below */
#define declare_mm_lock(name)                                                 \
    static always_inline void mm_lock_##name(                                 \
        const struct domain *d, mm_lock_t *l, const char *func, int rec)      \
    { _mm_lock(d, l, func, MM_LOCK_ORDER_##name, rec); }
#define declare_mm_rwlock(name)                                               \
    static always_inline void mm_write_lock_##name(                           \
        const struct domain *d, mm_rwlock_t *l, const char *func)             \
    { _mm_write_lock(d, l, func, MM_LOCK_ORDER_##name); }                     \
    static always_inline void mm_read_lock_##name(const struct domain *d,     \
                                                  mm_rwlock_t *l)             \
    { _mm_read_lock(d, l, MM_LOCK_ORDER_##name); }
/* These capture the name of the calling function */
#define mm_lock(name, d, l) mm_lock_##name(d, l, __func__, 0)
#define mm_lock_recursive(name, d, l) mm_lock_##name(d, l, __func__, 1)
#define mm_write_lock(name, d, l) mm_write_lock_##name(d, l, __func__)
#define mm_read_lock(name, d, l) mm_read_lock_##name(d, l)

/* This wrapper is intended for "external" locks which do not use
 * the mm_lock_t types. Such locks inside the mm code are also subject
 * to ordering constraints. */
#define declare_mm_order_constraint(name)                                       \
    static inline void mm_enforce_order_lock_pre_##name(const struct domain *d) \
    { _mm_enforce_order_lock_pre(d, MM_LOCK_ORDER_##name); }                    \
    static inline void mm_enforce_order_lock_post_##name(const struct domain *d,\
                        int *unlock_level, unsigned short *recurse_count)       \
    { _mm_enforce_order_lock_post(d, MM_LOCK_ORDER_##name, unlock_level,        \
                                  recurse_count); }

static inline void mm_unlock(mm_lock_t *l)
{
    if ( l->lock.recurse_cnt == 1 )
    {
        l->locker_function = "nobody";
        _set_lock_level(l->unlock_level);
    }
    spin_unlock_recursive(&l->lock);
}

static inline void mm_enforce_order_unlock(int unlock_level,
                                            unsigned short *recurse_count)
{
    if ( recurse_count )
    {
        BUG_ON(*recurse_count == 0);
        if ( (*recurse_count)-- == 1 )
        {
            _set_lock_level(unlock_level);
        }
    } else {
        _set_lock_level(unlock_level);
    }
}

/************************************************************************
 *                                                                      *
 * To avoid deadlocks, these locks _MUST_ be taken in the order listed  *
 * below.  The locking functions will enforce this.                     *
 *                                                                      *
 ************************************************************************/

#ifdef CONFIG_HVM

/* Nested P2M lock (per-domain)
 *
 * A per-domain lock that protects the mapping from nested-CR3 to
 * nested-p2m.  In particular it covers:
 * - the array of nested-p2m tables, and all LRU activity therein; and
 * - setting the "cr3" field of any p2m table to a non-P2M_BASE_EAADR value.
 *   (i.e. assigning a p2m table to be the shadow of that cr3 */

#define MM_LOCK_ORDER_nestedp2m               8
declare_mm_lock(nestedp2m)
#define nestedp2m_lock(d)   mm_lock(nestedp2m, d, &(d)->arch.nested_p2m_lock)
#define nestedp2m_unlock(d) mm_unlock(&(d)->arch.nested_p2m_lock)

/* P2M lock (per-non-alt-p2m-table)
 *
 * This protects all queries and updates to the p2m table.
 * Queries may be made under the read lock but all modifications
 * need the main (write) lock.
 *
 * The write lock is recursive as it is common for a code path to look
 * up a gfn and later mutate it.
 *
 * Note that this lock shares its implementation with the altp2m
 * lock (not the altp2m list lock), so the implementation
 * is found there.
 *
 * Changes made to the host p2m when in altp2m mode are propagated to the
 * altp2ms synchronously in ept_set_entry().  At that point, we will hold
 * the host p2m lock; propagating this change involves grabbing the
 * altp2m_list lock, and the locks of the individual alternate p2ms.  In
 * order to allow us to maintain locking order discipline, we split the p2m
 * lock into p2m (for host p2ms) and altp2m (for alternate p2ms), putting
 * the altp2mlist lock in the middle.
 */

#define MM_LOCK_ORDER_p2m                    16
declare_mm_rwlock(p2m);

/* Sharing per page lock
 *
 * This is an external lock, not represented by an mm_lock_t. The memory
 * sharing lock uses it to protect addition and removal of (gfn,domain)
 * tuples to a shared page. We enforce order here against the p2m lock,
 * which is taken after the page_lock to change the gfn's p2m entry.
 *
 * The lock is recursive because during share we lock two pages. */

#define MM_LOCK_ORDER_per_page_sharing       24
declare_mm_order_constraint(per_page_sharing)
#define page_sharing_mm_pre_lock() \
        mm_enforce_order_lock_pre_per_page_sharing(NULL)
#define page_sharing_mm_post_lock(l, r) \
        mm_enforce_order_lock_post_per_page_sharing(NULL, (l), (r))
#define page_sharing_mm_unlock(l, r) mm_enforce_order_unlock((l), (r))

/* Alternate P2M list lock (per-domain)
 *
 * A per-domain lock that protects the list of alternate p2m's.
 * Any operation that walks the list needs to acquire this lock.
 * Additionally, before destroying an alternate p2m all VCPU's
 * in the target domain must be paused.
 */

#define MM_LOCK_ORDER_altp2mlist             32
declare_mm_lock(altp2mlist)
#define altp2m_list_lock(d)   mm_lock(altp2mlist, d, \
                                      &(d)->arch.altp2m_list_lock)
#define altp2m_list_unlock(d) mm_unlock(&(d)->arch.altp2m_list_lock)

/* P2M lock (per-altp2m-table)
 *
 * This protects all queries and updates to the p2m table.
 * Queries may be made under the read lock but all modifications
 * need the main (write) lock.
 *
 * The write lock is recursive as it is common for a code path to look
 * up a gfn and later mutate it.
 */

#define MM_LOCK_ORDER_altp2m                 40
declare_mm_rwlock(altp2m);

static always_inline void p2m_lock(struct p2m_domain *p)
{
    if ( p2m_is_altp2m(p) )
        mm_write_lock(altp2m, p->domain, &p->lock);
    else
        mm_write_lock(p2m, p->domain, &p->lock);
    p->defer_flush++;
}

static inline void p2m_unlock(struct p2m_domain *p)
{
    if ( --p->defer_flush == 0 )
        p2m_unlock_and_tlb_flush(p);
    else
        mm_write_unlock(&p->lock);
}

#define gfn_lock(p,g,o)       p2m_lock(p)
#define gfn_unlock(p,g,o)     p2m_unlock(p)
#define p2m_read_lock(p)      mm_read_lock(p2m, (p)->domain, &(p)->lock)
#define p2m_read_unlock(p)    mm_read_unlock(&(p)->lock)
#define p2m_locked_by_me(p)   mm_write_locked_by_me(&(p)->lock)
#define gfn_locked_by_me(p,g) p2m_locked_by_me(p)

static always_inline void gfn_lock_if(bool condition, struct p2m_domain *p2m,
                                      gfn_t gfn, unsigned int order)
{
    if ( condition )
        gfn_lock(p2m, gfn, order);
    else
        block_lock_speculation();
}

/* PoD lock (per-p2m-table)
 *
 * Protects private PoD data structs: entry and cache
 * counts, page lists, sweep parameters. */

#define MM_LOCK_ORDER_pod                    48
declare_mm_lock(pod)
#define pod_lock(p)           mm_lock(pod, (p)->domain, &(p)->pod.lock)
#define pod_unlock(p)         mm_unlock(&(p)->pod.lock)
#define pod_locked_by_me(p)   mm_locked_by_me(&(p)->pod.lock)

#endif /* CONFIG_HVM */

/* Page alloc lock (per-domain)
 *
 * This is an external lock, not represented by an mm_lock_t. However,
 * pod code uses it in conjunction with the p2m lock, and expecting
 * the ordering which we enforce here.
 * The lock is not recursive. */

#define MM_LOCK_ORDER_page_alloc             56
declare_mm_order_constraint(page_alloc)
#define page_alloc_mm_pre_lock(d)  mm_enforce_order_lock_pre_page_alloc(d)
#define page_alloc_mm_post_lock(d, l) \
        mm_enforce_order_lock_post_page_alloc(d, &(l), NULL)
#define page_alloc_mm_unlock(l)    mm_enforce_order_unlock((l), NULL)

/* Paging lock (per-domain)
 *
 * For shadow pagetables, this lock protects
 *   - all changes to shadow page table pages
 *   - the shadow hash table
 *   - the shadow page allocator
 *   - all changes to guest page table pages
 *   - all changes to the page_info->tlbflush_timestamp
 *   - the page_info->count fields on shadow pages
 *
 * For HAP, it protects the NPT/EPT tables and mode changes.
 *
 * It also protects the log-dirty bitmap from concurrent accesses (and
 * teardowns, etc). */

#define MM_LOCK_ORDER_paging                 64
declare_mm_lock(paging)
#define paging_lock(d)         mm_lock(paging, d, &(d)->arch.paging.lock)
#define paging_lock_recursive(d) \
                    mm_lock_recursive(paging, d, &(d)->arch.paging.lock)
#define paging_unlock(d)       mm_unlock(&(d)->arch.paging.lock)
#define paging_locked_by_me(d) mm_locked_by_me(&(d)->arch.paging.lock)

#endif /* _MM_LOCKS_H */
