/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_LDLM
#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <linux/lustre_dlm.h>
#include <linux/obd_support.h>
#include <linux/lustre_lib.h>

#include "ldlm_internal.h"

/* The purpose of this function is to return:
 * - the maximum extent
 * - containing the requested extent
 * - and not overlapping existing conflicting extents outside the requested one
 */
static void
ldlm_extent_internal_policy(struct list_head *queue, struct ldlm_lock *req,
                            struct ldlm_extent *new_ex)
{
        struct list_head *tmp;
        ldlm_mode_t req_mode = req->l_req_mode;
        __u64 req_start = req->l_req_extent.start;
        __u64 req_end = req->l_req_extent.end;
        ENTRY;

        lockmode_verify(req_mode);

        list_for_each(tmp, queue) {
                struct ldlm_lock *lock;
                struct ldlm_extent *l_extent;

                lock = list_entry(tmp, struct ldlm_lock, l_res_link);
                l_extent = &lock->l_policy_data.l_extent;

                if (new_ex->start == req_start && new_ex->end == req_end) {
                        EXIT;
                        return;
                }

                /* Don't conflict with ourselves */
                if (req == lock)
                        continue;

                /* If lock doesn't overlap new_ex, skip it. */
                if (l_extent->end < new_ex->start ||
                    l_extent->start > new_ex->end)
                        continue;

                /* Locks are compatible, overlap doesn't matter */
                if (lockmode_compat(lock->l_req_mode, req_mode))
                        continue;

                /* Locks conflicting in requested extents and we can't satisfy
                 * both locks, so ignore it.  Either we will ping-pong this
                 * extent (we would regardless of what extent we granted) or
                 * lock is unused and it shouldn't limit our extent growth. */
                if (lock->l_req_extent.end >= req_start &&
                    lock->l_req_extent.start <= req_end)
                        continue;

                /* We grow extents downwards only as far as they don't overlap
                 * with already-granted locks, on the assumtion that clients
                 * will be writing beyond the initial requested end and would
                 * then need to enqueue a new lock beyond the previous request.
                 * We don't grow downwards if there are lots of lockers. */
                if (l_extent->start < req_start) {
                        if (atomic_read(&req->l_resource->lr_refcount) > 20)
                                new_ex->start = req_start;
                        else
                                new_ex->start = min(l_extent->end+1, req_start);
                }

                /* If we need to cancel this lock anyways because our request
                 * overlaps the granted lock, we grow up to its requested
                 * extent start instead of limiting this extent, assuming that
                 * clients are writing forwards and the lock had over grown
                 * its extent downwards before we enqueued our request. */
                if (l_extent->end > req_end) {
                        if (l_extent->start <= req_end)
                                new_ex->end = max(lock->l_req_extent.start - 1,
                                                  req_end);
                        else
                                new_ex->end = max(l_extent->start - 1, req_end);
                }
        }
        EXIT;
}

/* In order to determine the largest possible extent we can grant, we need
 * to scan all of the queues. */
static void ldlm_extent_policy(struct ldlm_resource *res,
                               struct ldlm_lock *lock, int *flags)
{
        struct ldlm_extent new_ex = { .start = 0, .end = ~0};

        ldlm_extent_internal_policy(&res->lr_granted, lock, &new_ex);
        ldlm_extent_internal_policy(&res->lr_waiting, lock, &new_ex);

        if (new_ex.start != lock->l_policy_data.l_extent.start ||
            new_ex.end != lock->l_policy_data.l_extent.end) {
                *flags |= LDLM_FL_LOCK_CHANGED;
                lock->l_policy_data.l_extent.start = new_ex.start;
                lock->l_policy_data.l_extent.end = new_ex.end;
        }
}

/* Determine if the lock is compatible with all locks on the queue.
 * We stop walking the queue if we hit ourselves so we don't take
 * conflicting locks enqueued after us into accound, or we'd wait forever.
 *
 * 0 if the lock is not compatible
 * 1 if the lock is compatible
 * 2 if this group lock is compatible and requires no further checking
 * negative error, such as EWOULDBLOCK for group locks
 */
static int
ldlm_extent_compat_queue(struct list_head *queue, struct ldlm_lock *req,
                         int send_cbs, int *flags, ldlm_error_t *err)
{
        struct list_head *tmp;
        struct ldlm_lock *lock;
        ldlm_mode_t req_mode = req->l_req_mode;
        __u64 req_start = req->l_req_extent.start;
        __u64 req_end = req->l_req_extent.end;
        int compat = 1;
        ENTRY;

        lockmode_verify(req_mode);

        list_for_each(tmp, queue) {
                lock = list_entry(tmp, struct ldlm_lock, l_res_link);

                if (req == lock)
                        RETURN(compat);

                /* locks are compatible, overlap doesn't matter */
                if (lockmode_compat(lock->l_req_mode, req_mode)) {
                        /* nonCW locks are compatible, overlap doesn't matter */
                        if (req_mode != LCK_CW)
                                continue;
                                
                        /* If we are trying to get a CW lock and there is
                           another one of this kind, we need to compare gid */
                        if (req->l_policy_data.l_extent.gid ==
                             lock->l_policy_data.l_extent.gid) {
                                if (lock->l_req_mode == lock->l_granted_mode)
                                        RETURN(2);

                                /* If we are in nonblocking mode - return
                                   immediately */
                                if (*flags & LDLM_FL_BLOCK_NOWAIT) {
                                        compat = -EWOULDBLOCK;
                                        goto destroylock;
                                }
                                /* If this group lock is compatible with another
                                 * group lock on the waiting list, they must be
                                 * together in the list, so they can be granted
                                 * at the same time.  Otherwise the later lock
                                 * can get stuck behind another, incompatible,
                                 * lock. */
                                ldlm_resource_insert_lock_after(lock, req);
                                /* Because 'lock' is not granted, we can stop
                                 * processing this queue and return immediately.
                                 * There is no need to check the rest of the
                                 * list. */
                                RETURN(0);
                        }
                }

                if (lock->l_req_mode == LCK_CW) {
                        /* If compared lock is CW, then requested is PR/PW/ =>
                         * this is not compatible; extent range does not
                         * matter */
                        if (*flags & LDLM_FL_BLOCK_NOWAIT) {
                                compat = -EWOULDBLOCK;
                                goto destroylock;
                        } else {
                                *flags |= LDLM_FL_NO_TIMEOUT;
                        }
                } else if (lock->l_policy_data.l_extent.end < req_start ||
                           lock->l_policy_data.l_extent.start > req_end) {
                        /* if a non-CW lock doesn't overlap skip it */
                        continue;
                }

                if (!send_cbs)
                        RETURN(0);

                compat = 0;
                if (lock->l_blocking_ast)
                        ldlm_add_ast_work_item(lock, req, NULL, 0);
        }

        return(compat);
destroylock:
        list_del_init(&req->l_res_link);
        ldlm_lock_destroy(req);
        *err = compat;
        RETURN(compat);
}

/* If first_enq is 0 (ie, called from ldlm_reprocess_queue):
  *   - blocking ASTs have already been sent
  *   - the caller has already initialized req->lr_tmp
  *   - must call this function with the ns lock held
  *
  * If first_enq is 1 (ie, called from ldlm_lock_enqueue):
  *   - blocking ASTs have not been sent
  *   - the caller has NOT initialized req->lr_tmp, so we must
  *   - must call this function with the ns lock held once */
int ldlm_process_extent_lock(struct ldlm_lock *lock, int *flags, int first_enq,
                             ldlm_error_t *err)
{
        struct ldlm_resource *res = lock->l_resource;
        struct list_head rpc_list = LIST_HEAD_INIT(rpc_list);
        int rc, rc2;
        ENTRY;

        LASSERT(list_empty(&res->lr_converting));
        *err = ELDLM_OK;

        if (!first_enq) {
                /* Careful observers will note that we don't handle -EWOULDBLOCK
                 * here, but it's ok for a non-obvious reason -- compat_queue
                 * can only return -EWOULDBLOCK if (flags & BLOCK_NOWAIT).
                 * flags should always be zero here, and if that ever stops
                 * being true, we want to find out. */
                LASSERT(*flags == 0);
                LASSERT(res->lr_tmp != NULL);
                rc = ldlm_extent_compat_queue(&res->lr_granted, lock, 0, flags,
                                              err);
                if (rc == 1) {
                        rc = ldlm_extent_compat_queue(&res->lr_waiting, lock, 0,
                                                      flags, err);
                }
                if (rc == 0)
                        RETURN(LDLM_ITER_STOP);

                ldlm_resource_unlink_lock(lock);

                ldlm_extent_policy(res, lock, flags);
                ldlm_grant_lock(lock, NULL, 0, 1);
                RETURN(LDLM_ITER_CONTINUE);
        }

 restart:
        LASSERT(res->lr_tmp == NULL);
        res->lr_tmp = &rpc_list;
        rc = ldlm_extent_compat_queue(&res->lr_granted, lock, 1, flags, err);
        if (rc < 0)
                RETURN(rc); /* lock was destroyed */
        if (rc == 2)
                goto grant;

        rc2 = ldlm_extent_compat_queue(&res->lr_waiting, lock, 1, flags, err);
        if (rc2 < 0)
                RETURN(rc2); /* lock was destroyed */
        res->lr_tmp = NULL;

        if (rc + rc2 == 2) {
        grant:
                ldlm_extent_policy(res, lock, flags);
                ldlm_resource_unlink_lock(lock);
                ldlm_grant_lock(lock, NULL, 0, 0);
        } else {
                /* If either of the compat_queue()s returned failure, then we
                 * have ASTs to send and must go onto the waiting list.
                 *
                 * bug 2322: we used to unlink and re-add here, which was a
                 * terrible folly -- if we goto restart, we could get
                 * re-ordered!  Causes deadlock, because ASTs aren't sent! */
                if (list_empty(&lock->l_res_link))
                        ldlm_resource_add_lock(res, &res->lr_waiting, lock);
                l_unlock(&res->lr_namespace->ns_lock);
                rc = ldlm_run_ast_work(res->lr_namespace, &rpc_list);
                l_lock(&res->lr_namespace->ns_lock);
                if (rc == -ERESTART)
                        GOTO(restart, -ERESTART);
                *flags |= LDLM_FL_BLOCK_GRANTED;
        }
        RETURN(0);
}

/* When a lock is cancelled by a client, the KMS may undergo change if this
 * is the "highest lock".  This function returns the new KMS value.
 *
 * NB: A lock on [x,y] protects a KMS of up to y + 1 bytes! */
__u64 ldlm_extent_shift_kms(struct ldlm_lock *lock, __u64 old_kms)
{
        struct ldlm_resource *res = lock->l_resource;
        struct list_head *tmp;
        struct ldlm_lock *lck;
        __u64 kms = 0;
        ENTRY;

        l_lock(&res->lr_namespace->ns_lock);
        list_for_each(tmp, &res->lr_granted) {
                lck = list_entry(tmp, struct ldlm_lock, l_res_link);

                if (lock == lck)
                        continue;
                if (lck->l_policy_data.l_extent.end >= old_kms)
                        GOTO(out, kms = old_kms);
                kms = lck->l_policy_data.l_extent.end + 1;
        }

        GOTO(out, kms);
 out:
        l_unlock(&res->lr_namespace->ns_lock);
        return kms;
}
