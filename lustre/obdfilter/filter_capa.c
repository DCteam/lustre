/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdfilter/filter_capa.c
 *
 * Author: Lai Siyao <lsy@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/fs.h>
#include <linux/version.h>
#include <asm/uaccess.h>
#include <linux/file.h>
#include <linux/kmod.h>

#include <lustre_fsfilt.h>
#include <lustre_capa.h>

#include "filter_internal.h"

static inline __u32 filter_ck_keyid(struct filter_capa_key *key)
{
        return key->k_key.lk_keyid;
}

int filter_update_capa_key(struct obd_device *obd, struct lustre_capa_key *new)
{
        struct filter_obd *filter = &obd->u.filter;
        struct filter_capa_key *k, *keys[2] = { NULL, NULL };
        int i;

        cfs_spin_lock(&capa_lock);
        cfs_list_for_each_entry(k, &filter->fo_capa_keys, k_list) {
                if (k->k_key.lk_seq != new->lk_seq)
                        continue;

                if (keys[0]) {
                        keys[1] = k;
                        if (filter_ck_keyid(keys[1]) > filter_ck_keyid(keys[0]))
                                keys[1] = keys[0], keys[0] = k;
                } else {
                        keys[0] = k;
                }
        }
        cfs_spin_unlock(&capa_lock);

        for (i = 0; i < 2; i++) {
                if (!keys[i])
                        continue;
                if (filter_ck_keyid(keys[i]) != new->lk_keyid)
                        continue;
                /* maybe because of recovery or other reasons, MDS sent the
                 * the old capability key again.
                 */
                cfs_spin_lock(&capa_lock);
                keys[i]->k_key = *new;
                cfs_spin_unlock(&capa_lock);

                RETURN(0);
        }

        if (keys[1]) {
                /* if OSS already have two keys, update the old one */
                k = keys[1];
        } else {
                OBD_ALLOC_PTR(k);
                if (!k)
                        RETURN(-ENOMEM);
                CFS_INIT_LIST_HEAD(&k->k_list);
        }

        cfs_spin_lock(&capa_lock);
        k->k_key = *new;
        if (cfs_list_empty(&k->k_list))
                cfs_list_add(&k->k_list, &filter->fo_capa_keys);
        cfs_spin_unlock(&capa_lock);

        DEBUG_CAPA_KEY(D_SEC, new, "new");
        RETURN(0);
}

int filter_auth_capa(struct obd_export *exp, struct lu_fid *fid, obd_seq seq,
                     struct lustre_capa *capa, __u64 opc)
{
        struct obd_device *obd = exp->exp_obd;
        struct filter_obd *filter = &obd->u.filter;
        struct filter_capa_key *k;
        struct lustre_capa_key key;
        struct obd_capa *oc;
        __u8 *hmac;
        int keys_ready = 0, key_found = 0, rc = 0;
        ENTRY;

        /* skip capa check for llog and obdecho */
        if (!fid_seq_is_mdt(seq))
                RETURN(0);

        /* capability is disabled */
        if (!filter->fo_fl_oss_capa)
                RETURN(0);

        if (!(exp->exp_connect_flags & OBD_CONNECT_OSS_CAPA))
                RETURN(0);

        if (capa == NULL) {
                if (fid)
                        CERROR("seq/fid/opc "LPU64"/"DFID"/"LPX64
                               ": no capability has been passed\n",
                               seq, PFID(fid), opc);
                else
                        CERROR("seq/opc "LPU64"/"LPX64
                               ": no capability has been passed\n",
                               seq, opc);
                RETURN(-EACCES);
        }

        if (opc == CAPA_OPC_OSS_READ) {
                if (!(capa->lc_opc & CAPA_OPC_OSS_RW))
                        rc = -EACCES;
        } else if (!capa_opc_supported(capa, opc)) {
                rc = -EACCES;
        }
        if (rc) {
                DEBUG_CAPA(D_ERROR, capa, "opc "LPX64" not supported by", opc);
                RETURN(rc);
        }

        oc = capa_lookup(filter->fo_capa_hash, capa, 0);
        if (oc) {
                cfs_spin_lock(&oc->c_lock);
                if (capa_is_expired(oc)) {
                        DEBUG_CAPA(D_ERROR, capa, "expired");
                        rc = -ESTALE;
                }
                cfs_spin_unlock(&oc->c_lock);

                capa_put(oc);
                RETURN(rc);
        }

        if (capa_is_expired_sec(capa)) {
                DEBUG_CAPA(D_ERROR, capa, "expired");
                RETURN(-ESTALE);
        }

        cfs_spin_lock(&capa_lock);
        cfs_list_for_each_entry(k, &filter->fo_capa_keys, k_list) {
                if (k->k_key.lk_seq == seq) {
                        keys_ready = 1;
                        if (k->k_key.lk_keyid == capa_keyid(capa)) {
                                key = k->k_key;
                                key_found = 1;
                                break;
                        }
                }
        }
        cfs_spin_unlock(&capa_lock);

        if (!keys_ready) {
                CDEBUG(D_SEC, "MDS hasn't propagated capability keys yet, "
                       "ignore check!\n");
                RETURN(0);
        }

       if (!key_found) {
                DEBUG_CAPA(D_ERROR, capa, "no matched capability key for");
                RETURN(-ESTALE);
        }

        OBD_ALLOC(hmac, CAPA_HMAC_MAX_LEN);
        if (hmac == NULL)
                RETURN(-ENOMEM);

        rc = capa_hmac(hmac, capa, key.lk_key);
        if (rc) {
                DEBUG_CAPA(D_ERROR, capa, "HMAC failed: rc %d", rc);
                OBD_FREE(hmac, CAPA_HMAC_MAX_LEN);
                RETURN(rc);
        }

        rc = memcmp(hmac, capa->lc_hmac, CAPA_HMAC_MAX_LEN);
        OBD_FREE(hmac, CAPA_HMAC_MAX_LEN);
        if (rc) {
                DEBUG_CAPA_KEY(D_ERROR, &key, "calculate HMAC with ");
                DEBUG_CAPA(D_ERROR, capa, "HMAC mismatch");
                RETURN(-EACCES);
        }

        /* store in capa hash */
        oc = capa_add(filter->fo_capa_hash, capa);
        capa_put(oc);
        RETURN(0);
}

int filter_capa_fixoa(struct obd_export *exp, struct obdo *oa, obd_seq seq,
                      struct lustre_capa *capa)
{
        int rc = 0;
        ENTRY;

        /* skip capa check for llog and obdecho */
        if (!fid_seq_is_mdt(seq))
                RETURN(0);

        if (!(exp->exp_connect_flags & OBD_CONNECT_OSS_CAPA))
                RETURN(0);

        if (unlikely(!capa))
                RETURN(-EACCES);

        if (capa_flags(capa) == LC_ID_CONVERT) {
                struct obd_device *obd = exp->exp_obd;
                struct filter_obd *filter = &obd->u.filter;
                struct filter_capa_key *k;
                int found = 0;

                cfs_spin_lock(&capa_lock);
                cfs_list_for_each_entry(k, &filter->fo_capa_keys, k_list) {
                        if (k->k_key.lk_seq == seq &&
                            k->k_key.lk_keyid == capa_keyid(capa)) {
                                found = 1;
                                break;
                        }
                }
                cfs_spin_unlock(&capa_lock);

                if (found) {
                        union {
                                __u64 id64;
                                __u32 id32[2];
                        } uid, gid;
                        __u32 d[4], s[4];

                        uid.id64 = capa_uid(capa);
                        gid.id64 = capa_gid(capa);
                        s[0] = uid.id32[0];
                        s[1] = uid.id32[1];
                        s[2] = gid.id32[0];
                        s[3] = gid.id32[1];

                        rc = capa_decrypt_id(d, s, k->k_key.lk_key,
                                             CAPA_HMAC_KEY_MAX_LEN);
                        if (unlikely(rc))
                                RETURN(rc);

                        oa->o_uid = d[0];
                        oa->o_gid = d[2];
                } else {
                        DEBUG_CAPA(D_ERROR, capa, "no matched capability key for");
                        rc = -ESTALE;
                }
        }

        RETURN(rc);
}

void filter_free_capa_keys(struct filter_obd *filter)
{
        struct filter_capa_key *key, *n;

        cfs_spin_lock(&capa_lock);
        cfs_list_for_each_entry_safe(key, n, &filter->fo_capa_keys, k_list) {
                cfs_list_del_init(&key->k_list);
                OBD_FREE(key, sizeof(*key));
        }
        cfs_spin_unlock(&capa_lock);
}
