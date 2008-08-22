
#include <linux/backing-dev.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>	/* generic_writepages */
#include <linux/pagevec.h>
#include <linux/task_io_accounting_ops.h>

int ceph_debug_addr = -1;
#define DOUT_VAR ceph_debug_addr
#define DOUT_PREFIX "addr: "
#include "super.h"

#include "osd_client.h"

/*
 * if snapc is NULL, we are dirtying within the most recent (head) snap.
 * if snapc is non-NULL, we are redirtying a page withing a past snap,
 * and need to adjust the count on the appropriate ceph_cap_snap.
 */
static int ceph_set_page_dirty(struct page *page,
			       struct ceph_snap_context *snapc)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode;
	struct ceph_inode_info *ci;
	int undo = 0;

	if (unlikely(!mapping))
		return !TestSetPageDirty(page);

	if (TestSetPageDirty(page)) {
		dout(20, "%p set_page_dirty %p -- already dirty\n",
		     mapping->host, page);
		return 0;
	}

	/*
	 * optimistically adjust accounting, on the assumption that
	 * we won't race with invalidate.
	 */
	inode = mapping->host;
	ci = ceph_inode(inode);

	spin_lock(&inode->i_lock);
	++ci->i_wrbuffer_ref;
	if (!snapc || snapc == ci->i_snap_realm->cached_context) {
		/* dirty the head */
		++ci->i_wrbuffer_ref_head;
		snapc = ceph_get_snap_context(ci->i_snap_realm->cached_context);
		dout(20, "%p set_page_dirty %p head %d/%d -> %d/%d "
		     "snapc %p seq %lld (%d snaps)\n",
		     mapping->host, page,
		     ci->i_wrbuffer_ref-1, ci->i_wrbuffer_ref_head-1,
		     ci->i_wrbuffer_ref, ci->i_wrbuffer_ref_head,
		     snapc, snapc->seq, snapc->num_snaps);
	} else {
		struct list_head *p;
		struct ceph_cap_snap *capsnap = 0;
		list_for_each(p, &ci->i_cap_snaps) {
			capsnap = list_entry(p, struct ceph_cap_snap,
					     ci_item);
			if (capsnap->context == snapc) {
				capsnap->dirty++;
				break;
			}
		}
		BUG_ON(!capsnap);
		dout(20, "%p set_page_dirty %p snap %lld %d/%d -> %d/%d"
		     " snapc %p seq %lld (%d snaps)\n",
		     mapping->host, page, capsnap->follows,
		     ci->i_wrbuffer_ref-1, capsnap->dirty-1,
		     ci->i_wrbuffer_ref, capsnap->dirty,
		     snapc, snapc->seq, snapc->num_snaps);
	}
	spin_unlock(&inode->i_lock);

	/* now adjust page */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	spin_lock_irq(&mapping->tree_lock);
#else
	write_lock_irq(&mapping->tree_lock);
#endif
	if (page->mapping) {	/* Race with truncate? */
		WARN_ON_ONCE(!PageUptodate(page));

		if (mapping_cap_account_dirty(mapping)) {
			__inc_zone_page_state(page, NR_FILE_DIRTY);
			__inc_bdi_stat(mapping->backing_dev_info,
					BDI_RECLAIMABLE);
			task_io_account_write(PAGE_CACHE_SIZE);
		}
		radix_tree_tag_set(&mapping->page_tree,
				page_index(page), PAGECACHE_TAG_DIRTY);

		/*
		 * reference snap context in page->private.  also set
		 * PagePrivate so that we get invalidatepage callback
		 * on truncate for dirty page accounting for mmap.
		 */
		ceph_put_snap_context((void *)page->private);
		page->private = (unsigned long)snapc;
		SetPagePrivate(page);
	} else {
		dout(20, "ANON set_page_dirty %p (raced truncate?)\n", page);
		undo = 1;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	spin_unlock_irq(&mapping->tree_lock);
#else
	write_unlock_irq(&mapping->tree_lock);
#endif

	if (undo)
		ceph_put_wrbuffer_cap_refs(ci, 1, snapc);

	__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);

	return 1;
}

static void ceph_redirty_page(struct address_space *mapping, struct page *page)
{
	BUG_ON(!PageLocked(page));

	if (TestSetPageDirty(page)) {
		dout(20, "%p redirty_page %p -- already dirty\n",
		     mapping->host, page);
		return;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	spin_lock_irq(&mapping->tree_lock);
#else
	write_lock_irq(&mapping->tree_lock);
#endif
	if (page->mapping) {	/* Race with truncate? */
		if (mapping_cap_account_dirty(mapping)) {
			__inc_zone_page_state(page,
					      NR_FILE_DIRTY);
			__inc_bdi_stat(mapping->backing_dev_info,
				       BDI_RECLAIMABLE);
			task_io_account_write(PAGE_CACHE_SIZE);
		}
		radix_tree_tag_set(&mapping->page_tree,
				   page_index(page),
				   PAGECACHE_TAG_DIRTY);
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	spin_unlock_irq(&mapping->tree_lock);
#else
	write_unlock_irq(&mapping->tree_lock);
#endif
}


static int ceph_set_page_dirty_vfs(struct page *page)
{
	return ceph_set_page_dirty(page, 0);
}

static void ceph_invalidatepage(struct page *page, unsigned long offset)
{
	struct inode *inode;
	struct ceph_inode_info *ci;
	struct ceph_snap_context *snapc = (void *)page->private;

	BUG_ON(!PageLocked(page));
	if (offset == 0)
		ClearPageChecked(page);
	if (!PageDirty(page)) {
		BUG_ON(snapc);
		return;
	}
	if (!page->mapping) {
		BUG_ON(snapc);
		return;
	}
	inode = page->mapping->host;
	ci = ceph_inode(inode);
	if (offset == 0) {
		dout(20, "%p invalidatepage %p idx %lu full dirty page %lu\n",
		     &ci->vfs_inode, page, page->index, offset);
		ceph_put_wrbuffer_cap_refs(ci, 1, snapc);
		ceph_put_snap_context(snapc);
		page->private = 0;
		ClearPagePrivate(page);
	} else
		dout(20, "%p invalidatepage %p idx %lu partial dirty page\n",
		     &ci->vfs_inode, page, page->index);
}

static int ceph_releasepage(struct page *page, gfp_t g)
{
	struct inode *inode = page->mapping ? page->mapping->host:0;
	dout(20, "%p releasepage %p idx %lu\n", inode, page, page->index);
	WARN_ON(PageDirty(page));
	WARN_ON(page->private);
	WARN_ON(PagePrivate(page));
	return 0;
}


static int ceph_readpage(struct file *filp, struct page *page)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_osd_client *osdc = &ceph_inode_to_client(inode)->osdc;
	int err = 0;

	dout(10, "readpage inode %p file %p page %p index %lu\n",
	     inode, filp, page, page->index);
	err = ceph_osdc_readpage(osdc, ceph_vino(inode), &ci->i_layout,
				 page->index << PAGE_SHIFT, PAGE_SIZE, page);
	if (unlikely(err < 0))
		goto out;

	if (unlikely(err < PAGE_CACHE_SIZE)) {
		void *kaddr = kmap_atomic(page, KM_USER0);
		dout(10, "readpage zeroing tail %d bytes of page %p\n",
		     (int)PAGE_CACHE_SIZE - err, page);
		memset(kaddr + err, 0, PAGE_CACHE_SIZE - err);
		kunmap_atomic(kaddr, KM_USER0);
	}
	SetPageUptodate(page);

out:
	unlock_page(page);
	return err;
}

static int ceph_readpages(struct file *file, struct address_space *mapping,
			  struct list_head *page_list, unsigned nr_pages)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_osd_client *osdc = &ceph_inode_to_client(inode)->osdc;
	int rc = 0;
	struct page *page;
	struct pagevec pvec;
	loff_t offset;

	dout(10, "readpages inode %p file %p nr_pages %d\n",
	     inode, file, nr_pages);

	/* guess read extent */
	BUG_ON(list_empty(page_list));
	page = list_entry(page_list->prev, struct page, lru);
	offset = page->index << PAGE_CACHE_SHIFT;
	rc = ceph_osdc_readpages(osdc, mapping, ceph_vino(inode), &ci->i_layout,
				 offset, nr_pages << PAGE_CACHE_SHIFT,
				 page_list, nr_pages);
	if (rc < 0)
		return rc;

	/* set uptodate and add to lru in pagevec-sized chunks */
	pagevec_init(&pvec, 0);
	if (rc > 0)
		rc += offset & ~PAGE_CACHE_MASK;
	for (; rc > 0; rc -= PAGE_CACHE_SIZE) {
		if (list_empty(page_list))
			break;  /* WTF */
		page = list_entry(page_list->prev, struct page, lru);
		list_del(&page->lru);

		if (add_to_page_cache(page, mapping, page->index,
				      GFP_NOFS)) {
			page_cache_release(page);
			dout(20, "readpages add_to_page_cache failed on %p\n",
			     page);
			continue;
		}
		dout(10, "readpages adding page %p\n", page);
		flush_dcache_page(page);
		SetPageUptodate(page);
		unlock_page(page);
		if (pagevec_add(&pvec, page) == 0)
			pagevec_lru_add(&pvec);
	}
	pagevec_lru_add(&pvec);
	return 0;
}

/*
 * ceph_writepage:
 *  clear dirty page, and set the writeback flag in the radix tree.
 *  to actually write data to the remote OSDs.
 * leave pages locked.
 */
static int writepage_nounlock(struct page *page, struct writeback_control *wbc)
{
	struct inode *inode;
	struct ceph_inode_info *ci;
	struct ceph_osd_client *osdc;
	loff_t page_off = page->index << PAGE_CACHE_SHIFT;
	int len = PAGE_CACHE_SIZE;
	loff_t i_size;
	int err = 0;
	int was_dirty;
	struct ceph_snap_context *snapc;

	if (!page->mapping || !page->mapping->host)
		return -EFAULT;
	inode = page->mapping->host;
	ci = ceph_inode(inode);
	osdc = &ceph_inode_to_client(inode)->osdc;

	/* is this a partial page at end of file? */
	i_size = i_size_read(inode);
	if (i_size < page_off + len)
		len = i_size - page_off;

	dout(10, "ceph_writepage inode %p page %p index %lu on %llu~%u\n",
	     inode, page, page->index, page_off, len);

	page_cache_get(page);
	was_dirty = PageDirty(page);
	snapc = (void *)page->private;
	set_page_writeback(page);
	err = ceph_osdc_writepages(osdc, ceph_vino(inode), &ci->i_layout,
				   (void *)page->private,
				   page_off, len, &page, 1);
	if (err >= 0) {
		if (was_dirty) {
			dout(20, "cleaned page %p\n", page);
			ceph_put_wrbuffer_cap_refs(ci, 1, snapc);
		}
		SetPageUptodate(page);
		err = 0;  /* vfs expects us to return 0 */
	} else {
		if (wbc)
			wbc->pages_skipped++;
		ceph_set_page_dirty(page, snapc);
	}
	end_page_writeback(page);
	page_cache_release(page);
	return err;
}

static int ceph_writepage(struct page *page, struct writeback_control *wbc)
{
	int err = writepage_nounlock(page, wbc);
	unlock_page(page);
	return err;
}


/*
 * lame release_pages helper.  release_pages() isn't exported to
 * modules.
 */
void ceph_release_pages(struct page **pages, int num)
{
	struct pagevec pvec;
	int i;
	pagevec_init(&pvec, 0);
	for (i = 0; i < num; i++) {
		if (pagevec_add(&pvec, pages[i]) == 0)
			pagevec_release(&pvec);
	}
	pagevec_release(&pvec);
}

/*
 * ceph_writepages:
 *  do write jobs for several pages
 */
static int ceph_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_client *client = ceph_inode_to_client(inode);
	pgoff_t index, end;
	int range_whole = 0;
	int should_loop = 1;
	struct page **pages;
	pgoff_t max_pages = 0;
	struct ceph_snap_context *snapc = 0;
	struct pagevec pvec;
	int done = 0;
	int rc = 0;
	unsigned wsize = 1 << inode->i_blkbits;
	struct list_head *p;
	struct ceph_cap_snap *capsnap = 0;

	if (client->mount_args.wsize && client->mount_args.wsize < wsize)
		wsize = client->mount_args.wsize;
	if (wsize < PAGE_CACHE_SIZE)
		wsize = PAGE_CACHE_SIZE;

	dout(10, "writepages on %p, wsize %u\n", inode, wsize);

	/* larger page vector? */
	max_pages = wsize >> PAGE_CACHE_SHIFT;
	pages = kmalloc(max_pages * sizeof(*pages), GFP_NOFS);
	if (!pages)
		return generic_writepages(mapping, wbc);
	pagevec_init(&pvec, 0);

	/* ?? from cifs. */
	/*
	if (wbc->nonblocking && bdi_write_congested(bdi)) {
		wbc->encountered_congestions = 1;
		return 0;
	}
	*/

	/* where to start? */
	if (wbc->range_cyclic) {
		index = mapping->writeback_index; /* Start from prev offset */
		end = -1;
		dout(10, "cyclic, start at %lu\n", index);
	} else {
		index = wbc->range_start >> PAGE_CACHE_SHIFT;
		end = wbc->range_end >> PAGE_CACHE_SHIFT;
		if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
			range_whole = 1;
		should_loop = 0;
		dout(10, "not cyclic, %lu to %lu\n", index, end);
	}


retry:
	/* find oldest snap context with dirty data */
	ceph_put_snap_context(snapc);
	snapc = 0;

	spin_lock(&inode->i_lock);
	list_for_each(p, &ci->i_cap_snaps) {
		capsnap = list_entry(p, struct ceph_cap_snap, ci_item);
		dout(20, " cap_snap %p has %d dirty\n", capsnap,
		     capsnap->dirty);
		if (capsnap->dirty)
			break;
	}
	if (capsnap && capsnap->dirty) {
		snapc = ceph_get_snap_context(capsnap->context);
	} else if (ci->i_snap_realm) {
		snapc = ceph_get_snap_context(ci->i_snap_realm->cached_context);
		dout(20, " %d head wrbuffer refs\n", ci->i_wrbuffer_ref_head);
	}
	spin_unlock(&inode->i_lock);

	if (!snapc) {
		/* hmm, why does writepages get called when there
		   is no dirty data? */
		dout(20, " no snap context with dirty data?\n");
		goto out;
	}
	dout(20, " snapc is %p seq %lld (%d snaps)\n",
	     snapc, snapc->seq, snapc->num_snaps);

	while (!done && index <= end) {
		unsigned i;
		int first;
		pgoff_t next;
		int pvec_pages, locked_pages;
		struct page *page;
		int want;
		loff_t offset, len;
		unsigned wrote;

		next = 0;
		locked_pages = 0;

get_more_pages:
		first = -1;
		want = min(end - index,
			   min((pgoff_t)PAGEVEC_SIZE,
			       max_pages - (pgoff_t)locked_pages) - 1) + 1;
		pvec_pages = pagevec_lookup_tag(&pvec, mapping, &index,
						PAGECACHE_TAG_DIRTY,
						want);
		dout(20, "pagevec_lookup_tag got %d\n", pvec_pages);
		if (!pvec_pages && !locked_pages)
			break;
		for (i = 0; i < pvec_pages && locked_pages < max_pages; i++) {
			page = pvec.pages[i];
			dout(20, "? %p idx %lu\n", page, page->index);
			if (locked_pages == 0)
				lock_page(page);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
			else if (!trylock_page(page))
#else
			else if (TestSetPageLocked(page))
#endif
			{
				dout(20, "couldn't lock page %p\n", page);
				break;
			}

			/* only dirty pages, or wrbuffer accounting breaks! */
			if (unlikely(!PageDirty(page)) ||
			    unlikely(page->mapping != mapping)) {
				dout(20, "!dirty or !mapping %p\n", page);
				unlock_page(page);
				break;
			}
			if (!wbc->range_cyclic && page->index > end) {
				dout(20, "end of range %p\n", page);
				done = 1;
				unlock_page(page);
				break;
			}
			if (next && (page->index != next)) {
				/* Not next consecutive page */
				dout(20, "not consecutive %p\n", page);
				unlock_page(page);
				break;
			}
			if (wbc->sync_mode != WB_SYNC_NONE) {
				dout(20, "waiting on writeback %p\n", page);
				wait_on_page_writeback(page);
			}

			if (page_offset(page) >= i_size_read(inode)) {
				dout(20, "past eof %p\n", page);
				done = 1;
				unlock_page(page);
				break;
			}

			if (PageWriteback(page)) {
				dout(20, "%p under writeback\n", page);
				unlock_page(page);
				break;
			}

			/* only if matching snap context */
			if (snapc != (void *)page->private) {
				dout(20, "page snapc %p != oldest %p\n",
				     (void *)page->private, snapc);
				unlock_page(page);
				if (!locked_pages)
					continue; /* keep looking for snap */
				break;
			}

			if (!clear_page_dirty_for_io(page)) {
				dout(20, "%p !clear_page_dirty_for_io\n", page);
				unlock_page(page);
				break;
			}

			/* ok */
			if (first < 0)
				first = i;
			set_page_writeback(page);

			dout(20, "%p locked page %p idx %lu\n",
			     inode, page, page->index);

			pages[locked_pages] = page;
			locked_pages++;
			next = page->index + 1;
		}

		/* did we get anything? */
		if (!locked_pages)
			goto release_pages;

		if (i) {
			int j;
			BUG_ON(!locked_pages || first < 0);

			if (pvec_pages && i == pvec_pages &&
			    locked_pages < max_pages) {
				dout(50, "reached end pvec, trying for more\n");
				pagevec_reinit(&pvec);
				goto get_more_pages;
			}

			/* shift unused pages over in the pvec...  we
			 * will need to release them below. */
			for (j = i; j < pvec_pages; j++) {
				dout(50, " pvec leftover page %p\n",
				     pvec.pages[j]);
				pvec.pages[j-i+first] = pvec.pages[j];
			}
			pvec.nr -= i-first;
		}

		/* write it */
		offset = pages[0]->index << PAGE_CACHE_SHIFT;
		len = min(i_size_read(inode) - offset,
				 (loff_t)locked_pages << PAGE_CACHE_SHIFT);
		dout(10, "writepages got %d pages at %llu~%llu\n",
		     locked_pages, offset, len);
		rc = ceph_osdc_writepages(&client->osdc,
					  ceph_vino(inode),
					  &ci->i_layout,
					  snapc,
					  offset, len,
					  pages,
					  locked_pages);
		if (rc >= 0)
			wrote = (rc + (offset & ~PAGE_CACHE_MASK)
				 + ~PAGE_CACHE_MASK)
				>> PAGE_CACHE_SHIFT;
		else
			wrote = 0;
		dout(20, "writepages rc %d wrote %d\n", rc, wrote);

		/* clean or redirty pages */
		for (i = 0; i < locked_pages; i++) {
			page = pages[i];
			WARN_ON(!PageUptodate(page));
			if (i < wrote) {
				dout(20, "%p cleaning %p\n", inode, page);
				page->private = 0;
				ClearPagePrivate(page);
				ceph_put_snap_context(snapc);
			} else {
				dout(20, "%p redirtying %p\n", inode, page);
				ceph_redirty_page(mapping, page);
				wbc->pages_skipped++;
			}
			dout(50, "unlocking %d %p\n", i, page);
			end_page_writeback(page);
			unlock_page(page);
		}
		dout(20, "%p wrote+cleaned %d pages\n", inode, wrote);
		ceph_put_wrbuffer_cap_refs(ci, wrote, snapc);

		/* continue? */
		index = next;
		wbc->nr_to_write -= locked_pages;
		if (wbc->nr_to_write <= 0)
			done = 1;

	release_pages:
		/* hmm, pagevec_release also does lru_add_drain()...? */
		dout(50, "release_pages on %d\n", locked_pages);
		ceph_release_pages(pages, locked_pages);

		dout(50, "pagevec_release on %d pages (%p)\n", (int)pvec.nr,
		     pvec.nr ? pvec.pages[0] : 0);
		pagevec_release(&pvec);

		if (locked_pages && !done)
			goto retry;
	}

	if (should_loop && !done) {
		/* more to do; loop back to beginning of file */
		dout(40, "writepages looping back to beginning of file\n");
		should_loop = 0;
		index = 0;
		goto retry;
	}

	if (wbc->range_cyclic || (range_whole && wbc->nr_to_write > 0))
		mapping->writeback_index = index;

out:
	kfree(pages);
	if (rc > 0)
		rc = 0;  /* vfs expects us to return 0 */
	ceph_put_snap_context(snapc);
	dout(10, "writepages done, rc = %d\n", rc);
	return rc;
}

/*
 * newer write interface
 */
static int ceph_write_begin(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned flags,
			    struct page **pagep, void **fsdata)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_osd_client *osdc = &ceph_inode_to_client(inode)->osdc;
	struct ceph_mds_client *mdsc = &ceph_inode_to_client(inode)->mdsc;
	struct page *page;
	pgoff_t index = pos >> PAGE_CACHE_SHIFT;
	loff_t page_off = pos & PAGE_MASK;
	int pos_in_page = pos & ~PAGE_MASK;
	int end_in_page = pos_in_page + len;
	loff_t i_size;
	int r;

	/* get a page*/
	page = __grab_cache_page(mapping, index);
	if (!page)
		return -ENOMEM;
	*pagep = page;

	dout(10, "write_begin file %p inode %p page %p %d~%d\n", file,
	     inode, page, (int)pos, (int)len);

	/* writepages currently holds page lock, but if we change that later, */
	wait_on_page_writeback(page);

	/* check snap context */
	BUG_ON(!ci->i_snap_realm);
	BUG_ON(!ci->i_snap_realm->cached_context);
	down_read(&mdsc->snap_rwsem);
	if (page->private &&
	    (void *)page->private != ci->i_snap_realm->cached_context) {
		/* force early writeback of snapped page */
		r = writepage_nounlock(page, 0);
		if (r < 0)
			goto fail;
	}

	if (PageUptodate(page))
		return 0;

	/* full page? */
	if (pos_in_page == 0 && len == PAGE_SIZE)
		return 0;

	/* past end of file? */
	i_size = inode->i_size;   /* caller holds i_mutex */
	if (page_off >= i_size ||
	    (pos_in_page == 0 && (pos+len) >= i_size)) {
		simple_prepare_write(file, page, pos_in_page, end_in_page);
		return 0;
	}

	/* we need to read it. */
	/* or, do sub-page granularity dirty accounting? */
	/* try to read the full page */
	r = ceph_osdc_readpage(osdc, ceph_vino(inode), &ci->i_layout,
			       page_off, PAGE_SIZE, page);
	if (r < 0)
		goto fail;
	if (r < pos_in_page) {
		void *kaddr = kmap_atomic(page, KM_USER1);
		dout(20, "write_begin zeroing pre %d~%d\n", r, pos_in_page-r);
		memset(kaddr+r, 0, pos_in_page-r);
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER1);
	}
	end_in_page = pos_in_page + len;
	if (end_in_page < PAGE_SIZE && r < PAGE_SIZE) {
		void *kaddr = kmap_atomic(page, KM_USER1);
		dout(20, "write_begin zeroing post %d~%d\n", end_in_page,
		     (int)PAGE_SIZE - end_in_page);
		memset(kaddr+end_in_page, 0, PAGE_SIZE-end_in_page);
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER1);
	}
	return 0;

fail:
	unlock_page(page);
	up_read(&mdsc->snap_rwsem);
	return r;
}

/*
 * we don't do anything in here that simple_write_end doesn't do
 * except adjust dirty page accounting.
 */
static int ceph_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned len, unsigned copied,
			  struct page *page, void *fsdata)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct ceph_mds_client *mdsc = &ceph_inode_to_client(inode)->mdsc;
	unsigned from = pos & (PAGE_CACHE_SIZE - 1);

	dout(10, "write_end file %p inode %p page %p %d~%d (%d)\n", file,
	     inode, page, (int)pos, (int)copied, (int)len);

	/* zero the stale part of the page if we did a short copy */
	if (copied < len) {
		void *kaddr = kmap_atomic(page, KM_USER0);
		memset(kaddr + from + copied, 0, len - copied);
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER0);
	}

	/* did file size increase? */
	/* (no need for i_size_read(); we caller holds i_mutex */
	if (pos+copied > inode->i_size)
		ceph_inode_set_size(inode, pos+copied);

	if (!PageUptodate(page))
		SetPageUptodate(page);

	set_page_dirty(page);

	unlock_page(page);
	up_read(&mdsc->snap_rwsem);
	page_cache_release(page);

	return copied;
}



const struct address_space_operations ceph_aops = {
	.readpage = ceph_readpage,
	.readpages = ceph_readpages,
	.writepage = ceph_writepage,
	.writepages = ceph_writepages,
	.write_begin = ceph_write_begin,
	.write_end = ceph_write_end,
	.set_page_dirty = ceph_set_page_dirty_vfs,
	.invalidatepage = ceph_invalidatepage,
	.releasepage = ceph_releasepage,
};


/*
 * vm ops 
 */

static int ceph_page_mkwrite(struct vm_area_struct *vma, struct page *page)
{
	struct inode *inode = vma->vm_file->f_dentry->d_inode;
	loff_t off = page->index << PAGE_CACHE_SHIFT;
	loff_t size, len;
	struct page *locked_page = NULL;
	void *fsdata = NULL;
	int ret;

	size = i_size_read(inode);
	if (off + PAGE_CACHE_SIZE <= size)
		len = PAGE_CACHE_SIZE;
	else
		len = size & ~PAGE_CACHE_MASK;

	dout(10, "page_mkwrite %p %llu~%llu (page %p offset %lu)\n", inode,
	     off, len, page, page->index);
	ret = ceph_write_begin(vma->vm_file, inode->i_mapping, off, len, 0,
			       &locked_page, &fsdata);
	if (!ret)
		ceph_write_end(vma->vm_file, inode->i_mapping, off, len, len,
			       locked_page, fsdata);
	dout(10, "page_mkwrite %p %llu~%llu = %d\n", inode, off, len, ret);
	return ret;
}

static struct vm_operations_struct ceph_vmops = {
	.fault		= filemap_fault,
	.page_mkwrite	= ceph_page_mkwrite,
};

int ceph_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &ceph_vmops;
	vma->vm_flags |= VM_CAN_NONLINEAR;
	return 0;
}
