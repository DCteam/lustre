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
 * Copyright  2009 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Author: Nathan Rutman <nathan.rutman@sun.com>
 *
 */

/* HSM copytool example program.
 * The copytool acts on action requests from Lustre to copy files to and from
 * an HSM archive system.
 *
 * Note: under Linux, until llapi_copytool_fini is called (or the program is
 * killed), the libcfs module will be referenced and unremovable,
 * even after Lustre services stop.
 */

#include <stdio.h>
#include <getopt.h>
#include <libcfs/libcfs.h>
#include <lustre/lustre_user.h>
#include <lustre/liblustreapi.h>

int main(int argc, char **argv) {
        int c, test = 0;
        struct option long_opts[] = {
                {"test", no_argument, 0, 't'},
                {0, 0, 0, 0}
        };
        void *ctdata;
        int archive_nums[] = {1}; /* which archive numbers we care about */
        int rc;

        optind = 0;
        while ((c = getopt_long(argc, argv, "t", long_opts, NULL)) != -1) {
                switch (c) {
                case 't':
                        test++;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return EINVAL;
                }
        }

        rc = llapi_copytool_start(&ctdata, 0, ARRAY_SIZE(archive_nums),
                                  archive_nums);
        if (rc < 0) {
                fprintf(stderr, "Can't start copytool interface: %s\n",
                        strerror(-rc));
                return -rc;
        }

        if (test)
                return -llapi_copytool_fini(&ctdata);

        printf("Waiting for message from kernel (pid=%d)\n", getpid());

        while(1) {
                struct hsm_action_list *hal;
                struct hsm_action_item *hai;
                int msgsize, i = 0;

                rc = llapi_copytool_recv(ctdata, &hal, &msgsize);
                if (rc == -ESHUTDOWN) {
                        fprintf(stderr, "shutting down");
                        break;
                }
                if (rc < 0) {
                        fprintf(stderr, "Message receive: %s", strerror(-rc));
                        break;
                }
                if (msgsize == 0)
                        continue; /* msg not for us */

                printf("Copytool fs=%s archive#=%d item_count=%d\n",
                       hal->hal_fsname, hal->hal_archive_num, hal->hal_count);

                hai = hai_zero(hal);
                while (++i <= hal->hal_count) {
                        printf("Item %d: action %d reclen %d\n", i,
                               hai->hai_action, hai->hai_len);
                        printf(" "DFID" gid="LPU64" cookie="LPU64"\n",
                               PFID(&hai->hai_fid), hai->hai_gid,
                               hai->hai_cookie);
                        hai = hai_next(hai);
                }

                llapi_copytool_free(&hal);
        }

        llapi_copytool_fini(&ctdata);

        return -rc;
}


