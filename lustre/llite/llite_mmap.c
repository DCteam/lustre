/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
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

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/version.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <asm/segment.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
#include <linux/iobuf.h>
#endif


#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/lustre_mds.h>
#include <linux/lustre_lite.h>
#include "llite_internal.h"
#include <linux/lustre_compat25.h>

#define VMA_DEBUG(vma, fmt, arg...)                                     \
        CDEBUG(D_MMAP, "vma(%p) start(%ld) end(%ld) pgoff(%ld) inode(%p) "   \
               "ino(%lu) iname(%s): " fmt, vma, vma->vm_start, vma->vm_end,  \
               vma->vm_pgoff, vma->vm_file->f_dentry->d_inode,               \
               vma->vm_file->f_dentry->d_inode->i_ino,                       \
               vma->vm_file->f_dentry->d_iname, ## arg);                     \


struct ll_lock_tree_node {
        rb_node_t               lt_node;
        struct list_head        lt_locked_item;
        __u64                   lt_oid;
        ldlm_policy_data_t      lt_policy;
        struct lustre_handle    lt_lockh;
        ldlm_mode_t             lt_mode;
        struct inode           *lt_inode;
};

__u64 lov_merge_size(struct lov_stripe_md *lsm, int kms);
int lt_get_mmap_locks(struct ll_lock_tree *tree,
                      unsigned long addr, size_t count);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
struct page *ll_nopage(struct vm_area_struct *vma, unsigned long address,
                       int *type);
#else

struct page *ll_nopage(struct vm_area_struct *vma, unsigned long address,
                       int unused);
#endif

struct ll_lock_tree_node * ll_node_from_inode(struct inode *inode, __u64 start,
                                              __u64 end, ldlm_mode_t mode)
{
        struct ll_lock_tree_node *node;

        OBD_ALLOC(node, sizeof(*node));
        if (node == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        node->lt_inode = inode;
        node->lt_oid = ll_i2info(inode)->lli_smd->lsm_object_id;
        node->lt_policy.l_extent.start = start;
        node->lt_policy.l_extent.end = end;
        memset(&node->lt_lockh, 0, sizeof(node->lt_lockh));
        INIT_LIST_HEAD(&node->lt_locked_item);
        node->lt_mode = mode;

        return node;
}

int lt_compare(struct ll_lock_tree_node *one, struct ll_lock_tree_node *two)
{
        /* To avoid multiple fs deadlock */
        if (one->lt_inode->i_sb->s_dev < two->lt_inode->i_sb->s_dev)
                return -1;
        if (one->lt_inode->i_sb->s_dev > two->lt_inode->i_sb->s_dev)
                return 1;

        if (one->lt_oid < two->lt_oid)
                return -1;
        if (one->lt_oid > two->lt_oid)
                return 1;

        if (one->lt_policy.l_extent.end < two->lt_policy.l_extent.start)
                return -1;
        if (one->lt_policy.l_extent.start > two->lt_policy.l_extent.end)
                return 1;

        return 0; /* they are the same object and overlap */
}

static void lt_merge(struct ll_lock_tree_node *dst,
                     struct ll_lock_tree_node *src)
{
        dst->lt_policy.l_extent.start = min(dst->lt_policy.l_extent.start,
                                            src->lt_policy.l_extent.start);
        dst->lt_policy.l_extent.end = max(dst->lt_policy.l_extent.end,
                                          src->lt_policy.l_extent.end);

        /* XXX could be a real call to the dlm to find superset modes */
        if (src->lt_mode == LCK_PW && dst->lt_mode != LCK_PW)
                dst->lt_mode = LCK_PW;
}

static void lt_insert(struct ll_lock_tree *tree,
                      struct ll_lock_tree_node *node)
{
        struct ll_lock_tree_node *walk;
        rb_node_t **p, *parent;
        ENTRY;

restart:
        p = &tree->lt_root.rb_node;
        parent = NULL;
        while (*p) {
                parent = *p;
                walk = rb_entry(parent, struct ll_lock_tree_node, lt_node);
                switch (lt_compare(node, walk)) {
                case -1:
                        p = &(*p)->rb_left;
                        break;
                case 1:
                        p = &(*p)->rb_right;
                        break;
                case 0:
                        lt_merge(node, walk);
                        rb_erase(&walk->lt_node, &tree->lt_root);
                        OBD_FREE(walk, sizeof(*walk));
                        goto restart;
                        break;
                default:
                        LBUG();
                        break;
                }
        }
        rb_link_node(&node->lt_node, parent, p);
        rb_insert_color(&node->lt_node, &tree->lt_root);
        EXIT;
}

static struct ll_lock_tree_node *lt_least_node(struct ll_lock_tree *tree)
{
        rb_node_t *rbnode;
        struct ll_lock_tree_node *node = NULL;

        for ( rbnode = tree->lt_root.rb_node; rbnode != NULL;
              rbnode = rbnode->rb_left) {
                if (rbnode->rb_left == NULL) {
                        node = rb_entry(rbnode, struct ll_lock_tree_node,
                                        lt_node);
                        break;
                }
        }
        RETURN(node);
}

int ll_tree_unlock(struct ll_lock_tree *tree)
{
        struct ll_lock_tree_node *node;
        struct list_head *pos, *n;
        struct inode *inode;
        int rc = 0;
        ENTRY;

        list_for_each_safe(pos, n, &tree->lt_locked_list) {
                node = list_entry(pos, struct ll_lock_tree_node,
                                  lt_locked_item);

                inode = node->lt_inode;
                rc = ll_extent_unlock(tree->lt_fd, inode,
                                      ll_i2info(inode)->lli_smd, node->lt_mode,
                                      &node->lt_lockh);
                if (rc != 0) {
                        /* XXX better message */
                        CERROR("couldn't unlock %d\n", rc);
                }
                list_del(&node->lt_locked_item);
                OBD_FREE(node, sizeof(*node));
        }

        while ((node = lt_least_node(tree))) {
                rb_erase(&node->lt_node, &tree->lt_root);
                OBD_FREE(node, sizeof(*node));
        }

        RETURN(rc);
}

int ll_tree_lock(struct ll_lock_tree *tree,
                 struct ll_lock_tree_node *first_node,
                 const char *buf, size_t count, int ast_flags)
{
        struct ll_lock_tree_node *node;
        int rc = 0;
        ENTRY;

        tree->lt_root.rb_node = NULL;
        INIT_LIST_HEAD(&tree->lt_locked_list);
        if (first_node != NULL)
                lt_insert(tree, first_node);

        /* To avoid such subtle deadlock case: client1 try to read file1 to
         * mmapped file2, on the same time, client2 try to read file2 to
         * mmapped file1.*/
        rc = lt_get_mmap_locks(tree, (unsigned long)buf, count);
        if (rc)
                GOTO(out, rc);

        while ((node = lt_least_node(tree))) {
                struct inode *inode = node->lt_inode;
                rc = ll_extent_lock(tree->lt_fd, inode,
                                    ll_i2info(inode)->lli_smd, node->lt_mode,
                                    &node->lt_policy, &node->lt_lockh,
                                    ast_flags);
                if (rc != 0)
                        GOTO(out, rc);

                rb_erase(&node->lt_node, &tree->lt_root);
                list_add_tail(&node->lt_locked_item, &tree->lt_locked_list);
        }
        RETURN(rc);
out:
        ll_tree_unlock(tree);
        RETURN(rc);
}

static ldlm_mode_t mode_from_vma(struct vm_area_struct *vma)
{
        /* we only want to hold PW locks if the mmap() can generate
         * writes back to the file and that only happens in shared
         * writable vmas */
        if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
                return LCK_PW;
        return LCK_PR;
}

static void policy_from_vma(ldlm_policy_data_t *policy,
                            struct vm_area_struct *vma, unsigned long addr,
                            size_t count)
{
        policy->l_extent.start = ((addr - vma->vm_start) & PAGE_CACHE_MASK) +
                                 (vma->vm_pgoff << PAGE_CACHE_SHIFT);
        policy->l_extent.end = (policy->l_extent.start + count - 1) |
                               (PAGE_CACHE_SIZE - 1);
}

static struct vm_area_struct * our_vma(unsigned long addr, size_t count)
{
        struct mm_struct *mm = current->mm;
        struct vm_area_struct *vma, *ret = NULL;
        ENTRY;

        /* No MM (e.g. NFS)? No vmas too. */
        if (!mm)
                RETURN(NULL);

        spin_lock(&mm->page_table_lock);
        for(vma = find_vma(mm, addr);
            vma != NULL && vma->vm_start < (addr + count); vma = vma->vm_next) {
                if (vma->vm_ops && vma->vm_ops->nopage == ll_nopage &&
                    vma->vm_flags & VM_SHARED) {
                        ret = vma;
                        break;
                }
        }
        spin_unlock(&mm->page_table_lock);
        RETURN(ret);
}

int lt_get_mmap_locks(struct ll_lock_tree *tree,
                      unsigned long addr, size_t count)
{
        struct vm_area_struct *vma;
        struct ll_lock_tree_node *node;
        ldlm_policy_data_t policy;
        struct inode *inode;
        ENTRY;

        if (count == 0)
                RETURN(0);

        /* we need to look up vmas on page aligned addresses */
        count += addr & (PAGE_SIZE - 1);
        addr &= PAGE_MASK;

        while ((vma = our_vma(addr, count)) != NULL) {
                LASSERT(vma->vm_file);

                inode = vma->vm_file->f_dentry->d_inode;
                policy_from_vma(&policy, vma, addr, count);
                node = ll_node_from_inode(inode, policy.l_extent.start,
                                          policy.l_extent.end,
                                          mode_from_vma(vma));
                if (IS_ERR(node)) {
                        CERROR("not enough mem for lock_tree_node!\n");
                        RETURN(-ENOMEM);
                }
                lt_insert(tree, node);

                if (vma->vm_end - addr >= count)
                        break;
                count -= vma->vm_end - addr;
                addr = vma->vm_end;
        }
        RETURN(0);
}

/* FIXME: there is a pagefault race goes as follow (only 2.4):
 * 1. A user process on node A accesses a portion of a mapped file,
 *    resulting in a page fault.  The pagefault handler invokes the
 *    ll_nopage function, which reads the page into memory.
 * 2. A user process on node B writes to the same portion of the file
 *    (either via mmap or write()), that cause node A to cancel the
 *    lock and truncate the page.
 * 3. Node A then executes the rest of do_no_page(), entering the
 *    now-invalid page into the PTEs.
 *
 * Make the whole do_no_page as a hook to cover both the page cache
 * and page mapping installing with dlm lock would eliminate this race.
 *
 * In 2.6, the truncate_count of address_space can cover this race.
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
struct page *ll_nopage(struct vm_area_struct *vma, unsigned long address,
                       int *type)
#else
struct page *ll_nopage(struct vm_area_struct *vma, unsigned long address,
                       int type /* unused */)
#endif
{
        struct file *filp = vma->vm_file;
        struct ll_file_data *fd = filp->private_data;
        struct inode *inode = filp->f_dentry->d_inode;
        struct lustre_handle lockh = { 0 };
        ldlm_policy_data_t policy;
        ldlm_mode_t mode;
        struct page *page = NULL;
        struct ll_inode_info *lli = ll_i2info(inode);
        __u64 kms, old_mtime;
        unsigned long pgoff, size, rand_read, seq_read;
        int rc = 0;
        ENTRY;

        if (lli->lli_smd == NULL) {
                CERROR("No lsm on fault?\n");
                RETURN(NULL);
        }

        /* start and end the lock on the first and last bytes in the page */
        policy_from_vma(&policy, vma, address, PAGE_CACHE_SIZE);

        CDEBUG(D_MMAP, "nopage vma %p inode %lu, locking ["LPU64", "LPU64"]\n",
               vma, inode->i_ino, policy.l_extent.start, policy.l_extent.end);

        mode = mode_from_vma(vma);
        old_mtime = LTIME_S(inode->i_mtime);

        rc = ll_extent_lock(fd, inode, lli->lli_smd, mode, &policy,
                            &lockh, LDLM_FL_CBPENDING | LDLM_FL_NO_LRU);
        if (rc != 0)
                RETURN(NULL);

        if (vma->vm_flags & VM_EXEC && LTIME_S(inode->i_mtime) != old_mtime)
                CWARN("binary changed. inode %lu\n", inode->i_ino);

        /* XXX change inode size without i_sem hold! there is a race condition
         *     with truncate path. (see ll_extent_lock) */
        //down(&lli->lli_size_sem);
        kms = lov_merge_size(lli->lli_smd, 1);
        pgoff = ((address - vma->vm_start) >> PAGE_CACHE_SHIFT) + vma->vm_pgoff;
        size = (kms + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;

        if (pgoff >= size) {
                //up(&lli->lli_size_sem);
                ll_glimpse_size(inode);
        } else {
                inode->i_size = kms;
                //up(&lli->lli_size_sem);
        }

        /* disable VM_SEQ_READ and use VM_RAND_READ to make sure that
         * the kernel will not read other pages not covered by ldlm in
         * filemap_nopage. we do our readahead in ll_readpage.
         */
        rand_read = vma->vm_flags & VM_RAND_READ;
        seq_read = vma->vm_flags & VM_SEQ_READ;
        vma->vm_flags &= ~ VM_SEQ_READ;
        vma->vm_flags |= VM_RAND_READ;

        page = filemap_nopage(vma, address, type);
        LL_CDEBUG_PAGE(D_PAGE, page, "got addr %lu type %lx\n", address,
                       (long)type);
        vma->vm_flags &= ~VM_RAND_READ;
        vma->vm_flags |= (rand_read | seq_read);

        ll_extent_unlock(fd, inode, ll_i2info(inode)->lli_smd, mode, &lockh);
        RETURN(page);
}

/* To avoid cancel the locks covering mmapped region for lock cache pressure,
 * we track the mapped vma count by lli_mmap_cnt.
 * ll_vm_open():  when first vma is linked, split locks from lru.
 * ll_vm_close(): when last vma is unlinked, join all this file's locks to lru.
 *
 * XXX we don't check the if the region of vma/lock for performance.
 */
static void ll_vm_open(struct vm_area_struct * vma)
{
        struct inode *inode = vma->vm_file->f_dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        LASSERT(vma->vm_file);

        spin_lock(&lli->lli_lock);
        LASSERT(atomic_read(&lli->lli_mmap_cnt) >= 0);

        atomic_inc(&lli->lli_mmap_cnt);
        if (atomic_read(&lli->lli_mmap_cnt) == 1) {
                struct lov_stripe_md *lsm = lli->lli_smd;
                struct ll_sb_info *sbi = ll_i2sbi(inode);
                int count;

                spin_unlock(&lli->lli_lock);
                count = obd_join_lru(sbi->ll_osc_exp, lsm, 0);
                VMA_DEBUG(vma, "split %d unused locks from lru\n", count);
        } else {
                spin_unlock(&lli->lli_lock);
        }

}

static void ll_vm_close(struct vm_area_struct *vma)
{
        struct inode *inode = vma->vm_file->f_dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        LASSERT(vma->vm_file);

        spin_lock(&lli->lli_lock);
        LASSERT(atomic_read(&lli->lli_mmap_cnt) > 0);

        atomic_dec(&lli->lli_mmap_cnt);
        if (atomic_read(&lli->lli_mmap_cnt) == 0) {
                struct lov_stripe_md *lsm = lli->lli_smd;
                struct ll_sb_info *sbi = ll_i2sbi(inode);
                int count;

                spin_unlock(&lli->lli_lock);
                count = obd_join_lru(sbi->ll_osc_exp, lsm, 1);
                VMA_DEBUG(vma, "join %d unused locks to lru\n", count);
        } else {
                spin_unlock(&lli->lli_lock);
        }
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
static int ll_populate(struct vm_area_struct *area, unsigned long address,
                       unsigned long len, pgprot_t prot, unsigned long pgoff,
                       int nonblock)
{
        int rc = 0;
        ENTRY;

        /* always set nonblock as true to avoid page read ahead */
        rc = filemap_populate(area, address, len, prot, pgoff, 1);
        RETURN(rc);
}
#endif

/* return the user space pointer that maps to a file offset via a vma */
static inline unsigned long file_to_user(struct vm_area_struct *vma, __u64 byte)
{
        return vma->vm_start + (byte - ((__u64)vma->vm_pgoff << PAGE_SHIFT));

}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
/* [first, last] are the byte offsets affected.
 * vm_{start, end} are user addresses of the first byte of the mapping and
 *      the next byte beyond it
 * vm_pgoff is the page index of the first byte in the mapping */
static void teardown_vmas(struct vm_area_struct *vma, __u64 first,
                          __u64 last)
{
        unsigned long address, len;
        for (; vma ; vma = vma->vm_next_share) {
                if (last >> PAGE_SHIFT < vma->vm_pgoff)
                        continue;
                if (first >> PAGE_SHIFT >= (vma->vm_pgoff +
                    ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT)))
                        continue;

                /* XXX in case of unmap the cow pages of a running file,
                 * don't unmap these private writeable mapping here!
                 * though that will break private mappping a little.
                 *
                 * the clean way is to check the mapping of every page
                 * and just unmap the non-cow pages, just like
                 * unmap_mapping_range() with even_cow=0 in kernel 2.6.
                 */
                if (!(vma->vm_flags & VM_SHARED) &&
                    (vma->vm_flags & VM_WRITE))
                        continue;

                address = max((unsigned long)vma->vm_start,
                              file_to_user(vma, first));
                len = min((unsigned long)vma->vm_end,
                          file_to_user(vma, last) + 1) - address;

                VMA_DEBUG(vma, "zapping vma [first="LPU64" last="LPU64" "
                          "address=%ld len=%ld]\n", first, last, address, len);
                LASSERT(len > 0);
                ll_zap_page_range(vma, address, len);
        }
}
#endif

/* XXX put nice comment here.  talk about __free_pte -> dirty pages and
 * nopage's reference passing to the pte */
int ll_teardown_mmaps(struct address_space *mapping, __u64 first, __u64 last)
{
        int rc = -ENOENT;
        ENTRY;

        LASSERTF(last > first, "last "LPU64" first "LPU64"\n", last, first);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
        if (mapping_mapped(mapping)) {
                rc = 0;
                unmap_mapping_range(mapping, first + PAGE_SIZE - 1,
                                    last - first + 1, 0);
        }
#else
        spin_lock(&mapping->i_shared_lock);
        if (mapping->i_mmap != NULL) {
                rc = 0;
                teardown_vmas(mapping->i_mmap, first, last);
        }
        if (mapping->i_mmap_shared != NULL) {
                rc = 0;
                teardown_vmas(mapping->i_mmap_shared, first, last);
        }
        spin_unlock(&mapping->i_shared_lock);
#endif
        RETURN(rc);
}

static struct vm_operations_struct ll_file_vm_ops = {
        .nopage         = ll_nopage,
        .open           = ll_vm_open,
        .close          = ll_vm_close,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
        .populate       = ll_populate,
#endif
};

int ll_file_mmap(struct file * file, struct vm_area_struct * vma)
{
        int rc;
        ENTRY;

        rc = generic_file_mmap(file, vma);
        if (rc == 0) {
                vma->vm_ops = &ll_file_vm_ops;
                vma->vm_ops->open(vma);
                /* update the inode's size and mtime */
                rc = ll_glimpse_size(file->f_dentry->d_inode);
        }

        RETURN(rc);
}
