/* SPDX-License-Identifier: GPL-2.0-or-later */
/******************************************************************************
 * arch/x86/mm/mem_access.c
 *
 * Parts of this code are Copyright (c) 2009 by Citrix Systems, Inc. (Patrick Colp)
 * Parts of this code are Copyright (c) 2007 by Advanced Micro Devices.
 * Parts of this code are Copyright (c) 2006-2007 by XenSource Inc.
 * Parts of this code are Copyright (c) 2006 by Michael A Fetterman
 * Parts based on earlier work by Michael A Fetterman, Ian Pratt et al.
 */

#include <xen/guest_access.h> /* copy_from_guest() */
#include <xen/mem_access.h>
#include <xen/nospec.h>
#include <xen/vm_event.h>
#include <xen/event.h>
#include <public/vm_event.h>
#include <asm/p2m.h>
#include <asm/altp2m.h>
#include <asm/hvm/emulate.h>
#include <asm/vm_event.h>

#include "mm-locks.h"

/*
 * Get access type for a gfn.
 * If gfn == INVALID_GFN, gets the default access type.
 */
static int _p2m_get_mem_access(struct p2m_domain *p2m, gfn_t gfn,
                               xenmem_access_t *access)
{
    p2m_type_t t;
    p2m_access_t a;
    mfn_t mfn;

    static const xenmem_access_t memaccess[] = {
#define ACCESS(ac) [p2m_access_##ac] = XENMEM_access_##ac
            ACCESS(n),
            ACCESS(r),
            ACCESS(w),
            ACCESS(rw),
            ACCESS(x),
            ACCESS(rx),
            ACCESS(wx),
            ACCESS(rwx),
            ACCESS(rx2rw),
            ACCESS(n2rwx),
#undef ACCESS
    };

    /* If request to get default access. */
    if ( gfn_eq(gfn, INVALID_GFN) )
    {
        *access = memaccess[p2m->default_access];
        return 0;
    }

    gfn_lock(p2m, gfn, 0);
    mfn = p2m->get_entry(p2m, gfn, &t, &a, 0, NULL, NULL);
    gfn_unlock(p2m, gfn, 0);

    if ( mfn_eq(mfn, INVALID_MFN) )
        return -ESRCH;

    if ( (unsigned int)a >= ARRAY_SIZE(memaccess) )
        return -ERANGE;

    *access =  memaccess[a];
    return 0;
}

bool p2m_mem_access_emulate_check(struct vcpu *v,
                                  const struct vm_event_st *rsp)
{
    xenmem_access_t access;
    bool violation = true;
    const struct vm_event_mem_access *data = &rsp->u.mem_access;
    struct domain *d = v->domain;
    struct p2m_domain *p2m = NULL;

    if ( altp2m_active(d) )
        p2m = p2m_get_altp2m(v);
    if ( !p2m )
        p2m = p2m_get_hostp2m(d);

    if ( _p2m_get_mem_access(p2m, _gfn(data->gfn), &access) == 0 )
    {
        switch ( access )
        {
        case XENMEM_access_n:
        case XENMEM_access_n2rwx:
        default:
            violation = data->flags & MEM_ACCESS_RWX;
            break;

        case XENMEM_access_r:
            violation = data->flags & MEM_ACCESS_WX;
            break;

        case XENMEM_access_w:
            violation = data->flags & MEM_ACCESS_RX;
            break;

        case XENMEM_access_x:
            violation = data->flags & MEM_ACCESS_RW;
            break;

        case XENMEM_access_rx:
        case XENMEM_access_rx2rw:
            violation = data->flags & MEM_ACCESS_W;
            break;

        case XENMEM_access_wx:
            violation = data->flags & MEM_ACCESS_R;
            break;

        case XENMEM_access_rw:
            violation = data->flags & MEM_ACCESS_X;
            break;

        case XENMEM_access_rwx:
            violation = false;
            break;
        }
    }

    return violation;
}

bool p2m_mem_access_check(paddr_t gpa, unsigned long gla,
                          struct npfec npfec,
                          struct vm_event_st **req_ptr)
{
    struct vcpu *v = current;
    gfn_t gfn = gaddr_to_gfn(gpa);
    struct domain *d = v->domain;
    struct p2m_domain *p2m = NULL;
    mfn_t mfn;
    p2m_type_t p2mt;
    p2m_access_t p2ma;
    vm_event_request_t *req;
    int rc;

    if ( altp2m_active(d) )
        p2m = p2m_get_altp2m(v);
    if ( !p2m )
        p2m = p2m_get_hostp2m(d);

    /* First, handle rx2rw conversion automatically.
     * These calls to p2m->set_entry() must succeed: we have the gfn
     * locked and just did a successful get_entry(). */
    gfn_lock(p2m, gfn, 0);
    mfn = p2m->get_entry(p2m, gfn, &p2mt, &p2ma, 0, NULL, NULL);

    if ( npfec.write_access && p2ma == p2m_access_rx2rw )
    {
        rc = p2m->set_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2mt, p2m_access_rw, -1);
        ASSERT(rc == 0);
        gfn_unlock(p2m, gfn, 0);
        return true;
    }
    else if ( p2ma == p2m_access_n2rwx )
    {
        ASSERT(npfec.write_access || npfec.read_access || npfec.insn_fetch);
        rc = p2m->set_entry(p2m, gfn, mfn, PAGE_ORDER_4K,
                            p2mt, p2m_access_rwx, -1);
        ASSERT(rc == 0);
    }
    gfn_unlock(p2m, gfn, 0);

    /* Otherwise, check if there is a memory event listener, and send the message along */
    if ( !vm_event_check_ring(d->vm_event_monitor) || !req_ptr )
    {
        /* No listener */
        if ( p2m->access_required )
        {
            gdprintk(XENLOG_INFO, "Memory access permissions failure, "
                                  "no vm_event listener VCPU %d, dom %d\n",
                                  v->vcpu_id, d->domain_id);
            domain_crash(v->domain);
            return false;
        }
        else
        {
            gfn_lock(p2m, gfn, 0);
            mfn = p2m->get_entry(p2m, gfn, &p2mt, &p2ma, 0, NULL, NULL);
            if ( p2ma != p2m_access_n2rwx )
            {
                /* A listener is not required, so clear the access
                 * restrictions.  This set must succeed: we have the
                 * gfn locked and just did a successful get_entry(). */
                rc = p2m->set_entry(p2m, gfn, mfn, PAGE_ORDER_4K,
                                    p2mt, p2m_access_rwx, -1);
                ASSERT(rc == 0);
            }
            gfn_unlock(p2m, gfn, 0);
            return true;
        }
    }

    /*
     * Try to avoid sending a mem event. Suppress events caused by page-walks
     * by emulating but still checking mem_access violations.
     */
    if ( vm_event_check_ring(d->vm_event_monitor) &&
         d->arch.monitor.inguest_pagefault_disabled &&
         npfec.kind == npfec_kind_in_gpt )
    {
        v->arch.vm_event->send_event = true;
        hvm_emulate_one_vm_event(EMUL_KIND_NORMAL, X86_EXC_UD, X86_EVENT_NO_EC);
        v->arch.vm_event->send_event = false;

        return true;
    }

    *req_ptr = NULL;
    req = xzalloc(vm_event_request_t);
    if ( req )
    {
        *req_ptr = req;

        req->reason = VM_EVENT_REASON_MEM_ACCESS;
        req->u.mem_access.gfn = gfn_x(gfn);
        req->u.mem_access.offset = gpa & ((1 << PAGE_SHIFT) - 1);

        if ( npfec.gla_valid )
        {
            req->u.mem_access.flags |= MEM_ACCESS_GLA_VALID;
            req->u.mem_access.gla = gla;
        }

        switch ( npfec.kind )
        {
        case npfec_kind_with_gla:
            req->u.mem_access.flags |= MEM_ACCESS_FAULT_WITH_GLA;
            break;

        case npfec_kind_in_gpt:
            req->u.mem_access.flags |= MEM_ACCESS_FAULT_IN_GPT;
            break;
        }

        req->u.mem_access.flags |= npfec.read_access    ? MEM_ACCESS_R : 0;
        req->u.mem_access.flags |= npfec.write_access   ? MEM_ACCESS_W : 0;
        req->u.mem_access.flags |= npfec.insn_fetch     ? MEM_ACCESS_X : 0;
    }

    /* Return whether vCPU pause is required (aka. sync event) */
    return (p2ma != p2m_access_n2rwx);
}

int p2m_set_altp2m_mem_access(struct domain *d, struct p2m_domain *hp2m,
                              struct p2m_domain *ap2m, p2m_access_t a,
                              gfn_t gfn)
{
    mfn_t mfn;
    p2m_type_t t;
    p2m_access_t old_a;
    int rc;

    rc = altp2m_get_effective_entry(ap2m, gfn, &mfn, &t, &old_a,
                                    AP2MGET_prepopulate);
    if ( rc )
        return rc;

    /*
     * Inherit the old suppress #VE bit value if it is already set, or set it
     * to 1 otherwise
     */
    return ap2m->set_entry(ap2m, gfn, mfn, PAGE_ORDER_4K, t, a, -1);
}

static int set_mem_access(struct domain *d, struct p2m_domain *p2m,
                          struct p2m_domain *ap2m, p2m_access_t a,
                          gfn_t gfn)
{
    int rc = 0;

    if ( ap2m )
    {
        rc = p2m_set_altp2m_mem_access(d, p2m, ap2m, a, gfn);
        /* If the corresponding mfn is invalid we will want to just skip it */
        if ( rc == -ESRCH )
            rc = 0;
    }
    else
    {
        p2m_access_t _a;
        p2m_type_t t;
        mfn_t mfn = p2m_get_gfn_type_access(p2m, gfn, &t, &_a,
                                            P2M_ALLOC, NULL, false);

        rc = p2m->set_entry(p2m, gfn, mfn, PAGE_ORDER_4K, t, a, -1);
    }

    return rc;
}

bool xenmem_access_to_p2m_access(const struct p2m_domain *p2m,
                                 xenmem_access_t xaccess,
                                 p2m_access_t *paccess)
{
    static const p2m_access_t memaccess[] = {
#define ACCESS(ac) [XENMEM_access_##ac] = p2m_access_##ac
        ACCESS(n),
        ACCESS(r),
        ACCESS(w),
        ACCESS(rw),
        ACCESS(x),
        ACCESS(rx),
        ACCESS(wx),
        ACCESS(rwx),
        ACCESS(rx2rw),
        ACCESS(n2rwx),
#undef ACCESS
    };

    switch ( xaccess )
    {
    case 0 ... ARRAY_SIZE(memaccess) - 1:
        xaccess = array_index_nospec(xaccess, ARRAY_SIZE(memaccess));
        *paccess = memaccess[xaccess];
        break;
    case XENMEM_access_default:
        *paccess = p2m->default_access;
        break;
    default:
        return false;
    }

    return true;
}

/*
 * Set access type for a region of gfns.
 * If gfn == INVALID_GFN, sets the default access type.
 */
long p2m_set_mem_access(struct domain *d, gfn_t gfn, uint32_t nr,
                        uint32_t start, uint32_t mask, xenmem_access_t access,
                        unsigned int altp2m_idx)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d), *ap2m = NULL;
    p2m_access_t a;
    unsigned long gfn_l;
    long rc = 0;

    /* altp2m view 0 is treated as the hostp2m */
    if ( altp2m_idx )
    {
        if ( altp2m_idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
             d->arch.altp2m_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] ==
             mfn_x(INVALID_MFN) )
            return -EINVAL;

        ap2m = array_access_nospec(d->arch.altp2m_p2m, altp2m_idx);
    }

    if ( !xenmem_access_to_p2m_access(p2m, access, &a) )
        return -EINVAL;

    /* If request to set default access. */
    if ( gfn_eq(gfn, INVALID_GFN) )
    {
        p2m->default_access = a;
        return 0;
    }

    p2m_lock(p2m);
    if ( ap2m )
        p2m_lock(ap2m);

    for ( gfn_l = gfn_x(gfn) + start; nr > start; ++gfn_l )
    {
        rc = set_mem_access(d, p2m, ap2m, a, _gfn(gfn_l));

        if ( rc )
            break;

        /* Check for continuation if it's not the last iteration. */
        if ( nr > ++start && !(start & mask) && hypercall_preempt_check() )
        {
            rc = start;
            break;
        }
    }

    if ( ap2m )
        p2m_unlock(ap2m);
    p2m_unlock(p2m);

    return rc;
}

long p2m_set_mem_access_multi(struct domain *d,
                              const XEN_GUEST_HANDLE(const_uint64) pfn_list,
                              const XEN_GUEST_HANDLE(const_uint8) access_list,
                              uint32_t nr, uint32_t start, uint32_t mask,
                              unsigned int altp2m_idx)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d), *ap2m = NULL;
    long rc = 0;

    /* altp2m view 0 is treated as the hostp2m */
    if ( altp2m_idx )
    {
        if ( altp2m_idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
             d->arch.altp2m_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] ==
             mfn_x(INVALID_MFN) )
            return -EINVAL;

        ap2m = array_access_nospec(d->arch.altp2m_p2m, altp2m_idx);
    }

    p2m_lock(p2m);
    if ( ap2m )
        p2m_lock(ap2m);

    while ( start < nr )
    {
        p2m_access_t a;
        uint8_t access;
        uint64_t gfn_l;

        if ( copy_from_guest_offset(&gfn_l, pfn_list, start, 1) ||
             copy_from_guest_offset(&access, access_list, start, 1) )
        {
            rc = -EFAULT;
            break;
        }

        if ( !xenmem_access_to_p2m_access(p2m, access, &a) )
        {
            rc = -EINVAL;
            break;
        }

        rc = set_mem_access(d, p2m, ap2m, a, _gfn(gfn_l));

        if ( rc )
            break;

        /* Check for continuation if it's not the last iteration. */
        if ( nr > ++start && !(start & mask) && hypercall_preempt_check() )
        {
            rc = start;
            break;
        }
    }

    if ( ap2m )
        p2m_unlock(ap2m);
    p2m_unlock(p2m);

    return rc;
}

int p2m_get_mem_access(struct domain *d, gfn_t gfn, xenmem_access_t *access,
                       unsigned int altp2m_idx)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( !altp2m_active(d) )
    {
        if ( altp2m_idx )
            return -EINVAL;
    }
    else if ( altp2m_idx ) /* altp2m view 0 is treated as the hostp2m */
    {
        if ( altp2m_idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
             d->arch.altp2m_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] ==
             mfn_x(INVALID_MFN) )
            return -EINVAL;

        p2m = array_access_nospec(d->arch.altp2m_p2m, altp2m_idx);
    }

    return _p2m_get_mem_access(p2m, gfn, access);
}

void arch_p2m_set_access_required(struct domain *d, bool access_required)
{
    ASSERT(atomic_read(&d->pause_count));

    p2m_get_hostp2m(d)->access_required = access_required;

    if ( altp2m_active(d) )
    {
        unsigned int i;
        for ( i = 0; i < MAX_ALTP2M; i++ )
        {
            struct p2m_domain *p2m = d->arch.altp2m_p2m[i];

            if ( p2m )
                p2m->access_required = access_required;
        }
    }
}

bool p2m_mem_access_sanity_check(const struct domain *d)
{
    return is_hvm_domain(d) && cpu_has_vmx && hap_enabled(d);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
