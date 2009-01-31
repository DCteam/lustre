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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * cl code shared between vvp and liblustre (and other Lustre clients in the
 * future).
 *
 */
#include <obd_class.h>
#include <obd_support.h>
#include <obd.h>

#include <lustre_lite.h>


/* Initialize the default and maximum LOV EA and cookie sizes.  This allows
 * us to make MDS RPCs with large enough reply buffers to hold the
 * maximum-sized (= maximum striped) EA and cookie without having to
 * calculate this (via a call into the LOV + OSCs) each time we make an RPC. */
int cl_init_ea_size(struct obd_export *md_exp, struct obd_export *dt_exp)
{
        struct lov_stripe_md lsm = { .lsm_magic = LOV_MAGIC_V3 };
        __u32 valsize = sizeof(struct lov_desc);
        int rc, easize, def_easize, cookiesize;
        struct lov_desc desc;
        __u32 stripes;
        ENTRY;

        rc = obd_get_info(dt_exp, sizeof(KEY_LOVDESC), KEY_LOVDESC,
                          &valsize, &desc, NULL);
        if (rc)
                RETURN(rc);

        stripes = min(desc.ld_tgt_count, (__u32)LOV_MAX_STRIPE_COUNT);
        lsm.lsm_stripe_count = stripes;
        easize = obd_size_diskmd(dt_exp, &lsm);

        lsm.lsm_stripe_count = desc.ld_default_stripe_count;
        def_easize = obd_size_diskmd(dt_exp, &lsm);

        cookiesize = stripes * sizeof(struct llog_cookie);

        CDEBUG(D_HA, "updating max_mdsize/max_cookiesize: %d/%d\n",
               easize, cookiesize);

        rc = md_init_ea_size(md_exp, easize, def_easize, cookiesize);
        RETURN(rc);
}

/**
 * This function is used as an upcall-callback hooked by liblustre and llite
 * clients into obd_notify() listeners chain to handle notifications about
 * change of import connect_flags. See llu_fsswop_mount() and
 * lustre_common_fill_super().
 */
int cl_ocd_update(struct obd_device *host,
                  struct obd_device *watched,
                  enum obd_notify_event ev, void *owner)
{
        struct lustre_client_ocd *lco;
        struct client_obd        *cli;
        __u64 flags;
        int   result;

        ENTRY;
        if (!strcmp(watched->obd_type->typ_name, LUSTRE_OSC_NAME)) {
                cli = &watched->u.cli;
                lco = owner;
                flags = cli->cl_import->imp_connect_data.ocd_connect_flags;
                CDEBUG(D_SUPER, "Changing connect_flags: "LPX64" -> "LPX64"\n",
                       lco->lco_flags, flags);
                spin_lock(&lco->lco_lock);
                lco->lco_flags &= flags;
                /* for each osc event update ea size */
                if (lco->lco_dt_exp)
                        cl_init_ea_size(lco->lco_md_exp, lco->lco_dt_exp);

                spin_unlock(&lco->lco_lock);
                result = 0;
        } else {
                CERROR("unexpected notification from %s %s!\n",
                       watched->obd_type->typ_name,
                       watched->obd_name);
                result = -EINVAL;
        }
        RETURN(result);
}