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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/linux/linux-obdo.c
 *
 * Object Devices Class Driver
 * These are the only exported functions, they provide some generic
 * infrastructure for managing object devices
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifndef __KERNEL__
#include <liblustre.h>
#else
#include <linux/module.h>
#include <obd_class.h>
#include <lustre/lustre_idl.h>
#endif

#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/pagemap.h> /* for PAGE_CACHE_SIZE */

/*FIXME: Just copy from obdo_from_inode*/
void obdo_from_la(struct obdo *dst, struct lu_attr *la, obd_flag valid)
{
        obd_flag newvalid = 0;

        if (valid & OBD_MD_FLATIME) {
                dst->o_atime = la->la_atime;
                newvalid |= OBD_MD_FLATIME;
        }
        if (valid & OBD_MD_FLMTIME) {
                dst->o_mtime = la->la_mtime;
                newvalid |= OBD_MD_FLMTIME;
        }
        if (valid & OBD_MD_FLCTIME) {
                dst->o_ctime = la->la_ctime;
                newvalid |= OBD_MD_FLCTIME;
        }
        if (valid & OBD_MD_FLSIZE) {
                dst->o_size = la->la_size;
                newvalid |= OBD_MD_FLSIZE;
        }
        if (valid & OBD_MD_FLBLOCKS) {  /* allocation of space (x512 bytes) */
                dst->o_blocks = la->la_blocks;
                newvalid |= OBD_MD_FLBLOCKS;
        }
        if (valid & OBD_MD_FLTYPE) {
                dst->o_mode = (dst->o_mode & S_IALLUGO)|(la->la_mode & S_IFMT);
                newvalid |= OBD_MD_FLTYPE;
        }
        if (valid & OBD_MD_FLMODE) {
                dst->o_mode = (dst->o_mode & S_IFMT)|(la->la_mode & S_IALLUGO);
                newvalid |= OBD_MD_FLMODE;
        }
        if (valid & OBD_MD_FLUID) {
                dst->o_uid = la->la_uid;
                newvalid |= OBD_MD_FLUID;
        }
        if (valid & OBD_MD_FLGID) {
                dst->o_gid = la->la_gid;
                newvalid |= OBD_MD_FLGID;
        }
        dst->o_valid |= newvalid;
}
EXPORT_SYMBOL(obdo_from_la);

void obdo_refresh_inode(struct inode *dst, struct obdo *src, obd_flag valid)
{
        valid &= src->o_valid;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE,
                       "valid "LPX64", cur time %lu/%lu, new "LPU64"/"LPU64"\n",
                       src->o_valid, LTIME_S(dst->i_mtime),
                       LTIME_S(dst->i_ctime), src->o_mtime, src->o_ctime);

        if (valid & OBD_MD_FLATIME && src->o_atime > LTIME_S(dst->i_atime))
                LTIME_S(dst->i_atime) = src->o_atime;
        if (valid & OBD_MD_FLMTIME && src->o_mtime > LTIME_S(dst->i_mtime))
                LTIME_S(dst->i_mtime) = src->o_mtime;
        if (valid & OBD_MD_FLCTIME && src->o_ctime > LTIME_S(dst->i_ctime))
                LTIME_S(dst->i_ctime) = src->o_ctime;
        if (valid & OBD_MD_FLSIZE)
                i_size_write(dst, src->o_size);
        /* optimum IO size */
        if (valid & OBD_MD_FLBLKSZ && src->o_blksize > (1 << dst->i_blkbits)) {
                dst->i_blkbits = cfs_ffs(src->o_blksize) - 1;
#ifdef HAVE_INODE_BLKSIZE
                dst->i_blksize = src->o_blksize;
#endif
        }

        if (dst->i_blkbits < CFS_PAGE_SHIFT) {
#ifdef HAVE_INODE_BLKSIZE
                dst->i_blksize = CFS_PAGE_SIZE;
#endif
                dst->i_blkbits = CFS_PAGE_SHIFT;
        }

        /* allocation of space */
        if (valid & OBD_MD_FLBLOCKS && src->o_blocks > dst->i_blocks)
                /*
                 * XXX shouldn't overflow be checked here like in
                 * obdo_to_inode().
                 */
                dst->i_blocks = src->o_blocks;
}
EXPORT_SYMBOL(obdo_refresh_inode);

void obdo_to_inode(struct inode *dst, struct obdo *src, obd_flag valid)
{
        valid &= src->o_valid;

        LASSERTF(!(valid & (OBD_MD_FLTYPE | OBD_MD_FLGENER | OBD_MD_FLFID |
                            OBD_MD_FLID | OBD_MD_FLGROUP)),
                 "object "LPU64"/"LPU64", valid %x\n",
                 src->o_id, src->o_seq, valid);

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE,
                       "valid "LPX64", cur time %lu/%lu, new "LPU64"/"LPU64"\n",
                       src->o_valid, LTIME_S(dst->i_mtime),
                       LTIME_S(dst->i_ctime), src->o_mtime, src->o_ctime);

        if (valid & OBD_MD_FLATIME)
                LTIME_S(dst->i_atime) = src->o_atime;
        if (valid & OBD_MD_FLMTIME)
                LTIME_S(dst->i_mtime) = src->o_mtime;
        if (valid & OBD_MD_FLCTIME && src->o_ctime > LTIME_S(dst->i_ctime))
                LTIME_S(dst->i_ctime) = src->o_ctime;
        if (valid & OBD_MD_FLSIZE)
                i_size_write(dst, src->o_size);
        if (valid & OBD_MD_FLBLOCKS) { /* allocation of space */
                dst->i_blocks = src->o_blocks;
                if (dst->i_blocks < src->o_blocks) /* overflow */
                        dst->i_blocks = -1;

        }
        if (valid & OBD_MD_FLBLKSZ) {
                dst->i_blkbits = cfs_ffs(src->o_blksize)-1;
#ifdef HAVE_INODE_BLKSIZE
                dst->i_blksize = src->o_blksize;
#endif
        }
        if (valid & OBD_MD_FLMODE)
                dst->i_mode = (dst->i_mode & S_IFMT) | (src->o_mode & ~S_IFMT);
        if (valid & OBD_MD_FLUID)
                dst->i_uid = src->o_uid;
        if (valid & OBD_MD_FLGID)
                dst->i_gid = src->o_gid;
        if (valid & OBD_MD_FLFLAGS)
                dst->i_flags = src->o_flags;
}
EXPORT_SYMBOL(obdo_to_inode);
#endif
