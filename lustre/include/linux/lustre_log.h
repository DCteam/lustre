/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <info@clusterfs.com>
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
 *
 * Generic infrastructure for managing a collection of logs.
 *
 * These logs are used for:
 *
 * - orphan recovery: OST adds record on create
 * - mtime/size consistency: the OST adds a record on first write
 * - open/unlinked objects: OST adds a record on destroy
 *
 * - mds unlink log: the MDS adds an entry upon delete
 *
 * - raid1 replication log between OST's
 * - MDS replication logs
 */

#ifndef _LUSTRE_LOG_H
#define _LUSTRE_LOG_H

#include <linux/obd.h>
#include <linux/lustre_idl.h>

struct obd_trans_info;
struct obd_device;
struct lov_stripe_md;

struct plain_handle_data {
        struct list_head   phd_entry;
        struct llog_handle     *phd_cat_handle; 
        struct llog_cookie phd_cookie; /* cookie of this log in its cat */
        int                phd_last_idx;
};

struct cat_handle_data {
        struct list_head        chd_head;
        struct llog_handle     *chd_current_log; /* currently open log */
};

/* In-memory descriptor for a log object or log catalog */
struct llog_handle {
        struct semaphore        lgh_lock;
        struct llog_logid       lgh_id;              /* id of this log */
        struct obd_device      *lgh_obd;
        struct llog_log_hdr    *lgh_hdr;
        struct file            *lgh_file;
        int                     lgh_last_idx;
        union {
                struct plain_handle_data phd;
                struct cat_handle_data   chd;
        } u;
};

#define LLOG_EEMPTY 4711

/* llog.c  -  general API */
typedef int (*llog_cb_t)(struct llog_handle *, struct llog_rec_hdr *, void *);
int llog_init_handle(struct llog_handle *handle, int flags, struct obd_uuid *uuid);
int llog_process(struct llog_handle *loghandle, llog_cb_t cb, void *data);
extern struct llog_handle *llog_alloc_handle(void);
extern void llog_free_handle(struct llog_handle *handle);
extern int llog_close(struct llog_handle *cathandle);

/* llog_cat.c   -  catalog api */
struct llog_process_data {
        void *lpd_data;
        llog_cb_t lpd_cb;
};
int llog_cat_put(struct llog_handle *cathandle);
int llog_cat_add_rec(struct llog_handle *cathandle, struct llog_rec_hdr *rec,
                     struct llog_cookie *reccookie, void *buf);
int llog_cat_cancel_records(struct llog_handle *cathandle, int count,
                            struct llog_cookie *cookies);
int llog_cat_process(struct llog_handle *cat_llh, llog_cb_t cb, void *data);

/* llog_obd.c */
int obd_llog_setup(struct obd_device *obd, struct obd_device *disk_obd,
                   int index, int count, struct llog_logid *logid);
int obd_llog_cleanup(struct obd_device *obd);
int obd_llog_origin_add(struct obd_export *exp,
                        int index,
                        struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                        struct llog_cookie *logcookies, int numcookies);
int obd_llog_repl_cancel(struct obd_device *, struct lov_stripe_md *lsm,
                         int count, struct llog_cookie *cookies, int flags);

int llog_obd_setup(struct obd_device *obd, struct obd_device *disk_obd,
                   int index, int count, struct llog_logid *logid);
int llog_obd_cleanup(struct obd_device *obd);
int llog_obd_origin_add(struct obd_export *exp,
                        int index,
                        struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                        struct llog_cookie *logcookies, int numcookies);
int llog_initialize(struct obd_device *obd);
int llog_disconnect(struct obd_device *obd);
int llog_cat_initialize(struct obd_device *obd, int count);


/* llog_net.c */
int llog_initiator_connect(struct obd_device *obd);
int llog_receptor_accept(struct obd_device *obd, struct obd_import *imp);
int llog_origin_handle_cancel(struct obd_device *obd, struct ptlrpc_request *req);

/* recov_thread.c */
int llog_obd_repl_cancel(struct obd_device *obd,
                         struct lov_stripe_md *lsm, int count,
                         struct llog_cookie *cookies, int flags);

struct llog_operations {
        int (*lop_write_rec)(struct llog_handle *loghandle,
                             struct llog_rec_hdr *rec, 
                             struct llog_cookie *logcookies, 
                             int numcookies, 
                             void *,
                             int idx);
        int (*lop_destroy)(struct llog_handle *handle);
        int (*lop_next_block)(struct llog_handle *h, 
                              int *curr_idx,  
                              int next_idx, 
                              __u64 *offset, 
                              void *buf, 
                              int len);
        int (*lop_create)(struct obd_device *obd, struct llog_handle **,
                          struct llog_logid *logid, char *name);
        int (*lop_close)(struct llog_handle *handle);
        int (*lop_read_header)(struct llog_handle *handle);
        /* for devices in stacks, using other obd's for log storage */
        int (*lop_origin_setup)(struct obd_device *, int);
        int (*lop_origin_cleanup)(struct obd_device *);
        int (*lop_origin_open)(struct obd_device *originator, 
                               struct obd_device *disk_obd,
                               int index, int named, int flags, 
                               struct obd_uuid *log_uuid);
};

extern struct llog_operations llog_lvfs_ops;

static inline int llog_obd2ops(struct obd_device *obd,
                               struct llog_operations **lop)
{
        struct obd_export *exp;

        if (obd == NULL)
                 return -ENOTCONN;
        exp = obd->obd_log_exp;
        if (exp == NULL)
                return -ENOTCONN;
        if (exp->exp_obd == NULL)
                return -ENOTCONN;
        *lop = exp->exp_obd->obd_logops;
        if (*lop == NULL)
                return -EOPNOTSUPP;
        return 0;
}

static inline int llog_handle2ops(struct llog_handle *loghandle,
                                  struct llog_operations **lop)
{
        if (loghandle == NULL)
                return -EINVAL;
        return llog_obd2ops(loghandle->lgh_obd, lop);
}


static inline int llog_write_rec(struct llog_handle *handle,
                                 struct llog_rec_hdr *rec,
                                 struct llog_cookie *logcookies,
                                 int numcookies, void *buf, int idx)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;
        
        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_write_rec == NULL)
                RETURN(-EOPNOTSUPP);
        LASSERT((le16_to_cpu(rec->lrh_len) % LLOG_MIN_REC_SIZE) == 0);

        rc = lop->lop_write_rec(handle, rec, logcookies, numcookies, buf, idx);
        RETURN(rc);
}

static inline int llog_read_header(struct llog_handle *handle)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;
        
        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_read_header == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_read_header(handle);
        RETURN(rc);
}

static inline int llog_destroy(struct llog_handle *handle)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;
        
        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_destroy == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_destroy(handle);
        RETURN(rc);
}

#if 0
static inline int llog_cancel(struct obd_export *exp,
                              struct lov_stripe_md *lsm, int count,
                              struct llog_cookie *cookies, int flags)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_cancel == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_cancel(exp, lsm, count, cookies, flags);
        RETURN(rc);
}
#endif 

static inline int llog_next_block(struct llog_handle *loghandle, int *cur_idx,
                                  int next_idx, __u64 *cur_offset, void *buf,
                                  int len)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_next_block == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_next_block(loghandle, cur_idx, next_idx, cur_offset, buf,
                                 len);
        RETURN(rc);
}

static inline int llog_create(struct obd_device *obd, struct llog_handle **res,
                              struct llog_logid *logid, char *name)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_obd2ops(obd, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_create == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_create(obd, res, logid, name);
        RETURN(rc);
}


/* llog obd interfaces */
#define LLOG_OBD_MAX_HANDLES 3

/* MDS stored handles in OSC */
#define LLOG_OBD_DEL_LOG_HANDLE 0

/* OBDFILTER stored handles in OBDFILTER */
#define LLOG_OBD_SZ_LOG_HANDLE  0
#define LLOG_OBD_RD1_LOG_HANDLE 1

struct llog_obd_ctxt {
        struct obd_device *loc_obd;
        struct llog_handle *loc_handles[LLOG_OBD_MAX_HANDLES];
        struct llog_commit_data *loc_llcd;
        struct semaphore loc_sem; /* protects loc_llcd */
        struct obd_import *loc_imp;
};

void llog_obd_cleanup_ctxt(struct obd_device *obd);
int obd_log_cancel(struct obd_export *exp, struct llog_handle *cathandle,
                   void *buf, int count, struct llog_cookie *cookies, int flags);


int llog_originator_setup(struct obd_device *, int);
int llog_originator_cleanup(struct obd_device *);
int llog_originator_open(struct obd_device *originator, 
                         struct obd_device *disk_obd,
                         int index, int named, int flags, 
                         struct obd_uuid *log_uuid);



#endif
