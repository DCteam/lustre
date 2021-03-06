/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 *   Author: Zach Brown <zab@zabbo.net>
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 *   This file is part of Portals, http://www.sf.net/projects/sandiaportals/
 *
 *   Portals is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Portals is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Portals; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "socklnd.h"

ksock_tx_t *
ksocknal_alloc_tx(int type, int size)
{
        ksock_tx_t *tx = NULL;

        if (type == KSOCK_MSG_NOOP) {
                LASSERT (size == KSOCK_NOOP_TX_SIZE);

                /* searching for a noop tx in free list */
                cfs_spin_lock(&ksocknal_data.ksnd_tx_lock);

                if (!cfs_list_empty(&ksocknal_data.ksnd_idle_noop_txs)) {
                        tx = cfs_list_entry(ksocknal_data.ksnd_idle_noop_txs. \
                                            next, ksock_tx_t, tx_list);
                        LASSERT(tx->tx_desc_size == size);
                        cfs_list_del(&tx->tx_list);
                }

                cfs_spin_unlock(&ksocknal_data.ksnd_tx_lock);
        }

        if (tx == NULL)
                LIBCFS_ALLOC(tx, size);

        if (tx == NULL)
                return NULL;

        cfs_atomic_set(&tx->tx_refcount, 1);
        tx->tx_zc_capable = 0;
        tx->tx_zc_checked = 0;
        tx->tx_desc_size  = size;

        cfs_atomic_inc(&ksocknal_data.ksnd_nactive_txs);

        return tx;
}

ksock_tx_t *
ksocknal_alloc_tx_noop(__u64 cookie, int nonblk)
{
        ksock_tx_t *tx;

        tx = ksocknal_alloc_tx(KSOCK_MSG_NOOP, KSOCK_NOOP_TX_SIZE);
        if (tx == NULL) {
                CERROR("Can't allocate noop tx desc\n");
                return NULL;
        }

        tx->tx_conn     = NULL;
        tx->tx_lnetmsg  = NULL;
        tx->tx_kiov     = NULL;
        tx->tx_nkiov    = 0;
        tx->tx_iov      = tx->tx_frags.virt.iov;
        tx->tx_niov     = 1;
        tx->tx_nonblk   = nonblk;

        socklnd_init_msg(&tx->tx_msg, KSOCK_MSG_NOOP);
        tx->tx_msg.ksm_zc_cookies[1] = cookie;

        return tx;
}


void
ksocknal_free_tx (ksock_tx_t *tx)
{
        cfs_atomic_dec(&ksocknal_data.ksnd_nactive_txs);

        if (tx->tx_lnetmsg == NULL && tx->tx_desc_size == KSOCK_NOOP_TX_SIZE) {
                /* it's a noop tx */
                cfs_spin_lock(&ksocknal_data.ksnd_tx_lock);

                cfs_list_add(&tx->tx_list, &ksocknal_data.ksnd_idle_noop_txs);

                cfs_spin_unlock(&ksocknal_data.ksnd_tx_lock);
        } else {
                LIBCFS_FREE(tx, tx->tx_desc_size);
        }
}

int
ksocknal_send_iov (ksock_conn_t *conn, ksock_tx_t *tx)
{
        struct iovec  *iov = tx->tx_iov;
        int    nob;
        int    rc;

        LASSERT (tx->tx_niov > 0);

        /* Never touch tx->tx_iov inside ksocknal_lib_send_iov() */
        rc = ksocknal_lib_send_iov(conn, tx);

        if (rc <= 0)                            /* sent nothing? */
                return (rc);

        nob = rc;
        LASSERT (nob <= tx->tx_resid);
        tx->tx_resid -= nob;

        /* "consume" iov */
        do {
                LASSERT (tx->tx_niov > 0);

                if (nob < (int) iov->iov_len) {
                        iov->iov_base = (void *)((char *)iov->iov_base + nob);
                        iov->iov_len -= nob;
                        return (rc);
                }

                nob -= iov->iov_len;
                tx->tx_iov = ++iov;
                tx->tx_niov--;
        } while (nob != 0);

        return (rc);
}

int
ksocknal_send_kiov (ksock_conn_t *conn, ksock_tx_t *tx)
{
        lnet_kiov_t    *kiov = tx->tx_kiov;
        int     nob;
        int     rc;

        LASSERT (tx->tx_niov == 0);
        LASSERT (tx->tx_nkiov > 0);

        /* Never touch tx->tx_kiov inside ksocknal_lib_send_kiov() */
        rc = ksocknal_lib_send_kiov(conn, tx);

        if (rc <= 0)                            /* sent nothing? */
                return (rc);

        nob = rc;
        LASSERT (nob <= tx->tx_resid);
        tx->tx_resid -= nob;

        /* "consume" kiov */
        do {
                LASSERT(tx->tx_nkiov > 0);

                if (nob < (int)kiov->kiov_len) {
                        kiov->kiov_offset += nob;
                        kiov->kiov_len -= nob;
                        return rc;
                }

                nob -= (int)kiov->kiov_len;
                tx->tx_kiov = ++kiov;
                tx->tx_nkiov--;
        } while (nob != 0);

        return (rc);
}

int
ksocknal_transmit (ksock_conn_t *conn, ksock_tx_t *tx)
{
        int      rc;
        int      bufnob;

        if (ksocknal_data.ksnd_stall_tx != 0) {
                cfs_pause(cfs_time_seconds(ksocknal_data.ksnd_stall_tx));
        }

        LASSERT (tx->tx_resid != 0);

        rc = ksocknal_connsock_addref(conn);
        if (rc != 0) {
                LASSERT (conn->ksnc_closing);
                return (-ESHUTDOWN);
        }

        do {
                if (ksocknal_data.ksnd_enomem_tx > 0) {
                        /* testing... */
                        ksocknal_data.ksnd_enomem_tx--;
                        rc = -EAGAIN;
                } else if (tx->tx_niov != 0) {
                        rc = ksocknal_send_iov (conn, tx);
                } else {
                        rc = ksocknal_send_kiov (conn, tx);
                }

                bufnob = libcfs_sock_wmem_queued(conn->ksnc_sock);
                if (rc > 0)                     /* sent something? */
                        conn->ksnc_tx_bufnob += rc; /* account it */

                if (bufnob < conn->ksnc_tx_bufnob) {
                        /* allocated send buffer bytes < computed; infer
                         * something got ACKed */
                        conn->ksnc_tx_deadline =
                                cfs_time_shift(*ksocknal_tunables.ksnd_timeout);
                        conn->ksnc_peer->ksnp_last_alive = cfs_time_current();
                        conn->ksnc_tx_bufnob = bufnob;
                        cfs_mb();
                }

                if (rc <= 0) { /* Didn't write anything? */

                        if (rc == 0) /* some stacks return 0 instead of -EAGAIN */
                                rc = -EAGAIN;

                        /* Check if EAGAIN is due to memory pressure */
                        if(rc == -EAGAIN && ksocknal_lib_memory_pressure(conn))
                                rc = -ENOMEM;

                        break;
                }

                /* socket's wmem_queued now includes 'rc' bytes */
                cfs_atomic_sub (rc, &conn->ksnc_tx_nob);
                rc = 0;

        } while (tx->tx_resid != 0);

        ksocknal_connsock_decref(conn);
        return (rc);
}

int
ksocknal_recv_iov (ksock_conn_t *conn)
{
        struct iovec *iov = conn->ksnc_rx_iov;
        int     nob;
        int     rc;

        LASSERT (conn->ksnc_rx_niov > 0);

        /* Never touch conn->ksnc_rx_iov or change connection 
         * status inside ksocknal_lib_recv_iov */
        rc = ksocknal_lib_recv_iov(conn);

        if (rc <= 0)
                return (rc);

        /* received something... */
        nob = rc;

        conn->ksnc_peer->ksnp_last_alive = cfs_time_current();
        conn->ksnc_rx_deadline =
                cfs_time_shift(*ksocknal_tunables.ksnd_timeout);
        cfs_mb();                       /* order with setting rx_started */
        conn->ksnc_rx_started = 1;

        conn->ksnc_rx_nob_wanted -= nob;
        conn->ksnc_rx_nob_left -= nob;

        do {
                LASSERT (conn->ksnc_rx_niov > 0);

                if (nob < (int)iov->iov_len) {
                        iov->iov_len -= nob;
                        iov->iov_base = (void *)((char *)iov->iov_base + nob);
                        return (-EAGAIN);
                }

                nob -= iov->iov_len;
                conn->ksnc_rx_iov = ++iov;
                conn->ksnc_rx_niov--;
        } while (nob != 0);

        return (rc);
}

int
ksocknal_recv_kiov (ksock_conn_t *conn)
{
        lnet_kiov_t   *kiov = conn->ksnc_rx_kiov;
        int     nob;
        int     rc;
        LASSERT (conn->ksnc_rx_nkiov > 0);

        /* Never touch conn->ksnc_rx_kiov or change connection 
         * status inside ksocknal_lib_recv_iov */
        rc = ksocknal_lib_recv_kiov(conn);

        if (rc <= 0)
                return (rc);

        /* received something... */
        nob = rc;

        conn->ksnc_peer->ksnp_last_alive = cfs_time_current();
        conn->ksnc_rx_deadline =
                cfs_time_shift(*ksocknal_tunables.ksnd_timeout);
        cfs_mb();                       /* order with setting rx_started */
        conn->ksnc_rx_started = 1;

        conn->ksnc_rx_nob_wanted -= nob;
        conn->ksnc_rx_nob_left -= nob;

        do {
                LASSERT (conn->ksnc_rx_nkiov > 0);

                if (nob < (int) kiov->kiov_len) {
                        kiov->kiov_offset += nob;
                        kiov->kiov_len -= nob;
                        return -EAGAIN;
                }

                nob -= kiov->kiov_len;
                conn->ksnc_rx_kiov = ++kiov;
                conn->ksnc_rx_nkiov--;
        } while (nob != 0);

        return 1;
}

int
ksocknal_receive (ksock_conn_t *conn)
{
        /* Return 1 on success, 0 on EOF, < 0 on error.
         * Caller checks ksnc_rx_nob_wanted to determine
         * progress/completion. */
        int     rc;
        ENTRY;

        if (ksocknal_data.ksnd_stall_rx != 0) {
                cfs_pause(cfs_time_seconds (ksocknal_data.ksnd_stall_rx));
        }

        rc = ksocknal_connsock_addref(conn);
        if (rc != 0) {
                LASSERT (conn->ksnc_closing);
                return (-ESHUTDOWN);
        }

        for (;;) {
                if (conn->ksnc_rx_niov != 0)
                        rc = ksocknal_recv_iov (conn);
                else
                        rc = ksocknal_recv_kiov (conn);

                if (rc <= 0) {
                        /* error/EOF or partial receive */
                        if (rc == -EAGAIN) {
                                rc = 1;
                        } else if (rc == 0 && conn->ksnc_rx_started) {
                                /* EOF in the middle of a message */
                                rc = -EPROTO;
                        }
                        break;
                }

                /* Completed a fragment */

                if (conn->ksnc_rx_nob_wanted == 0) {
                        rc = 1;
                        break;
                }
        }

        ksocknal_connsock_decref(conn);
        RETURN (rc);
}

void
ksocknal_tx_done (lnet_ni_t *ni, ksock_tx_t *tx)
{
        lnet_msg_t  *lnetmsg = tx->tx_lnetmsg;
        int          rc = (tx->tx_resid == 0) ? 0 : -EIO;
        ENTRY;

        LASSERT(ni != NULL || tx->tx_conn != NULL);

        if (tx->tx_conn != NULL)
                ksocknal_conn_decref(tx->tx_conn);

        if (ni == NULL && tx->tx_conn != NULL)
                ni = tx->tx_conn->ksnc_peer->ksnp_ni;

        ksocknal_free_tx (tx);
        if (lnetmsg != NULL) /* KSOCK_MSG_NOOP go without lnetmsg */
                lnet_finalize (ni, lnetmsg, rc);

        EXIT;
}

void
ksocknal_txlist_done (lnet_ni_t *ni, cfs_list_t *txlist, int error)
{
        ksock_tx_t *tx;

        while (!cfs_list_empty (txlist)) {
                tx = cfs_list_entry (txlist->next, ksock_tx_t, tx_list);

                if (error && tx->tx_lnetmsg != NULL) {
                        CDEBUG (D_NETERROR, "Deleting packet type %d len %d %s->%s\n",
                                le32_to_cpu (tx->tx_lnetmsg->msg_hdr.type),
                                le32_to_cpu (tx->tx_lnetmsg->msg_hdr.payload_length),
                                libcfs_nid2str(le64_to_cpu(tx->tx_lnetmsg->msg_hdr.src_nid)),
                                libcfs_nid2str(le64_to_cpu(tx->tx_lnetmsg->msg_hdr.dest_nid)));
                } else if (error) {
                        CDEBUG (D_NETERROR, "Deleting noop packet\n");
                }

                cfs_list_del (&tx->tx_list);

                LASSERT (cfs_atomic_read(&tx->tx_refcount) == 1);
                ksocknal_tx_done (ni, tx);
        }
}

static void
ksocknal_check_zc_req(ksock_tx_t *tx)
{
        ksock_conn_t   *conn = tx->tx_conn;
        ksock_peer_t   *peer = conn->ksnc_peer;

        /* Set tx_msg.ksm_zc_cookies[0] to a unique non-zero cookie and add tx
         * to ksnp_zc_req_list if some fragment of this message should be sent
         * zero-copy.  Our peer will send an ACK containing this cookie when
         * she has received this message to tell us we can signal completion.
         * tx_msg.ksm_zc_cookies[0] remains non-zero while tx is on
         * ksnp_zc_req_list. */
        LASSERT (tx->tx_msg.ksm_type != KSOCK_MSG_NOOP);
        LASSERT (tx->tx_zc_capable);

        tx->tx_zc_checked = 1;

        if (conn->ksnc_proto == &ksocknal_protocol_v1x ||
            !conn->ksnc_zc_capable)
                return;

        /* assign cookie and queue tx to pending list, it will be released when
         * a matching ack is received. See ksocknal_handle_zcack() */

        ksocknal_tx_addref(tx);

        cfs_spin_lock(&peer->ksnp_lock);

        /* ZC_REQ is going to be pinned to the peer */
        tx->tx_deadline =
                cfs_time_shift(*ksocknal_tunables.ksnd_timeout);

        LASSERT (tx->tx_msg.ksm_zc_cookies[0] == 0);

        tx->tx_msg.ksm_zc_cookies[0] = peer->ksnp_zc_next_cookie++;

        if (peer->ksnp_zc_next_cookie == 0)
                peer->ksnp_zc_next_cookie = SOCKNAL_KEEPALIVE_PING + 1;

        cfs_list_add_tail(&tx->tx_zc_list, &peer->ksnp_zc_req_list);

        cfs_spin_unlock(&peer->ksnp_lock);
}

static void
ksocknal_uncheck_zc_req(ksock_tx_t *tx)
{
        ksock_peer_t   *peer = tx->tx_conn->ksnc_peer;

        LASSERT (tx->tx_msg.ksm_type != KSOCK_MSG_NOOP);
        LASSERT (tx->tx_zc_capable);

        tx->tx_zc_checked = 0;

        cfs_spin_lock(&peer->ksnp_lock);

        if (tx->tx_msg.ksm_zc_cookies[0] == 0) {
                /* Not waiting for an ACK */
                cfs_spin_unlock(&peer->ksnp_lock);
                return;
        }

        tx->tx_msg.ksm_zc_cookies[0] = 0;
        cfs_list_del(&tx->tx_zc_list);

        cfs_spin_unlock(&peer->ksnp_lock);

        ksocknal_tx_decref(tx);
}

int
ksocknal_process_transmit (ksock_conn_t *conn, ksock_tx_t *tx)
{
        int            rc;

        if (tx->tx_zc_capable && !tx->tx_zc_checked)
                ksocknal_check_zc_req(tx);

        rc = ksocknal_transmit (conn, tx);

        CDEBUG (D_NET, "send(%d) %d\n", tx->tx_resid, rc);

        if (tx->tx_resid == 0) {
                /* Sent everything OK */
                LASSERT (rc == 0);

                return (0);
        }

        if (rc == -EAGAIN)
                return (rc);

        if (rc == -ENOMEM) {
                static int counter;

                counter++;   /* exponential backoff warnings */
                if ((counter & (-counter)) == counter)
                        CWARN("%u ENOMEM tx %p (%u allocated)\n",
                              counter, conn, cfs_atomic_read(&libcfs_kmemory));

                /* Queue on ksnd_enomem_conns for retry after a timeout */
                cfs_spin_lock_bh (&ksocknal_data.ksnd_reaper_lock);

                /* enomem list takes over scheduler's ref... */
                LASSERT (conn->ksnc_tx_scheduled);
                cfs_list_add_tail(&conn->ksnc_tx_list,
                                  &ksocknal_data.ksnd_enomem_conns);
                if (!cfs_time_aftereq(cfs_time_add(cfs_time_current(),
                                                   SOCKNAL_ENOMEM_RETRY),
                                   ksocknal_data.ksnd_reaper_waketime))
                        cfs_waitq_signal (&ksocknal_data.ksnd_reaper_waitq);

                cfs_spin_unlock_bh (&ksocknal_data.ksnd_reaper_lock);
                return (rc);
        }

        /* Actual error */
        LASSERT (rc < 0);

        if (!conn->ksnc_closing) {
                switch (rc) {
                case -ECONNRESET:
                        LCONSOLE_WARN("Host %u.%u.%u.%u reset our connection "
                                      "while we were sending data; it may have "
                                      "rebooted.\n",
                                      HIPQUAD(conn->ksnc_ipaddr));
                        break;
                default:
                        LCONSOLE_WARN("There was an unexpected network error "
                                      "while writing to %u.%u.%u.%u: %d.\n",
                                      HIPQUAD(conn->ksnc_ipaddr), rc);
                        break;
                }
                CDEBUG(D_NET, "[%p] Error %d on write to %s"
                       " ip %d.%d.%d.%d:%d\n", conn, rc,
                       libcfs_id2str(conn->ksnc_peer->ksnp_id),
                       HIPQUAD(conn->ksnc_ipaddr),
                       conn->ksnc_port);
        }

        if (tx->tx_zc_checked)
                ksocknal_uncheck_zc_req(tx);

        /* it's not an error if conn is being closed */
        ksocknal_close_conn_and_siblings (conn,
                                          (conn->ksnc_closing) ? 0 : rc);

        return (rc);
}

void
ksocknal_launch_connection_locked (ksock_route_t *route)
{

        /* called holding write lock on ksnd_global_lock */

        LASSERT (!route->ksnr_scheduled);
        LASSERT (!route->ksnr_connecting);
        LASSERT ((ksocknal_route_mask() & ~route->ksnr_connected) != 0);

        route->ksnr_scheduled = 1;              /* scheduling conn for connd */
        ksocknal_route_addref(route);           /* extra ref for connd */

        cfs_spin_lock_bh (&ksocknal_data.ksnd_connd_lock);

        cfs_list_add_tail (&route->ksnr_connd_list,
                           &ksocknal_data.ksnd_connd_routes);
        cfs_waitq_signal (&ksocknal_data.ksnd_connd_waitq);

        cfs_spin_unlock_bh (&ksocknal_data.ksnd_connd_lock);
}

void
ksocknal_launch_all_connections_locked (ksock_peer_t *peer)
{
        ksock_route_t *route;

        /* called holding write lock on ksnd_global_lock */
        for (;;) {
                /* launch any/all connections that need it */
                route = ksocknal_find_connectable_route_locked(peer);
                if (route == NULL)
                        return;

                ksocknal_launch_connection_locked(route);
        }
}

ksock_conn_t *
ksocknal_find_conn_locked(ksock_peer_t *peer, ksock_tx_t *tx, int nonblk)
{
        cfs_list_t       *tmp;
        ksock_conn_t     *conn;
        ksock_conn_t     *typed = NULL;
        ksock_conn_t     *fallback = NULL;
        int               tnob     = 0;
        int               fnob     = 0;

        cfs_list_for_each (tmp, &peer->ksnp_conns) {
                ksock_conn_t *c  = cfs_list_entry(tmp, ksock_conn_t, ksnc_list);
                int           nob = cfs_atomic_read(&c->ksnc_tx_nob) +
                                    libcfs_sock_wmem_queued(c->ksnc_sock);
                int           rc;

                LASSERT (!c->ksnc_closing);
                LASSERT (c->ksnc_proto != NULL &&
                         c->ksnc_proto->pro_match_tx != NULL);

                rc = c->ksnc_proto->pro_match_tx(c, tx, nonblk);

                switch (rc) {
                default:
                        LBUG();
                case SOCKNAL_MATCH_NO: /* protocol rejected the tx */
                        continue;

                case SOCKNAL_MATCH_YES: /* typed connection */
                        if (typed == NULL || tnob > nob ||
                            (tnob == nob && *ksocknal_tunables.ksnd_round_robin &&
                             cfs_time_after(typed->ksnc_tx_last_post, c->ksnc_tx_last_post))) {
                                typed = c;
                                tnob  = nob;
                        }
                        break;

                case SOCKNAL_MATCH_MAY: /* fallback connection */
                        if (fallback == NULL || fnob > nob ||
                            (fnob == nob && *ksocknal_tunables.ksnd_round_robin &&
                             cfs_time_after(fallback->ksnc_tx_last_post, c->ksnc_tx_last_post))) {
                                fallback = c;
                                fnob     = nob;
                        }
                        break;
                }
        }

        /* prefer the typed selection */
        conn = (typed != NULL) ? typed : fallback;

        if (conn != NULL)
                conn->ksnc_tx_last_post = cfs_time_current();

        return conn;
}

void
ksocknal_tx_prep(ksock_conn_t *conn, ksock_tx_t *tx)
{
        conn->ksnc_proto->pro_pack(tx);

        cfs_atomic_add (tx->tx_nob, &conn->ksnc_tx_nob);
        ksocknal_conn_addref(conn); /* +1 ref for tx */
        tx->tx_conn = conn;
}

void
ksocknal_queue_tx_locked (ksock_tx_t *tx, ksock_conn_t *conn)
{
        ksock_sched_t *sched = conn->ksnc_scheduler;
        ksock_msg_t   *msg = &tx->tx_msg;
        ksock_tx_t    *ztx = NULL;
        int            bufnob = 0;

        /* called holding global lock (read or irq-write) and caller may
         * not have dropped this lock between finding conn and calling me,
         * so we don't need the {get,put}connsock dance to deref
         * ksnc_sock... */
        LASSERT(!conn->ksnc_closing);

        CDEBUG (D_NET, "Sending to %s ip %d.%d.%d.%d:%d\n", 
                libcfs_id2str(conn->ksnc_peer->ksnp_id),
                HIPQUAD(conn->ksnc_ipaddr),
                conn->ksnc_port);

        ksocknal_tx_prep(conn, tx);

        /* Ensure the frags we've been given EXACTLY match the number of
         * bytes we want to send.  Many TCP/IP stacks disregard any total
         * size parameters passed to them and just look at the frags. 
         *
         * We always expect at least 1 mapped fragment containing the
         * complete ksocknal message header. */
        LASSERT (lnet_iov_nob (tx->tx_niov, tx->tx_iov) +
                 lnet_kiov_nob(tx->tx_nkiov, tx->tx_kiov) ==
                 (unsigned int)tx->tx_nob);
        LASSERT (tx->tx_niov >= 1);
        LASSERT (tx->tx_resid == tx->tx_nob);

        CDEBUG (D_NET, "Packet %p type %d, nob %d niov %d nkiov %d\n",
                tx, (tx->tx_lnetmsg != NULL) ? tx->tx_lnetmsg->msg_hdr.type:
                                               KSOCK_MSG_NOOP,
                tx->tx_nob, tx->tx_niov, tx->tx_nkiov);

        /*
         * FIXME: SOCK_WMEM_QUEUED and SOCK_ERROR could block in __DARWIN8__
         * but they're used inside spinlocks a lot.
         */
        bufnob = libcfs_sock_wmem_queued(conn->ksnc_sock);
        cfs_spin_lock_bh (&sched->kss_lock);

        if (cfs_list_empty(&conn->ksnc_tx_queue) && bufnob == 0) {
                /* First packet starts the timeout */
                conn->ksnc_tx_deadline =
                        cfs_time_shift(*ksocknal_tunables.ksnd_timeout);
                if (conn->ksnc_tx_bufnob > 0) /* something got ACKed */
                        conn->ksnc_peer->ksnp_last_alive = cfs_time_current();
                conn->ksnc_tx_bufnob = 0;
                cfs_mb(); /* order with adding to tx_queue */
        }

        if (msg->ksm_type == KSOCK_MSG_NOOP) {
                /* The packet is noop ZC ACK, try to piggyback the ack_cookie
                 * on a normal packet so I don't need to send it */
                LASSERT (msg->ksm_zc_cookies[1] != 0);
                LASSERT (conn->ksnc_proto->pro_queue_tx_zcack != NULL);

                if (conn->ksnc_proto->pro_queue_tx_zcack(conn, tx, 0))
                        ztx = tx; /* ZC ACK piggybacked on ztx release tx later */

        } else {
                /* It's a normal packet - can it piggback a noop zc-ack that
                 * has been queued already? */
                LASSERT (msg->ksm_zc_cookies[1] == 0);
                LASSERT (conn->ksnc_proto->pro_queue_tx_msg != NULL);

                ztx = conn->ksnc_proto->pro_queue_tx_msg(conn, tx);
                /* ztx will be released later */
        }

        if (ztx != NULL) {
                cfs_atomic_sub (ztx->tx_nob, &conn->ksnc_tx_nob);
                cfs_list_add_tail(&ztx->tx_list, &sched->kss_zombie_noop_txs);
        }

        if (conn->ksnc_tx_ready &&      /* able to send */
            !conn->ksnc_tx_scheduled) { /* not scheduled to send */
                /* +1 ref for scheduler */
                ksocknal_conn_addref(conn);
                cfs_list_add_tail (&conn->ksnc_tx_list,
                                   &sched->kss_tx_conns);
                conn->ksnc_tx_scheduled = 1;
                cfs_waitq_signal (&sched->kss_waitq);
        }

        cfs_spin_unlock_bh (&sched->kss_lock);
}


ksock_route_t *
ksocknal_find_connectable_route_locked (ksock_peer_t *peer)
{
        cfs_time_t     now = cfs_time_current();
        cfs_list_t    *tmp;
        ksock_route_t *route;

        cfs_list_for_each (tmp, &peer->ksnp_routes) {
                route = cfs_list_entry (tmp, ksock_route_t, ksnr_list);

                LASSERT (!route->ksnr_connecting || route->ksnr_scheduled);

                if (route->ksnr_scheduled)      /* connections being established */
                        continue;

                /* all route types connected ? */
                if ((ksocknal_route_mask() & ~route->ksnr_connected) == 0)
                        continue;

                if (!(route->ksnr_retry_interval == 0 || /* first attempt */
                      cfs_time_aftereq(now, route->ksnr_timeout))) {
                        CDEBUG(D_NET,
                               "Too soon to retry route %u.%u.%u.%u "
                               "(cnted %d, interval %ld, %ld secs later)\n",
                               HIPQUAD(route->ksnr_ipaddr),
                               route->ksnr_connected,
                               route->ksnr_retry_interval,
                               cfs_duration_sec(route->ksnr_timeout - now));
                        continue;
                }

                return (route);
        }

        return (NULL);
}

ksock_route_t *
ksocknal_find_connecting_route_locked (ksock_peer_t *peer)
{
        cfs_list_t        *tmp;
        ksock_route_t     *route;

        cfs_list_for_each (tmp, &peer->ksnp_routes) {
                route = cfs_list_entry (tmp, ksock_route_t, ksnr_list);

                LASSERT (!route->ksnr_connecting || route->ksnr_scheduled);

                if (route->ksnr_scheduled)
                        return (route);
        }

        return (NULL);
}

int
ksocknal_launch_packet (lnet_ni_t *ni, ksock_tx_t *tx, lnet_process_id_t id)
{
        ksock_peer_t     *peer;
        ksock_conn_t     *conn;
        cfs_rwlock_t     *g_lock;
        int               retry;
        int               rc;

        LASSERT (tx->tx_conn == NULL);

        g_lock = &ksocknal_data.ksnd_global_lock;

        for (retry = 0;; retry = 1) {
                cfs_read_lock (g_lock);
                peer = ksocknal_find_peer_locked(ni, id);
                if (peer != NULL) {
                        if (ksocknal_find_connectable_route_locked(peer) == NULL) {
                                conn = ksocknal_find_conn_locked(peer, tx, tx->tx_nonblk);
                                if (conn != NULL) {
                                        /* I've got no routes that need to be
                                         * connecting and I do have an actual
                                         * connection... */
                                        ksocknal_queue_tx_locked (tx, conn);
                                        cfs_read_unlock (g_lock);
                                        return (0);
                                }
                        }
                }

                /* I'll need a write lock... */
                cfs_read_unlock (g_lock);

                cfs_write_lock_bh (g_lock);

                peer = ksocknal_find_peer_locked(ni, id);
                if (peer != NULL)
                        break;

                cfs_write_unlock_bh (g_lock);

                if ((id.pid & LNET_PID_USERFLAG) != 0) {
                        CERROR("Refusing to create a connection to "
                               "userspace process %s\n", libcfs_id2str(id));
                        return -EHOSTUNREACH;
                }

                if (retry) {
                        CERROR("Can't find peer %s\n", libcfs_id2str(id));
                        return -EHOSTUNREACH;
                }

                rc = ksocknal_add_peer(ni, id,
                                       LNET_NIDADDR(id.nid),
                                       lnet_acceptor_port());
                if (rc != 0) {
                        CERROR("Can't add peer %s: %d\n",
                               libcfs_id2str(id), rc);
                        return rc;
                }
        }

        ksocknal_launch_all_connections_locked(peer);

        conn = ksocknal_find_conn_locked(peer, tx, tx->tx_nonblk);
        if (conn != NULL) {
                /* Connection exists; queue message on it */
                ksocknal_queue_tx_locked (tx, conn);
                cfs_write_unlock_bh (g_lock);
                return (0);
        }

        if (peer->ksnp_accepting > 0 ||
            ksocknal_find_connecting_route_locked (peer) != NULL) {
                /* the message is going to be pinned to the peer */
                tx->tx_deadline =
                        cfs_time_shift(*ksocknal_tunables.ksnd_timeout);

                /* Queue the message until a connection is established */
                cfs_list_add_tail (&tx->tx_list, &peer->ksnp_tx_queue);
                cfs_write_unlock_bh (g_lock);
                return 0;
        }

        cfs_write_unlock_bh (g_lock);

        /* NB Routes may be ignored if connections to them failed recently */
        CDEBUG(D_NETERROR, "No usable routes to %s\n", libcfs_id2str(id));
        return (-EHOSTUNREACH);
}

int
ksocknal_send(lnet_ni_t *ni, void *private, lnet_msg_t *lntmsg)
{
        int               mpflag = 0;
        int               type = lntmsg->msg_type;
        lnet_process_id_t target = lntmsg->msg_target;
        unsigned int      payload_niov = lntmsg->msg_niov;
        struct iovec     *payload_iov = lntmsg->msg_iov;
        lnet_kiov_t      *payload_kiov = lntmsg->msg_kiov;
        unsigned int      payload_offset = lntmsg->msg_offset;
        unsigned int      payload_nob = lntmsg->msg_len;
        ksock_tx_t       *tx;
        int               desc_size;
        int               rc;

        /* NB 'private' is different depending on what we're sending.
         * Just ignore it... */

        CDEBUG(D_NET, "sending %u bytes in %d frags to %s\n",
               payload_nob, payload_niov, libcfs_id2str(target));

        LASSERT (payload_nob == 0 || payload_niov > 0);
        LASSERT (payload_niov <= LNET_MAX_IOV);
        /* payload is either all vaddrs or all pages */
        LASSERT (!(payload_kiov != NULL && payload_iov != NULL));
        LASSERT (!cfs_in_interrupt ());

        if (payload_iov != NULL)
                desc_size = offsetof(ksock_tx_t,
                                     tx_frags.virt.iov[1 + payload_niov]);
        else
                desc_size = offsetof(ksock_tx_t,
                                     tx_frags.paged.kiov[payload_niov]);

        if (lntmsg->msg_vmflush)
                mpflag = cfs_memory_pressure_get_and_set();
        tx = ksocknal_alloc_tx(KSOCK_MSG_LNET, desc_size);
        if (tx == NULL) {
                CERROR("Can't allocate tx desc type %d size %d\n",
                       type, desc_size);
                if (lntmsg->msg_vmflush)
                        cfs_memory_pressure_restore(mpflag);
                return (-ENOMEM);
        }

        tx->tx_conn = NULL;                     /* set when assigned a conn */
        tx->tx_lnetmsg = lntmsg;

        if (payload_iov != NULL) {
                tx->tx_kiov = NULL;
                tx->tx_nkiov = 0;
                tx->tx_iov = tx->tx_frags.virt.iov;
                tx->tx_niov = 1 +
                              lnet_extract_iov(payload_niov, &tx->tx_iov[1],
                                               payload_niov, payload_iov,
                                               payload_offset, payload_nob);
        } else {
                tx->tx_niov = 1;
                tx->tx_iov = &tx->tx_frags.paged.iov;
                tx->tx_kiov = tx->tx_frags.paged.kiov;
                tx->tx_nkiov = lnet_extract_kiov(payload_niov, tx->tx_kiov,
                                                 payload_niov, payload_kiov,
                                                 payload_offset, payload_nob);

                if (payload_nob >= *ksocknal_tunables.ksnd_zc_min_payload)
                        tx->tx_zc_capable = 1;
        }

        socklnd_init_msg(&tx->tx_msg, KSOCK_MSG_LNET);

        /* The first fragment will be set later in pro_pack */
        rc = ksocknal_launch_packet(ni, tx, target);
        if (lntmsg->msg_vmflush)
                cfs_memory_pressure_restore(mpflag);
        if (rc == 0)
                return (0);

        ksocknal_free_tx(tx);
        return (-EIO);
}

int
ksocknal_thread_start (int (*fn)(void *arg), void *arg)
{
        long          pid = cfs_kernel_thread (fn, arg, 0);

        if (pid < 0)
                return ((int)pid);

        cfs_write_lock_bh (&ksocknal_data.ksnd_global_lock);
        ksocknal_data.ksnd_nthreads++;
        cfs_write_unlock_bh (&ksocknal_data.ksnd_global_lock);
        return (0);
}

void
ksocknal_thread_fini (void)
{
        cfs_write_lock_bh (&ksocknal_data.ksnd_global_lock);
        ksocknal_data.ksnd_nthreads--;
        cfs_write_unlock_bh (&ksocknal_data.ksnd_global_lock);
}

int
ksocknal_new_packet (ksock_conn_t *conn, int nob_to_skip)
{
        static char ksocknal_slop_buffer[4096];

        int            nob;
        unsigned int   niov;
        int            skipped;

        LASSERT(conn->ksnc_proto != NULL);

        if ((*ksocknal_tunables.ksnd_eager_ack & conn->ksnc_type) != 0) {
                /* Remind the socket to ack eagerly... */
                ksocknal_lib_eager_ack(conn);
        }

        if (nob_to_skip == 0) {         /* right at next packet boundary now */
                conn->ksnc_rx_started = 0;
                cfs_mb();                       /* racing with timeout thread */

                switch (conn->ksnc_proto->pro_version) {
                case  KSOCK_PROTO_V2:
                case  KSOCK_PROTO_V3:
                        conn->ksnc_rx_state = SOCKNAL_RX_KSM_HEADER;
                        conn->ksnc_rx_iov = (struct iovec *)&conn->ksnc_rx_iov_space;
                        conn->ksnc_rx_iov[0].iov_base = (char *)&conn->ksnc_msg;

                        conn->ksnc_rx_nob_wanted = offsetof(ksock_msg_t, ksm_u);
                        conn->ksnc_rx_nob_left = offsetof(ksock_msg_t, ksm_u);
                        conn->ksnc_rx_iov[0].iov_len  = offsetof(ksock_msg_t, ksm_u);
                        break;

                case KSOCK_PROTO_V1:
                        /* Receiving bare lnet_hdr_t */
                        conn->ksnc_rx_state = SOCKNAL_RX_LNET_HEADER;
                        conn->ksnc_rx_nob_wanted = sizeof(lnet_hdr_t);
                        conn->ksnc_rx_nob_left = sizeof(lnet_hdr_t);

                        conn->ksnc_rx_iov = (struct iovec *)&conn->ksnc_rx_iov_space;
                        conn->ksnc_rx_iov[0].iov_base = (char *)&conn->ksnc_msg.ksm_u.lnetmsg;
                        conn->ksnc_rx_iov[0].iov_len  = sizeof (lnet_hdr_t);
                        break;

                default:
                        LBUG ();
                }
                conn->ksnc_rx_niov = 1;

                conn->ksnc_rx_kiov = NULL;
                conn->ksnc_rx_nkiov = 0;
                conn->ksnc_rx_csum = ~0;
                return (1);
        }

        /* Set up to skip as much as possible now.  If there's more left
         * (ran out of iov entries) we'll get called again */

        conn->ksnc_rx_state = SOCKNAL_RX_SLOP;
        conn->ksnc_rx_nob_left = nob_to_skip;
        conn->ksnc_rx_iov = (struct iovec *)&conn->ksnc_rx_iov_space;
        skipped = 0;
        niov = 0;

        do {
                nob = MIN (nob_to_skip, sizeof (ksocknal_slop_buffer));

                conn->ksnc_rx_iov[niov].iov_base = ksocknal_slop_buffer;
                conn->ksnc_rx_iov[niov].iov_len  = nob;
                niov++;
                skipped += nob;
                nob_to_skip -=nob;

        } while (nob_to_skip != 0 &&    /* mustn't overflow conn's rx iov */
                 niov < sizeof(conn->ksnc_rx_iov_space) / sizeof (struct iovec));

        conn->ksnc_rx_niov = niov;
        conn->ksnc_rx_kiov = NULL;
        conn->ksnc_rx_nkiov = 0;
        conn->ksnc_rx_nob_wanted = skipped;
        return (0);
}

int
ksocknal_process_receive (ksock_conn_t *conn)
{
        lnet_hdr_t        *lhdr;
        lnet_process_id_t *id;
        int                rc;

        LASSERT (cfs_atomic_read(&conn->ksnc_conn_refcount) > 0);

        /* NB: sched lock NOT held */
        /* SOCKNAL_RX_LNET_HEADER is here for backward compatability */
        LASSERT (conn->ksnc_rx_state == SOCKNAL_RX_KSM_HEADER ||
                 conn->ksnc_rx_state == SOCKNAL_RX_LNET_PAYLOAD ||
                 conn->ksnc_rx_state == SOCKNAL_RX_LNET_HEADER ||
                 conn->ksnc_rx_state == SOCKNAL_RX_SLOP);
 again:
        if (conn->ksnc_rx_nob_wanted != 0) {
                rc = ksocknal_receive(conn);

                if (rc <= 0) {
                        LASSERT (rc != -EAGAIN);

                        if (rc == 0)
                                CDEBUG (D_NET, "[%p] EOF from %s"
                                        " ip %d.%d.%d.%d:%d\n", conn,
                                        libcfs_id2str(conn->ksnc_peer->ksnp_id),
                                        HIPQUAD(conn->ksnc_ipaddr),
                                        conn->ksnc_port);
                        else if (!conn->ksnc_closing)
                                CERROR ("[%p] Error %d on read from %s"
                                        " ip %d.%d.%d.%d:%d\n",
                                        conn, rc,
                                        libcfs_id2str(conn->ksnc_peer->ksnp_id),
                                        HIPQUAD(conn->ksnc_ipaddr),
                                        conn->ksnc_port);

                        /* it's not an error if conn is being closed */
                        ksocknal_close_conn_and_siblings (conn,
                                                          (conn->ksnc_closing) ? 0 : rc);
                        return (rc == 0 ? -ESHUTDOWN : rc);
                }

                if (conn->ksnc_rx_nob_wanted != 0) {
                        /* short read */
                        return (-EAGAIN);
                }
        }
        switch (conn->ksnc_rx_state) {
        case SOCKNAL_RX_KSM_HEADER:
                if (conn->ksnc_flip) {
                        __swab32s(&conn->ksnc_msg.ksm_type);
                        __swab32s(&conn->ksnc_msg.ksm_csum);
                        __swab64s(&conn->ksnc_msg.ksm_zc_cookies[0]);
                        __swab64s(&conn->ksnc_msg.ksm_zc_cookies[1]);
                }

                if (conn->ksnc_msg.ksm_type != KSOCK_MSG_NOOP &&
                    conn->ksnc_msg.ksm_type != KSOCK_MSG_LNET) {
                        CERROR("%s: Unknown message type: %x\n",
                               libcfs_id2str(conn->ksnc_peer->ksnp_id),
                               conn->ksnc_msg.ksm_type);
                        ksocknal_new_packet(conn, 0);
                        ksocknal_close_conn_and_siblings(conn, -EPROTO);
                        return (-EPROTO);
                }

                if (conn->ksnc_msg.ksm_type == KSOCK_MSG_NOOP &&
                    conn->ksnc_msg.ksm_csum != 0 &&     /* has checksum */
                    conn->ksnc_msg.ksm_csum != conn->ksnc_rx_csum) {
                        /* NOOP Checksum error */
                        CERROR("%s: Checksum error, wire:0x%08X data:0x%08X\n",
                               libcfs_id2str(conn->ksnc_peer->ksnp_id),
                               conn->ksnc_msg.ksm_csum, conn->ksnc_rx_csum);
                        ksocknal_new_packet(conn, 0);
                        ksocknal_close_conn_and_siblings(conn, -EPROTO);
                        return (-EIO);
                }

                if (conn->ksnc_msg.ksm_zc_cookies[1] != 0) {
                        __u64 cookie = 0;

                        LASSERT (conn->ksnc_proto != &ksocknal_protocol_v1x);

                        if (conn->ksnc_msg.ksm_type == KSOCK_MSG_NOOP)
                                cookie = conn->ksnc_msg.ksm_zc_cookies[0];

                        rc = conn->ksnc_proto->pro_handle_zcack(conn, cookie,
                                               conn->ksnc_msg.ksm_zc_cookies[1]);

                        if (rc != 0) {
                                CERROR("%s: Unknown ZC-ACK cookie: "LPU64", "LPU64"\n",
                                       libcfs_id2str(conn->ksnc_peer->ksnp_id),
                                       cookie, conn->ksnc_msg.ksm_zc_cookies[1]);
                                ksocknal_new_packet(conn, 0);
                                ksocknal_close_conn_and_siblings(conn, -EPROTO);
                                return (rc);
                        }
                }

                if (conn->ksnc_msg.ksm_type == KSOCK_MSG_NOOP) {
                        ksocknal_new_packet (conn, 0);
                        return 0;       /* NOOP is done and just return */
                }

                conn->ksnc_rx_state = SOCKNAL_RX_LNET_HEADER;
                conn->ksnc_rx_nob_wanted = sizeof(ksock_lnet_msg_t);
                conn->ksnc_rx_nob_left = sizeof(ksock_lnet_msg_t);

                conn->ksnc_rx_iov = (struct iovec *)&conn->ksnc_rx_iov_space;
                conn->ksnc_rx_iov[0].iov_base = (char *)&conn->ksnc_msg.ksm_u.lnetmsg;
                conn->ksnc_rx_iov[0].iov_len  = sizeof(ksock_lnet_msg_t);

                conn->ksnc_rx_niov = 1;
                conn->ksnc_rx_kiov = NULL;
                conn->ksnc_rx_nkiov = 0;

                goto again;     /* read lnet header now */

        case SOCKNAL_RX_LNET_HEADER:
                /* unpack message header */
                conn->ksnc_proto->pro_unpack(&conn->ksnc_msg);

                if ((conn->ksnc_peer->ksnp_id.pid & LNET_PID_USERFLAG) != 0) {
                        /* Userspace peer */
                        lhdr = &conn->ksnc_msg.ksm_u.lnetmsg.ksnm_hdr;
                        id   = &conn->ksnc_peer->ksnp_id;

                        /* Substitute process ID assigned at connection time */
                        lhdr->src_pid = cpu_to_le32(id->pid);
                        lhdr->src_nid = cpu_to_le64(id->nid);
                }

                conn->ksnc_rx_state = SOCKNAL_RX_PARSE;
                ksocknal_conn_addref(conn);     /* ++ref while parsing */

                rc = lnet_parse(conn->ksnc_peer->ksnp_ni,
                                &conn->ksnc_msg.ksm_u.lnetmsg.ksnm_hdr,
                                conn->ksnc_peer->ksnp_id.nid, conn, 0);
                if (rc < 0) {
                        /* I just received garbage: give up on this conn */
                        ksocknal_new_packet(conn, 0);
                        ksocknal_close_conn_and_siblings (conn, rc);
                        ksocknal_conn_decref(conn);
                        return (-EPROTO);
                }

                /* I'm racing with ksocknal_recv() */
                LASSERT (conn->ksnc_rx_state == SOCKNAL_RX_PARSE ||
                         conn->ksnc_rx_state == SOCKNAL_RX_LNET_PAYLOAD);

                if (conn->ksnc_rx_state != SOCKNAL_RX_LNET_PAYLOAD)
                        return 0;

                /* ksocknal_recv() got called */
                goto again;

        case SOCKNAL_RX_LNET_PAYLOAD:
                /* payload all received */
                rc = 0;

                if (conn->ksnc_rx_nob_left == 0 &&   /* not truncating */
                    conn->ksnc_msg.ksm_csum != 0 &&  /* has checksum */
                    conn->ksnc_msg.ksm_csum != conn->ksnc_rx_csum) {
                        CERROR("%s: Checksum error, wire:0x%08X data:0x%08X\n",
                               libcfs_id2str(conn->ksnc_peer->ksnp_id),
                               conn->ksnc_msg.ksm_csum, conn->ksnc_rx_csum);
                        rc = -EIO;
                }

                if (rc == 0 && conn->ksnc_msg.ksm_zc_cookies[0] != 0) {
                        LASSERT(conn->ksnc_proto != &ksocknal_protocol_v1x);

                        lhdr = &conn->ksnc_msg.ksm_u.lnetmsg.ksnm_hdr;
                        id   = &conn->ksnc_peer->ksnp_id;

                        rc = conn->ksnc_proto->pro_handle_zcreq(conn,
                                        conn->ksnc_msg.ksm_zc_cookies[0],
                                        *ksocknal_tunables.ksnd_nonblk_zcack ||
                                        le64_to_cpu(lhdr->src_nid) != id->nid);
                }

                lnet_finalize(conn->ksnc_peer->ksnp_ni, conn->ksnc_cookie, rc);

                if (rc != 0) {
                        ksocknal_new_packet(conn, 0);
                        ksocknal_close_conn_and_siblings (conn, rc);
                        return (-EPROTO);
                }
                /* Fall through */

        case SOCKNAL_RX_SLOP:
                /* starting new packet? */
                if (ksocknal_new_packet (conn, conn->ksnc_rx_nob_left))
                        return 0;       /* come back later */
                goto again;             /* try to finish reading slop now */

        default:
                break;
        }

        /* Not Reached */
        LBUG ();
        return (-EINVAL);                       /* keep gcc happy */
}

int
ksocknal_recv (lnet_ni_t *ni, void *private, lnet_msg_t *msg, int delayed,
               unsigned int niov, struct iovec *iov, lnet_kiov_t *kiov,
               unsigned int offset, unsigned int mlen, unsigned int rlen)
{
        ksock_conn_t  *conn = (ksock_conn_t *)private;
        ksock_sched_t *sched = conn->ksnc_scheduler;

        LASSERT (mlen <= rlen);
        LASSERT (niov <= LNET_MAX_IOV);

        conn->ksnc_cookie = msg;
        conn->ksnc_rx_nob_wanted = mlen;
        conn->ksnc_rx_nob_left   = rlen;

        if (mlen == 0 || iov != NULL) {
                conn->ksnc_rx_nkiov = 0;
                conn->ksnc_rx_kiov = NULL;
                conn->ksnc_rx_iov = conn->ksnc_rx_iov_space.iov;
                conn->ksnc_rx_niov =
                        lnet_extract_iov(LNET_MAX_IOV, conn->ksnc_rx_iov,
                                         niov, iov, offset, mlen);
        } else {
                conn->ksnc_rx_niov = 0;
                conn->ksnc_rx_iov  = NULL;
                conn->ksnc_rx_kiov = conn->ksnc_rx_iov_space.kiov;
                conn->ksnc_rx_nkiov =
                        lnet_extract_kiov(LNET_MAX_IOV, conn->ksnc_rx_kiov,
                                          niov, kiov, offset, mlen);
        }

        LASSERT (mlen ==
                 lnet_iov_nob (conn->ksnc_rx_niov, conn->ksnc_rx_iov) +
                 lnet_kiov_nob (conn->ksnc_rx_nkiov, conn->ksnc_rx_kiov));

        LASSERT (conn->ksnc_rx_scheduled);

        cfs_spin_lock_bh (&sched->kss_lock);

        switch (conn->ksnc_rx_state) {
        case SOCKNAL_RX_PARSE_WAIT:
                cfs_list_add_tail(&conn->ksnc_rx_list, &sched->kss_rx_conns);
                cfs_waitq_signal (&sched->kss_waitq);
                LASSERT (conn->ksnc_rx_ready);
                break;

        case SOCKNAL_RX_PARSE:
                /* scheduler hasn't noticed I'm parsing yet */
                break;
        }

        conn->ksnc_rx_state = SOCKNAL_RX_LNET_PAYLOAD;

        cfs_spin_unlock_bh (&sched->kss_lock);
        ksocknal_conn_decref(conn);
        return (0);
}

static inline int
ksocknal_sched_cansleep(ksock_sched_t *sched)
{
        int           rc;

        cfs_spin_lock_bh (&sched->kss_lock);

        rc = (!ksocknal_data.ksnd_shuttingdown &&
              cfs_list_empty(&sched->kss_rx_conns) &&
              cfs_list_empty(&sched->kss_tx_conns));

        cfs_spin_unlock_bh (&sched->kss_lock);
        return (rc);
}

int ksocknal_scheduler (void *arg)
{
        ksock_sched_t     *sched = (ksock_sched_t *)arg;
        ksock_conn_t      *conn;
        ksock_tx_t        *tx;
        int                rc;
        int                nloops = 0;
        int                id = (int)(sched - ksocknal_data.ksnd_schedulers);
        char               name[16];

        snprintf (name, sizeof (name),"socknal_sd%02d", id);
        cfs_daemonize (name);
        cfs_block_allsigs ();

        if (ksocknal_lib_bind_thread_to_cpu(id))
                CERROR ("Can't set CPU affinity for %s to %d\n", name, id);

        cfs_spin_lock_bh (&sched->kss_lock);

        while (!ksocknal_data.ksnd_shuttingdown) {
                int did_something = 0;

                /* Ensure I progress everything semi-fairly */

                if (!cfs_list_empty (&sched->kss_rx_conns)) {
                        conn = cfs_list_entry(sched->kss_rx_conns.next,
                                              ksock_conn_t, ksnc_rx_list);
                        cfs_list_del(&conn->ksnc_rx_list);

                        LASSERT(conn->ksnc_rx_scheduled);
                        LASSERT(conn->ksnc_rx_ready);

                        /* clear rx_ready in case receive isn't complete.
                         * Do it BEFORE we call process_recv, since
                         * data_ready can set it any time after we release
                         * kss_lock. */
                        conn->ksnc_rx_ready = 0;
                        cfs_spin_unlock_bh (&sched->kss_lock);

                        rc = ksocknal_process_receive(conn);

                        cfs_spin_lock_bh (&sched->kss_lock);

                        /* I'm the only one that can clear this flag */
                        LASSERT(conn->ksnc_rx_scheduled);

                        /* Did process_receive get everything it wanted? */
                        if (rc == 0)
                                conn->ksnc_rx_ready = 1;

                        if (conn->ksnc_rx_state == SOCKNAL_RX_PARSE) {
                                /* Conn blocked waiting for ksocknal_recv()
                                 * I change its state (under lock) to signal
                                 * it can be rescheduled */
                                conn->ksnc_rx_state = SOCKNAL_RX_PARSE_WAIT;
                        } else if (conn->ksnc_rx_ready) {
                                /* reschedule for rx */
                                cfs_list_add_tail (&conn->ksnc_rx_list,
                                                   &sched->kss_rx_conns);
                        } else {
                                conn->ksnc_rx_scheduled = 0;
                                /* drop my ref */
                                ksocknal_conn_decref(conn);
                        }

                        did_something = 1;
                }

                if (!cfs_list_empty (&sched->kss_tx_conns)) {
                        CFS_LIST_HEAD    (zlist);

                        if (!cfs_list_empty(&sched->kss_zombie_noop_txs)) {
                                cfs_list_add(&zlist,
                                             &sched->kss_zombie_noop_txs);
                                cfs_list_del_init(&sched->kss_zombie_noop_txs);
                        }

                        conn = cfs_list_entry(sched->kss_tx_conns.next,
                                              ksock_conn_t, ksnc_tx_list);
                        cfs_list_del (&conn->ksnc_tx_list);

                        LASSERT(conn->ksnc_tx_scheduled);
                        LASSERT(conn->ksnc_tx_ready);
                        LASSERT(!cfs_list_empty(&conn->ksnc_tx_queue));

                        tx = cfs_list_entry(conn->ksnc_tx_queue.next,
                                            ksock_tx_t, tx_list);

                        if (conn->ksnc_tx_carrier == tx)
                                ksocknal_next_tx_carrier(conn);

                        /* dequeue now so empty list => more to send */
                        cfs_list_del(&tx->tx_list);

                        /* Clear tx_ready in case send isn't complete.  Do
                         * it BEFORE we call process_transmit, since
                         * write_space can set it any time after we release
                         * kss_lock. */
                        conn->ksnc_tx_ready = 0;
                        cfs_spin_unlock_bh (&sched->kss_lock);

                        if (!cfs_list_empty(&zlist)) {
                                /* free zombie noop txs, it's fast because 
                                 * noop txs are just put in freelist */
                                ksocknal_txlist_done(NULL, &zlist, 0);
                        }

                        rc = ksocknal_process_transmit(conn, tx);

                        if (rc == -ENOMEM || rc == -EAGAIN) {
                                /* Incomplete send: replace tx on HEAD of tx_queue */
                                cfs_spin_lock_bh (&sched->kss_lock);
                                cfs_list_add (&tx->tx_list,
                                              &conn->ksnc_tx_queue);
                        } else {
                                /* Complete send; tx -ref */
                                ksocknal_tx_decref (tx);

                                cfs_spin_lock_bh (&sched->kss_lock);
                                /* assume space for more */
                                conn->ksnc_tx_ready = 1;
                        }

                        if (rc == -ENOMEM) {
                                /* Do nothing; after a short timeout, this
                                 * conn will be reposted on kss_tx_conns. */
                        } else if (conn->ksnc_tx_ready &&
                                   !cfs_list_empty (&conn->ksnc_tx_queue)) {
                                /* reschedule for tx */
                                cfs_list_add_tail (&conn->ksnc_tx_list,
                                                   &sched->kss_tx_conns);
                        } else {
                                conn->ksnc_tx_scheduled = 0;
                                /* drop my ref */
                                ksocknal_conn_decref(conn);
                        }

                        did_something = 1;
                }
                if (!did_something ||           /* nothing to do */
                    ++nloops == SOCKNAL_RESCHED) { /* hogging CPU? */
                        cfs_spin_unlock_bh (&sched->kss_lock);

                        nloops = 0;

                        if (!did_something) {   /* wait for something to do */
                                cfs_wait_event_interruptible_exclusive(
                                        sched->kss_waitq,
                                        !ksocknal_sched_cansleep(sched), rc);
                                LASSERT (rc == 0);
                        } else {
                                cfs_cond_resched();
                        }

                        cfs_spin_lock_bh (&sched->kss_lock);
                }
        }

        cfs_spin_unlock_bh (&sched->kss_lock);
        ksocknal_thread_fini ();
        return (0);
}

/*
 * Add connection to kss_rx_conns of scheduler
 * and wakeup the scheduler.
 */
void ksocknal_read_callback (ksock_conn_t *conn)
{
        ksock_sched_t *sched;
        ENTRY;

        sched = conn->ksnc_scheduler;

        cfs_spin_lock_bh (&sched->kss_lock);

        conn->ksnc_rx_ready = 1;

        if (!conn->ksnc_rx_scheduled) {  /* not being progressed */
                cfs_list_add_tail(&conn->ksnc_rx_list,
                                  &sched->kss_rx_conns);
                conn->ksnc_rx_scheduled = 1;
                /* extra ref for scheduler */
                ksocknal_conn_addref(conn);

                cfs_waitq_signal (&sched->kss_waitq);
        }
        cfs_spin_unlock_bh (&sched->kss_lock);

        EXIT;
}

/*
 * Add connection to kss_tx_conns of scheduler
 * and wakeup the scheduler.
 */
void ksocknal_write_callback (ksock_conn_t *conn)
{
        ksock_sched_t *sched;
        ENTRY;

        sched = conn->ksnc_scheduler;

        cfs_spin_lock_bh (&sched->kss_lock);

        conn->ksnc_tx_ready = 1;

        if (!conn->ksnc_tx_scheduled && // not being progressed
            !cfs_list_empty(&conn->ksnc_tx_queue)){//packets to send
                cfs_list_add_tail (&conn->ksnc_tx_list,
                                   &sched->kss_tx_conns);
                conn->ksnc_tx_scheduled = 1;
                /* extra ref for scheduler */
                ksocknal_conn_addref(conn);

                cfs_waitq_signal (&sched->kss_waitq);
        }

        cfs_spin_unlock_bh (&sched->kss_lock);

        EXIT;
}

ksock_proto_t *
ksocknal_parse_proto_version (ksock_hello_msg_t *hello)
{
        __u32   version = 0;

        if (hello->kshm_magic == LNET_PROTO_MAGIC)
                version = hello->kshm_version;
        else if (hello->kshm_magic == __swab32(LNET_PROTO_MAGIC))
                version = __swab32(hello->kshm_version);

        if (version != 0) {
#if SOCKNAL_VERSION_DEBUG
                if (*ksocknal_tunables.ksnd_protocol == 1)
                        return NULL;

                if (*ksocknal_tunables.ksnd_protocol == 2 &&
                    version == KSOCK_PROTO_V3)
                        return NULL;
#endif
                if (version == KSOCK_PROTO_V2)
                        return &ksocknal_protocol_v2x;

                if (version == KSOCK_PROTO_V3)
                        return &ksocknal_protocol_v3x;

                return NULL;
        }

        if (hello->kshm_magic == le32_to_cpu(LNET_PROTO_TCP_MAGIC)) {
                lnet_magicversion_t *hmv = (lnet_magicversion_t *)hello;

                CLASSERT (sizeof (lnet_magicversion_t) ==
                          offsetof (ksock_hello_msg_t, kshm_src_nid));

                if (hmv->version_major == cpu_to_le16 (KSOCK_PROTO_V1_MAJOR) &&
                    hmv->version_minor == cpu_to_le16 (KSOCK_PROTO_V1_MINOR))
                        return &ksocknal_protocol_v1x;
        }

        return NULL;
}

int
ksocknal_send_hello (lnet_ni_t *ni, ksock_conn_t *conn,
                     lnet_nid_t peer_nid, ksock_hello_msg_t *hello)
{
        /* CAVEAT EMPTOR: this byte flips 'ipaddrs' */
        ksock_net_t         *net = (ksock_net_t *)ni->ni_data;

        LASSERT (hello->kshm_nips <= LNET_MAX_INTERFACES);

        /* rely on caller to hold a ref on socket so it wouldn't disappear */
        LASSERT (conn->ksnc_proto != NULL);

        hello->kshm_src_nid         = ni->ni_nid;
        hello->kshm_dst_nid         = peer_nid;
        hello->kshm_src_pid         = the_lnet.ln_pid;

        hello->kshm_src_incarnation = net->ksnn_incarnation;
        hello->kshm_ctype           = conn->ksnc_type;

        return conn->ksnc_proto->pro_send_hello(conn, hello);
}

int
ksocknal_invert_type(int type)
{
        switch (type)
        {
        case SOCKLND_CONN_ANY:
        case SOCKLND_CONN_CONTROL:
                return (type);
        case SOCKLND_CONN_BULK_IN:
                return SOCKLND_CONN_BULK_OUT;
        case SOCKLND_CONN_BULK_OUT:
                return SOCKLND_CONN_BULK_IN;
        default:
                return (SOCKLND_CONN_NONE);
        }
}

int
ksocknal_recv_hello (lnet_ni_t *ni, ksock_conn_t *conn,
                     ksock_hello_msg_t *hello, lnet_process_id_t *peerid,
                     __u64 *incarnation)
{
        /* Return < 0        fatal error
         *        0          success
         *        EALREADY   lost connection race
         *        EPROTO     protocol version mismatch
         */
        cfs_socket_t        *sock = conn->ksnc_sock;
        int                  active = (conn->ksnc_proto != NULL);
        int                  timeout;
        int                  proto_match;
        int                  rc;
        ksock_proto_t       *proto;
        lnet_process_id_t    recv_id;

        /* socket type set on active connections - not set on passive */
        LASSERT (!active == !(conn->ksnc_type != SOCKLND_CONN_NONE));

        timeout = active ? *ksocknal_tunables.ksnd_timeout :
                            lnet_acceptor_timeout();

        rc = libcfs_sock_read(sock, &hello->kshm_magic, sizeof (hello->kshm_magic), timeout);
        if (rc != 0) {
                CERROR ("Error %d reading HELLO from %u.%u.%u.%u\n",
                        rc, HIPQUAD(conn->ksnc_ipaddr));
                LASSERT (rc < 0);
                return rc;
        }

        if (hello->kshm_magic != LNET_PROTO_MAGIC &&
            hello->kshm_magic != __swab32(LNET_PROTO_MAGIC) &&
            hello->kshm_magic != le32_to_cpu (LNET_PROTO_TCP_MAGIC)) {
                /* Unexpected magic! */
                CERROR ("Bad magic(1) %#08x (%#08x expected) from "
                        "%u.%u.%u.%u\n", __cpu_to_le32 (hello->kshm_magic),
                        LNET_PROTO_TCP_MAGIC,
                        HIPQUAD(conn->ksnc_ipaddr));
                return -EPROTO;
        }

        rc = libcfs_sock_read(sock, &hello->kshm_version,
                              sizeof(hello->kshm_version), timeout);
        if (rc != 0) {
                CERROR ("Error %d reading HELLO from %u.%u.%u.%u\n",
                        rc, HIPQUAD(conn->ksnc_ipaddr));
                LASSERT (rc < 0);
                return rc;
        }

        proto = ksocknal_parse_proto_version(hello);
        if (proto == NULL) {
                if (!active) {
                        /* unknown protocol from peer, tell peer my protocol */
                        conn->ksnc_proto = &ksocknal_protocol_v3x;
#if SOCKNAL_VERSION_DEBUG
                        if (*ksocknal_tunables.ksnd_protocol == 2)
                                conn->ksnc_proto = &ksocknal_protocol_v2x;
                        else if (*ksocknal_tunables.ksnd_protocol == 1)
                                conn->ksnc_proto = &ksocknal_protocol_v1x;
#endif
                        hello->kshm_nips = 0;
                        ksocknal_send_hello(ni, conn, ni->ni_nid, hello);
                }

                CERROR ("Unknown protocol version (%d.x expected)"
                        " from %u.%u.%u.%u\n",
                        conn->ksnc_proto->pro_version,
                        HIPQUAD(conn->ksnc_ipaddr));

                return -EPROTO;
        }

        proto_match = (conn->ksnc_proto == proto);
        conn->ksnc_proto = proto;

        /* receive the rest of hello message anyway */
        rc = conn->ksnc_proto->pro_recv_hello(conn, hello, timeout);
        if (rc != 0) {
                CERROR("Error %d reading or checking hello from from %u.%u.%u.%u\n",
                       rc, HIPQUAD(conn->ksnc_ipaddr));
                LASSERT (rc < 0);
                return rc;
        }

        *incarnation = hello->kshm_src_incarnation;

        if (hello->kshm_src_nid == LNET_NID_ANY) {
                CERROR("Expecting a HELLO hdr with a NID, but got LNET_NID_ANY"
                       "from %u.%u.%u.%u\n", HIPQUAD(conn->ksnc_ipaddr));
                return -EPROTO;
        }

        if (!active &&
            conn->ksnc_port > LNET_ACCEPTOR_MAX_RESERVED_PORT) {
                /* Userspace NAL assigns peer process ID from socket */
                recv_id.pid = conn->ksnc_port | LNET_PID_USERFLAG;
                recv_id.nid = LNET_MKNID(LNET_NIDNET(ni->ni_nid), conn->ksnc_ipaddr);
        } else {
                recv_id.nid = hello->kshm_src_nid;
                recv_id.pid = hello->kshm_src_pid;
        }

        if (!active) {
                *peerid = recv_id;

                /* peer determines type */
                conn->ksnc_type = ksocknal_invert_type(hello->kshm_ctype);
                if (conn->ksnc_type == SOCKLND_CONN_NONE) {
                        CERROR ("Unexpected type %d from %s ip %u.%u.%u.%u\n",
                                hello->kshm_ctype, libcfs_id2str(*peerid),
                                HIPQUAD(conn->ksnc_ipaddr));
                        return -EPROTO;
                }

                return 0;
        }

        if (peerid->pid != recv_id.pid ||
            peerid->nid != recv_id.nid) {
                LCONSOLE_ERROR_MSG(0x130, "Connected successfully to %s on host"
                                   " %u.%u.%u.%u, but they claimed they were "
                                   "%s; please check your Lustre "
                                   "configuration.\n",
                                   libcfs_id2str(*peerid),
                                   HIPQUAD(conn->ksnc_ipaddr),
                                   libcfs_id2str(recv_id));
                return -EPROTO;
        }

        if (hello->kshm_ctype == SOCKLND_CONN_NONE) {
                /* Possible protocol mismatch or I lost the connection race */
                return proto_match ? EALREADY : EPROTO;
        }

        if (ksocknal_invert_type(hello->kshm_ctype) != conn->ksnc_type) {
                CERROR ("Mismatched types: me %d, %s ip %u.%u.%u.%u %d\n",
                        conn->ksnc_type, libcfs_id2str(*peerid), 
                        HIPQUAD(conn->ksnc_ipaddr),
                        hello->kshm_ctype);
                return -EPROTO;
        }

        return 0;
}

int
ksocknal_connect (ksock_route_t *route)
{
        CFS_LIST_HEAD    (zombies);
        ksock_peer_t     *peer = route->ksnr_peer;
        int               type;
        int               wanted;
        cfs_socket_t     *sock;
        cfs_time_t        deadline;
        int               retry_later = 0;
        int               rc = 0;

        deadline = cfs_time_add(cfs_time_current(),
                                cfs_time_seconds(*ksocknal_tunables.ksnd_timeout));

        cfs_write_lock_bh (&ksocknal_data.ksnd_global_lock);

        LASSERT (route->ksnr_scheduled);
        LASSERT (!route->ksnr_connecting);

        route->ksnr_connecting = 1;

        for (;;) {
                wanted = ksocknal_route_mask() & ~route->ksnr_connected;

                /* stop connecting if peer/route got closed under me, or
                 * route got connected while queued */
                if (peer->ksnp_closing || route->ksnr_deleted ||
                    wanted == 0) {
                        retry_later = 0;
                        break;
                }

                /* reschedule if peer is connecting to me */
                if (peer->ksnp_accepting > 0) {
                        CDEBUG(D_NET,
                               "peer %s(%d) already connecting to me, retry later.\n",
                               libcfs_nid2str(peer->ksnp_id.nid), peer->ksnp_accepting);
                        retry_later = 1;
                }

                if (retry_later) /* needs reschedule */
                        break;

                if ((wanted & (1 << SOCKLND_CONN_ANY)) != 0) {
                        type = SOCKLND_CONN_ANY;
                } else if ((wanted & (1 << SOCKLND_CONN_CONTROL)) != 0) {
                        type = SOCKLND_CONN_CONTROL;
                } else if ((wanted & (1 << SOCKLND_CONN_BULK_IN)) != 0) {
                        type = SOCKLND_CONN_BULK_IN;
                } else {
                        LASSERT ((wanted & (1 << SOCKLND_CONN_BULK_OUT)) != 0);
                        type = SOCKLND_CONN_BULK_OUT;
                }

                cfs_write_unlock_bh (&ksocknal_data.ksnd_global_lock);

                if (cfs_time_aftereq(cfs_time_current(), deadline)) {
                        rc = -ETIMEDOUT;
                        lnet_connect_console_error(rc, peer->ksnp_id.nid,
                                                   route->ksnr_ipaddr,
                                                   route->ksnr_port);
                        goto failed;
                }

                rc = lnet_connect(&sock, peer->ksnp_id.nid,
                                  route->ksnr_myipaddr,
                                  route->ksnr_ipaddr, route->ksnr_port);
                if (rc != 0)
                        goto failed;

                rc = ksocknal_create_conn(peer->ksnp_ni, route, sock, type);
                if (rc < 0) {
                        lnet_connect_console_error(rc, peer->ksnp_id.nid,
                                                   route->ksnr_ipaddr,
                                                   route->ksnr_port);
                        goto failed;
                }

                /* A +ve RC means I have to retry because I lost the connection
                 * race or I have to renegotiate protocol version */
                retry_later = (rc != 0);
                if (retry_later)
                        CDEBUG(D_NET, "peer %s: conn race, retry later.\n",
                               libcfs_nid2str(peer->ksnp_id.nid));

                cfs_write_lock_bh (&ksocknal_data.ksnd_global_lock);
        }

        route->ksnr_scheduled = 0;
        route->ksnr_connecting = 0;

        if (retry_later) {
                /* re-queue for attention; this frees me up to handle
                 * the peer's incoming connection request */

                if (rc == EALREADY ||
                    (rc == 0 && peer->ksnp_accepting > 0)) {
                        /* We want to introduce a delay before next
                         * attempt to connect if we lost conn race,
                         * but the race is resolved quickly usually,
                         * so min_reconnectms should be good heuristic */
                        route->ksnr_retry_interval =
                                cfs_time_seconds(*ksocknal_tunables.ksnd_min_reconnectms)/1000;
                        route->ksnr_timeout = cfs_time_add(cfs_time_current(),
                                                           route->ksnr_retry_interval);
                }

                ksocknal_launch_connection_locked(route);
        }

        cfs_write_unlock_bh (&ksocknal_data.ksnd_global_lock);
        return retry_later;

 failed:
        cfs_write_lock_bh (&ksocknal_data.ksnd_global_lock);

        route->ksnr_scheduled = 0;
        route->ksnr_connecting = 0;

        /* This is a retry rather than a new connection */
        route->ksnr_retry_interval *= 2;
        route->ksnr_retry_interval =
                MAX(route->ksnr_retry_interval,
                    cfs_time_seconds(*ksocknal_tunables.ksnd_min_reconnectms)/1000);
        route->ksnr_retry_interval =
                MIN(route->ksnr_retry_interval,
                    cfs_time_seconds(*ksocknal_tunables.ksnd_max_reconnectms)/1000);

        LASSERT (route->ksnr_retry_interval != 0);
        route->ksnr_timeout = cfs_time_add(cfs_time_current(),
                                           route->ksnr_retry_interval);

        if (!cfs_list_empty(&peer->ksnp_tx_queue) &&
            peer->ksnp_accepting == 0 &&
            ksocknal_find_connecting_route_locked(peer) == NULL) {
                ksock_conn_t *conn;

                /* ksnp_tx_queue is queued on a conn on successful
                 * connection for V1.x and V2.x */
                if (!cfs_list_empty (&peer->ksnp_conns)) {
                        conn = cfs_list_entry(peer->ksnp_conns.next,
                                              ksock_conn_t, ksnc_list);
                        LASSERT (conn->ksnc_proto == &ksocknal_protocol_v3x);
                }

                /* take all the blocked packets while I've got the lock and
                 * complete below... */
                cfs_list_splice_init(&peer->ksnp_tx_queue, &zombies);
        }

#if 0           /* irrelevent with only eager routes */
        if (!route->ksnr_deleted) {
                /* make this route least-favourite for re-selection */
                cfs_list_del(&route->ksnr_list);
                cfs_list_add_tail(&route->ksnr_list, &peer->ksnp_routes);
        }
#endif
        cfs_write_unlock_bh (&ksocknal_data.ksnd_global_lock);

        ksocknal_peer_failed(peer);
        ksocknal_txlist_done(peer->ksnp_ni, &zombies, 1);
        return 0;
}

/* Go through connd_routes queue looking for a route that
   we can process right now */
static ksock_route_t *
ksocknal_connd_get_route_locked(signed long *timeout_p)
{
        ksock_route_t *route;
        cfs_time_t     now;

        /* Only handle an outgoing connection request if there
         * is someone left to handle incoming connections */
        if ((ksocknal_data.ksnd_connd_connecting + 1) >=
            *ksocknal_tunables.ksnd_nconnds)
                return NULL;

        now = cfs_time_current();

        /* connd_routes can contain both pending and ordinary routes */
        cfs_list_for_each_entry (route, &ksocknal_data.ksnd_connd_routes,
                                 ksnr_connd_list) {

                if (route->ksnr_retry_interval == 0 ||
                    cfs_time_aftereq(now, route->ksnr_timeout))
                        return route;

                if (*timeout_p == CFS_MAX_SCHEDULE_TIMEOUT ||
                    (int)*timeout_p > (int)(route->ksnr_timeout - now))
                        *timeout_p = (int)(route->ksnr_timeout - now);
        }

        return NULL;
}

int
ksocknal_connd (void *arg)
{
        long               id = (long)(long_ptr_t)arg;
        char               name[16];
        ksock_connreq_t   *cr;
        ksock_route_t     *route;
        cfs_waitlink_t     wait;
        signed long        timeout;
        int                nloops = 0;
        int                cons_retry = 0;
        int                dropped_lock;

        snprintf (name, sizeof (name), "socknal_cd%02ld", id);
        cfs_daemonize (name);
        cfs_block_allsigs ();

        cfs_waitlink_init (&wait);

        cfs_spin_lock_bh (&ksocknal_data.ksnd_connd_lock);

        while (!ksocknal_data.ksnd_shuttingdown) {

                dropped_lock = 0;

                if (!cfs_list_empty(&ksocknal_data.ksnd_connd_connreqs)) {
                        /* Connection accepted by the listener */
                        cr = cfs_list_entry(ksocknal_data.ksnd_connd_connreqs. \
                                            next, ksock_connreq_t, ksncr_list);

                        cfs_list_del(&cr->ksncr_list);
                        cfs_spin_unlock_bh (&ksocknal_data.ksnd_connd_lock);
                        dropped_lock = 1;

                        ksocknal_create_conn(cr->ksncr_ni, NULL,
                                             cr->ksncr_sock, SOCKLND_CONN_NONE);
                        lnet_ni_decref(cr->ksncr_ni);
                        LIBCFS_FREE(cr, sizeof(*cr));

                        cfs_spin_lock_bh (&ksocknal_data.ksnd_connd_lock);
                }

                /* Sleep till explicit wake_up if no pending routes present */
                timeout = CFS_MAX_SCHEDULE_TIMEOUT;

                /* Connection request */
                route = ksocknal_connd_get_route_locked(&timeout);

                if (route != NULL) {
                        cfs_list_del (&route->ksnr_connd_list);
                        ksocknal_data.ksnd_connd_connecting++;
                        cfs_spin_unlock_bh (&ksocknal_data.ksnd_connd_lock);
                        dropped_lock = 1;

                        if (ksocknal_connect(route)) {
                                /* consecutive retry */
                                if (cons_retry++ > SOCKNAL_INSANITY_RECONN) {
                                        CWARN("massive consecutive "
                                              "re-connecting to %u.%u.%u.%u\n",
                                              HIPQUAD(route->ksnr_ipaddr));
                                        cons_retry = 0;
                                }
                        } else {
                                cons_retry = 0;
                        }

                        ksocknal_route_decref(route);

                        cfs_spin_lock_bh (&ksocknal_data.ksnd_connd_lock);
                        ksocknal_data.ksnd_connd_connecting--;
                }

                if (dropped_lock) {
                        if (++nloops < SOCKNAL_RESCHED)
                                continue;
                        cfs_spin_unlock_bh(&ksocknal_data.ksnd_connd_lock);
                        nloops = 0;
                        cfs_cond_resched();
                        cfs_spin_lock_bh(&ksocknal_data.ksnd_connd_lock);
                        continue;
                }

                /* Nothing to do for 'timeout'  */
                cfs_set_current_state (CFS_TASK_INTERRUPTIBLE);
                cfs_waitq_add_exclusive (&ksocknal_data.ksnd_connd_waitq,
                                         &wait);
                cfs_spin_unlock_bh (&ksocknal_data.ksnd_connd_lock);

                nloops = 0;
                cfs_waitq_timedwait (&wait, CFS_TASK_INTERRUPTIBLE, timeout);

                cfs_set_current_state (CFS_TASK_RUNNING);
                cfs_waitq_del (&ksocknal_data.ksnd_connd_waitq, &wait);
                cfs_spin_lock_bh (&ksocknal_data.ksnd_connd_lock);
        }

        cfs_spin_unlock_bh (&ksocknal_data.ksnd_connd_lock);

        ksocknal_thread_fini ();
        return (0);
}

ksock_conn_t *
ksocknal_find_timed_out_conn (ksock_peer_t *peer)
{
        /* We're called with a shared lock on ksnd_global_lock */
        ksock_conn_t      *conn;
        cfs_list_t        *ctmp;

        cfs_list_for_each (ctmp, &peer->ksnp_conns) {
                int     error;
                conn = cfs_list_entry (ctmp, ksock_conn_t, ksnc_list);

                /* Don't need the {get,put}connsock dance to deref ksnc_sock */
                LASSERT (!conn->ksnc_closing);

                /* SOCK_ERROR will reset error code of socket in
                 * some platform (like Darwin8.x) */
                error = libcfs_sock_error(conn->ksnc_sock);
                if (error != 0) {
                        ksocknal_conn_addref(conn);

                        switch (error) {
                        case ECONNRESET:
                                CDEBUG(D_NETERROR, "A connection with %s "
                                       "(%u.%u.%u.%u:%d) was reset; "
                                       "it may have rebooted.\n",
                                       libcfs_id2str(peer->ksnp_id),
                                       HIPQUAD(conn->ksnc_ipaddr),
                                       conn->ksnc_port);
                                break;
                        case ETIMEDOUT:
                                CDEBUG(D_NETERROR, "A connection with %s "
                                       "(%u.%u.%u.%u:%d) timed out; the "
                                       "network or node may be down.\n",
                                       libcfs_id2str(peer->ksnp_id),
                                       HIPQUAD(conn->ksnc_ipaddr),
                                       conn->ksnc_port);
                                break;
                        default:
                                CDEBUG(D_NETERROR, "An unexpected network error %d "
                                       "occurred with %s "
                                       "(%u.%u.%u.%u:%d\n", error,
                                       libcfs_id2str(peer->ksnp_id),
                                       HIPQUAD(conn->ksnc_ipaddr),
                                       conn->ksnc_port);
                                break;
                        }

                        return (conn);
                }

                if (conn->ksnc_rx_started &&
                    cfs_time_aftereq(cfs_time_current(),
                                     conn->ksnc_rx_deadline)) {
                        /* Timed out incomplete incoming message */
                        ksocknal_conn_addref(conn);
                        CDEBUG(D_NETERROR, "Timeout receiving from %s "
                               "(%u.%u.%u.%u:%d), state %d wanted %d left %d\n",
                               libcfs_id2str(peer->ksnp_id),
                               HIPQUAD(conn->ksnc_ipaddr),
                               conn->ksnc_port,
                               conn->ksnc_rx_state,
                               conn->ksnc_rx_nob_wanted,
                               conn->ksnc_rx_nob_left);
                        return (conn);
                }

                if ((!cfs_list_empty(&conn->ksnc_tx_queue) ||
                     libcfs_sock_wmem_queued(conn->ksnc_sock) != 0) &&
                    cfs_time_aftereq(cfs_time_current(),
                                     conn->ksnc_tx_deadline)) {
                        /* Timed out messages queued for sending or
                         * buffered in the socket's send buffer */
                        ksocknal_conn_addref(conn);
                        CDEBUG(D_NETERROR, "Timeout sending data to %s "
                               "(%u.%u.%u.%u:%d) the network or that "
                               "node may be down.\n",
                               libcfs_id2str(peer->ksnp_id),
                               HIPQUAD(conn->ksnc_ipaddr),
                               conn->ksnc_port);
                        return (conn);
                }
        }

        return (NULL);
}

static inline void
ksocknal_flush_stale_txs(ksock_peer_t *peer)
{
        ksock_tx_t        *tx;
        CFS_LIST_HEAD      (stale_txs);

        cfs_write_lock_bh (&ksocknal_data.ksnd_global_lock);

        while (!cfs_list_empty (&peer->ksnp_tx_queue)) {
                tx = cfs_list_entry (peer->ksnp_tx_queue.next,
                                     ksock_tx_t, tx_list);

                if (!cfs_time_aftereq(cfs_time_current(),
                                      tx->tx_deadline))
                        break;

                cfs_list_del (&tx->tx_list);
                cfs_list_add_tail (&tx->tx_list, &stale_txs);
        }

        cfs_write_unlock_bh (&ksocknal_data.ksnd_global_lock);

        ksocknal_txlist_done(peer->ksnp_ni, &stale_txs, 1);
}

int
ksocknal_send_keepalive_locked(ksock_peer_t *peer)
{
        ksock_sched_t  *sched;
        ksock_conn_t   *conn;
        ksock_tx_t     *tx;

        if (cfs_list_empty(&peer->ksnp_conns)) /* last_alive will be updated by create_conn */
                return 0;

        if (peer->ksnp_proto != &ksocknal_protocol_v3x)
                return 0;

        if (*ksocknal_tunables.ksnd_keepalive <= 0 ||
            cfs_time_before(cfs_time_current(),
                            cfs_time_add(peer->ksnp_last_alive,
                                         cfs_time_seconds(*ksocknal_tunables.ksnd_keepalive))))
                return 0;

        if (cfs_time_before(cfs_time_current(),
                            peer->ksnp_send_keepalive))
                return 0;

        /* retry 10 secs later, so we wouldn't put pressure
         * on this peer if we failed to send keepalive this time */
        peer->ksnp_send_keepalive = cfs_time_shift(10);

        conn = ksocknal_find_conn_locked(peer, NULL, 1);
        if (conn != NULL) {
                sched = conn->ksnc_scheduler;

                cfs_spin_lock_bh (&sched->kss_lock);
                if (!cfs_list_empty(&conn->ksnc_tx_queue)) {
                        cfs_spin_unlock_bh(&sched->kss_lock);
                        /* there is an queued ACK, don't need keepalive */
                        return 0;
                }

                cfs_spin_unlock_bh(&sched->kss_lock);
        }

        cfs_read_unlock(&ksocknal_data.ksnd_global_lock);

        /* cookie = 1 is reserved for keepalive PING */
        tx = ksocknal_alloc_tx_noop(1, 1);
        if (tx == NULL) {
                cfs_read_lock(&ksocknal_data.ksnd_global_lock);
                return -ENOMEM;
        }

        if (ksocknal_launch_packet(peer->ksnp_ni, tx, peer->ksnp_id) == 0) {
                cfs_read_lock(&ksocknal_data.ksnd_global_lock);
                return 1;
        }

        ksocknal_free_tx(tx);
        cfs_read_lock(&ksocknal_data.ksnd_global_lock);

        return -EIO;
}


void
ksocknal_check_peer_timeouts (int idx)
{
        cfs_list_t       *peers = &ksocknal_data.ksnd_peers[idx];
        ksock_peer_t     *peer;
        ksock_conn_t     *conn;
        ksock_tx_t       *tx;

 again:
        /* NB. We expect to have a look at all the peers and not find any
         * connections to time out, so we just use a shared lock while we
         * take a look... */
        cfs_read_lock (&ksocknal_data.ksnd_global_lock);

        cfs_list_for_each_entry_typed(peer, peers, ksock_peer_t, ksnp_list) {
                cfs_time_t  deadline = 0;
                int         resid = 0;
                int         n     = 0;

                if (ksocknal_send_keepalive_locked(peer) != 0) {
                        cfs_read_unlock (&ksocknal_data.ksnd_global_lock);
                        goto again;
                }

                conn = ksocknal_find_timed_out_conn (peer);

                if (conn != NULL) {
                        cfs_read_unlock (&ksocknal_data.ksnd_global_lock);

                        ksocknal_close_conn_and_siblings (conn, -ETIMEDOUT);

                        /* NB we won't find this one again, but we can't
                         * just proceed with the next peer, since we dropped
                         * ksnd_global_lock and it might be dead already! */
                        ksocknal_conn_decref(conn);
                        goto again;
                }

                /* we can't process stale txs right here because we're
                 * holding only shared lock */
                if (!cfs_list_empty (&peer->ksnp_tx_queue)) {
                        ksock_tx_t *tx =
                                cfs_list_entry (peer->ksnp_tx_queue.next,
                                                ksock_tx_t, tx_list);

                        if (cfs_time_aftereq(cfs_time_current(),
                                             tx->tx_deadline)) {

                                ksocknal_peer_addref(peer);
                                cfs_read_unlock (&ksocknal_data.ksnd_global_lock);

                                ksocknal_flush_stale_txs(peer);

                                ksocknal_peer_decref(peer);
                                goto again;
                        }
                }

                if (cfs_list_empty(&peer->ksnp_zc_req_list))
                        continue;

                cfs_spin_lock(&peer->ksnp_lock);
                cfs_list_for_each_entry_typed(tx, &peer->ksnp_zc_req_list,
                                              ksock_tx_t, tx_zc_list) {
                        if (!cfs_time_aftereq(cfs_time_current(),
                                              tx->tx_deadline))
                                break;
                        /* ignore the TX if connection is being closed */
                        if (tx->tx_conn->ksnc_closing)
                                continue;
                        n++;
                }

                if (n == 0) {
                        cfs_spin_unlock(&peer->ksnp_lock);
                        continue;
                }

                tx = cfs_list_entry(peer->ksnp_zc_req_list.next,
                                    ksock_tx_t, tx_zc_list);
                deadline = tx->tx_deadline;
                resid    = tx->tx_resid;
                conn     = tx->tx_conn;
                ksocknal_conn_addref(conn);

                cfs_spin_unlock(&peer->ksnp_lock);
                cfs_read_unlock (&ksocknal_data.ksnd_global_lock);

                CERROR("Total %d stale ZC_REQs for peer %s detected; the "
                       "oldest(%p) timed out %ld secs ago, "
                       "resid: %d, wmem: %d\n",
                       n, libcfs_nid2str(peer->ksnp_id.nid), tx,
                       cfs_duration_sec(cfs_time_current() - deadline),
                       resid, libcfs_sock_wmem_queued(conn->ksnc_sock));

                ksocknal_close_conn_and_siblings (conn, -ETIMEDOUT);
                ksocknal_conn_decref(conn);
                goto again;
        }

        cfs_read_unlock (&ksocknal_data.ksnd_global_lock);
}

int
ksocknal_reaper (void *arg)
{
        cfs_waitlink_t     wait;
        ksock_conn_t      *conn;
        ksock_sched_t     *sched;
        cfs_list_t         enomem_conns;
        int                nenomem_conns;
        cfs_duration_t     timeout;
        int                i;
        int                peer_index = 0;
        cfs_time_t         deadline = cfs_time_current();

        cfs_daemonize ("socknal_reaper");
        cfs_block_allsigs ();

        CFS_INIT_LIST_HEAD(&enomem_conns);
        cfs_waitlink_init (&wait);

        cfs_spin_lock_bh (&ksocknal_data.ksnd_reaper_lock);

        while (!ksocknal_data.ksnd_shuttingdown) {

                if (!cfs_list_empty (&ksocknal_data.ksnd_deathrow_conns)) {
                        conn = cfs_list_entry (ksocknal_data. \
                                               ksnd_deathrow_conns.next,
                                               ksock_conn_t, ksnc_list);
                        cfs_list_del (&conn->ksnc_list);

                        cfs_spin_unlock_bh (&ksocknal_data.ksnd_reaper_lock);

                        ksocknal_terminate_conn (conn);
                        ksocknal_conn_decref(conn);

                        cfs_spin_lock_bh (&ksocknal_data.ksnd_reaper_lock);
                        continue;
                }

                if (!cfs_list_empty (&ksocknal_data.ksnd_zombie_conns)) {
                        conn = cfs_list_entry (ksocknal_data.ksnd_zombie_conns.\
                                               next, ksock_conn_t, ksnc_list);
                        cfs_list_del (&conn->ksnc_list);

                        cfs_spin_unlock_bh (&ksocknal_data.ksnd_reaper_lock);

                        ksocknal_destroy_conn (conn);

                        cfs_spin_lock_bh (&ksocknal_data.ksnd_reaper_lock);
                        continue;
                }

                if (!cfs_list_empty (&ksocknal_data.ksnd_enomem_conns)) {
                        cfs_list_add(&enomem_conns,
                                     &ksocknal_data.ksnd_enomem_conns);
                        cfs_list_del_init(&ksocknal_data.ksnd_enomem_conns);
                }

                cfs_spin_unlock_bh (&ksocknal_data.ksnd_reaper_lock);

                /* reschedule all the connections that stalled with ENOMEM... */
                nenomem_conns = 0;
                while (!cfs_list_empty (&enomem_conns)) {
                        conn = cfs_list_entry (enomem_conns.next,
                                               ksock_conn_t, ksnc_tx_list);
                        cfs_list_del (&conn->ksnc_tx_list);

                        sched = conn->ksnc_scheduler;

                        cfs_spin_lock_bh (&sched->kss_lock);

                        LASSERT (conn->ksnc_tx_scheduled);
                        conn->ksnc_tx_ready = 1;
                        cfs_list_add_tail(&conn->ksnc_tx_list,
                                          &sched->kss_tx_conns);
                        cfs_waitq_signal (&sched->kss_waitq);

                        cfs_spin_unlock_bh (&sched->kss_lock);
                        nenomem_conns++;
                }

                /* careful with the jiffy wrap... */
                while ((timeout = cfs_time_sub(deadline,
                                               cfs_time_current())) <= 0) {
                        const int n = 4;
                        const int p = 1;
                        int       chunk = ksocknal_data.ksnd_peer_hash_size;

                        /* Time to check for timeouts on a few more peers: I do
                         * checks every 'p' seconds on a proportion of the peer
                         * table and I need to check every connection 'n' times
                         * within a timeout interval, to ensure I detect a
                         * timeout on any connection within (n+1)/n times the
                         * timeout interval. */

                        if (*ksocknal_tunables.ksnd_timeout > n * p)
                                chunk = (chunk * n * p) /
                                        *ksocknal_tunables.ksnd_timeout;
                        if (chunk == 0)
                                chunk = 1;

                        for (i = 0; i < chunk; i++) {
                                ksocknal_check_peer_timeouts (peer_index);
                                peer_index = (peer_index + 1) %
                                             ksocknal_data.ksnd_peer_hash_size;
                        }

                        deadline = cfs_time_add(deadline, cfs_time_seconds(p));
                }

                if (nenomem_conns != 0) {
                        /* Reduce my timeout if I rescheduled ENOMEM conns.
                         * This also prevents me getting woken immediately
                         * if any go back on my enomem list. */
                        timeout = SOCKNAL_ENOMEM_RETRY;
                }
                ksocknal_data.ksnd_reaper_waketime =
                        cfs_time_add(cfs_time_current(), timeout);

                cfs_set_current_state (CFS_TASK_INTERRUPTIBLE);
                cfs_waitq_add (&ksocknal_data.ksnd_reaper_waitq, &wait);

                if (!ksocknal_data.ksnd_shuttingdown &&
                    cfs_list_empty (&ksocknal_data.ksnd_deathrow_conns) &&
                    cfs_list_empty (&ksocknal_data.ksnd_zombie_conns))
                        cfs_waitq_timedwait (&wait, CFS_TASK_INTERRUPTIBLE,
                                             timeout);

                cfs_set_current_state (CFS_TASK_RUNNING);
                cfs_waitq_del (&ksocknal_data.ksnd_reaper_waitq, &wait);

                cfs_spin_lock_bh (&ksocknal_data.ksnd_reaper_lock);
        }

        cfs_spin_unlock_bh (&ksocknal_data.ksnd_reaper_lock);

        ksocknal_thread_fini ();
        return (0);
}
