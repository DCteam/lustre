/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef _ECHO_INTERNAL_H
#define _ECHO_INTERNAL_H

/* The persistent object (i.e. actually stores stuff!) */
#define ECHO_PERSISTENT_OBJID    1ULL
#define ECHO_PERSISTENT_SIZE     ((__u64)(1<<20))

/* block size to use for data verification */
#define OBD_ECHO_BLOCK_SIZE	(4<<10)

#ifndef __KERNEL__
/* Kludge here, define some functions and macros needed by liblustre -jay */
static inline void page_cache_get(struct page *page)
{
}

static inline void page_cache_release(struct page *page)
{
}

#define READ    0
#define WRITE   1

#endif /* ifndef __KERNEL__ */

#endif
