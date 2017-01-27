#ifndef _LINUX_DAX_H
#define _LINUX_DAX_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/radix-tree.h>
#include <asm/pgtable.h>

struct iomap_ops;

int dax_read_lock(void);
void dax_read_unlock(int id);
struct dax_device *dax_get_by_host(const char *host);
struct dax_device *alloc_dax(void *private, const char *host,
		const struct dax_operations *ops);
void put_dax(struct dax_device *dax_dev);
bool dax_alive(struct dax_device *dax_dev);
void kill_dax(struct dax_device *dax_dev);
void *dax_get_private(struct dax_device *dax_dev);
long dax_direct_access(struct dax_device *dax_dev, pgoff_t pgoff, long nr_pages,
		void **kaddr, pfn_t *pfn);

/*
 * We use lowest available bit in exceptional entry for locking, one bit for
 * the entry size (PMD) and two more to tell us if the entry is a huge zero
 * page (HZP) or an empty entry that is just used for locking.  In total four
 * special bits.
 *
 * If the PMD bit isn't set the entry has size PAGE_SIZE, and if the HZP and
 * EMPTY bits aren't set the entry is a normal DAX entry with a filesystem
 * block allocation.
 */
#define RADIX_DAX_SHIFT	(RADIX_TREE_EXCEPTIONAL_SHIFT + 4)
#define RADIX_DAX_ENTRY_LOCK (1 << RADIX_TREE_EXCEPTIONAL_SHIFT)

ssize_t iomap_dax_rw(struct kiocb *iocb, struct iov_iter *iter,
		struct iomap_ops *ops);
ssize_t dax_do_io(struct kiocb *, struct inode *, struct iov_iter *,
		  get_block_t, dio_iodone_t, int flags);
int dax_zero_page_range(struct inode *, loff_t from, unsigned len, get_block_t);
int dax_truncate_page(struct inode *, loff_t from, get_block_t);
int iomap_dax_fault(struct vm_area_struct *vma, struct vm_fault *vmf,
			struct iomap_ops *ops);
int dax_fault(struct vm_area_struct *, struct vm_fault *, get_block_t);
int dax_delete_mapping_entry(struct address_space *mapping, pgoff_t index);
void dax_wake_mapping_entry_waiter(struct address_space *mapping,
				   pgoff_t index, bool wake_all);

#ifdef CONFIG_FS_DAX
struct page *read_dax_sector(struct block_device *bdev, sector_t n);
void dax_unlock_mapping_entry(struct address_space *mapping, pgoff_t index);
int __dax_zero_page_range(struct block_device *bdev, sector_t sector,
		unsigned int offset, unsigned int length);
#else
static inline struct page *read_dax_sector(struct block_device *bdev,
		sector_t n)
{
	return ERR_PTR(-ENXIO);
}
/* Shouldn't ever be called when dax is disabled. */
static inline void dax_unlock_mapping_entry(struct address_space *mapping,
					    pgoff_t index)
{
	BUG();
}
static inline int __dax_zero_page_range(struct block_device *bdev,
		sector_t sector, unsigned int offset, unsigned int length)
{
	return -ENXIO;
}
#endif

#if defined(CONFIG_TRANSPARENT_HUGEPAGE)
int dax_pmd_fault(struct vm_area_struct *, unsigned long addr, pmd_t *,
				unsigned int flags, get_block_t);
#else
static inline int dax_pmd_fault(struct vm_area_struct *vma, unsigned long addr,
				pmd_t *pmd, unsigned int flags, get_block_t gb)
{
	return VM_FAULT_FALLBACK;
}
#endif
int dax_pfn_mkwrite(struct vm_area_struct *, struct vm_fault *);
#define dax_mkwrite(vma, vmf, gb)	dax_fault(vma, vmf, gb)

static inline bool dax_mapping(struct address_space *mapping)
{
	return mapping->host && IS_DAX(mapping->host);
}

struct writeback_control;
int dax_writeback_mapping_range(struct address_space *mapping,
		struct block_device *bdev, struct writeback_control *wbc);
#endif
