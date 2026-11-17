/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Licensed under the GPL
 *
 * Ported the filesystem routines to 2.5.
 * 2003-02-10 Petr Baudis <pasky@ucw.cz>
 */

#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/writeback.h>
#include <linux/mount.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/falloc.h>
#include "hostfs.h"
#ifndef CONFIG_LKL
#include <init.h>
#include <kern.h>
#endif

struct hostfs_fs_info {
	char *host_root_path;
};

struct hostfs_inode_info {
	int fd;
	fmode_t mode;
	struct inode vfs_inode;
	struct mutex open_mutex;
	dev_t dev;
};

static inline struct hostfs_inode_info *HOSTFS_I(struct inode *inode)
{
	return list_entry(inode, struct hostfs_inode_info, vfs_inode);
}

#define FILE_HOSTFS_I(file) HOSTFS_I(file_inode(file))

static struct kmem_cache *hostfs_inode_cache;

/* Changed in hostfs_args before the kernel starts running */
static char *root_ino = "";
static int append = 0;

static const struct inode_operations hostfs_iops;
static const struct inode_operations hostfs_dir_iops;
static const struct inode_operations hostfs_link_iops;

#if !defined(MODULE) && !defined(CONFIG_LKL)
static int __init hostfs_args(char *options, int *add)
{
	char *ptr;

	ptr = strchr(options, ',');
	if (ptr != NULL)
		*ptr++ = '\0';
	if (*options != '\0')
		root_ino = options;

	options = ptr;
	while (options) {
		ptr = strchr(options, ',');
		if (ptr != NULL)
			*ptr++ = '\0';
		if (*options != '\0') {
			if (!strcmp(options, "append"))
				append = 1;
			else printf("hostfs_args - unsupported option - %s\n",
				    options);
		}
		options = ptr;
	}
	return 0;
}

__uml_setup("hostfs=", hostfs_args,
"hostfs=<root dir>,<flags>,...\n"
"    This is used to set hostfs parameters.  The root directory argument\n"
"    is used to confine all hostfs mounts to within the specified directory\n"
"    tree on the host.  If this isn't specified, then a user inside UML can\n"
"    mount anything on the host that's accessible to the user that's running\n"
"    it.\n"
"    The only flag currently supported is 'append', which specifies that all\n"
"    files opened by hostfs will be opened in append mode.\n\n"
);
#endif

static char *__dentry_name(struct dentry *dentry, char *name)
{
	char *p = dentry_path_raw(dentry, name, PATH_MAX);
	char *root;
	size_t len;
	struct hostfs_fs_info *fsi;

	fsi = dentry->d_sb->s_fs_info;
	root = fsi->host_root_path;
	len = strlen(root);
	if (IS_ERR(p)) {
		__putname(name);
		return NULL;
	}

	/*
	 * This function relies on the fact that dentry_path_raw() will place
	 * the path name at the end of the provided buffer.
	 */
	BUG_ON(p + strlen(p) + 1 != name + PATH_MAX);

	strscpy(name, root, PATH_MAX);
	if (len > p - name) {
		__putname(name);
		return NULL;
	}

	if (p > name + len)
		strcpy(name + len, p);

	return name;
}

static char *dentry_name(struct dentry *dentry)
{
	char *name = __getname();
	if (!name)
		return NULL;

	return __dentry_name(dentry, name);
}

static char *inode_name(struct inode *ino)
{
	struct dentry *dentry;
	char *name;

	dentry = d_find_alias(ino);
	if (!dentry)
		return NULL;

	name = dentry_name(dentry);

	dput(dentry);

	return name;
}

static char *follow_link(char *link)
{
	char *name, *resolved, *end;
	int n;

	name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!name) {
		n = -ENOMEM;
		goto out_free;
	}

	n = hostfs_do_readlink(link, name, PATH_MAX);
	if (n < 0)
		goto out_free;
	else if (n == PATH_MAX) {
		n = -E2BIG;
		goto out_free;
	}

	if (*name == '/')
		return name;

	end = strrchr(link, '/');
	if (end == NULL)
		return name;

	*(end + 1) = '\0';

	resolved = kasprintf(GFP_KERNEL, "%s%s", link, name);
	if (resolved == NULL) {
		n = -ENOMEM;
		goto out_free;
	}

	kfree(name);
	return resolved;

 out_free:
	kfree(name);
	return ERR_PTR(n);
}

static int hostfs_statfs(struct dentry *dentry, struct kstatfs *sf)
{
	/*
	 * do_statfs uses struct statfs64 internally, but the linux kernel
	 * struct statfs still has 32-bit versions for most of these fields,
	 * so we convert them here
	 */
	int err;
	long long f_blocks;
	long long f_bfree;
	long long f_bavail;
	long long f_files;
	long long f_ffree;
	struct hostfs_fs_info *fsi;

	fsi = dentry->d_sb->s_fs_info;
	err = do_statfs(fsi->host_root_path,
			&sf->f_bsize, &f_blocks, &f_bfree, &f_bavail, &f_files,
			&f_ffree, &sf->f_fsid, sizeof(sf->f_fsid),
			&sf->f_namelen);
	if (err)
		return err;
	sf->f_blocks = f_blocks;
	sf->f_bfree = f_bfree;
	sf->f_bavail = f_bavail;
	sf->f_files = f_files;
	sf->f_ffree = f_ffree;
	sf->f_type = HOSTFS_SUPER_MAGIC;
	return 0;
}

static struct inode *hostfs_alloc_inode(struct super_block *sb)
{
	struct hostfs_inode_info *hi;

	hi = alloc_inode_sb(sb, hostfs_inode_cache, GFP_KERNEL_ACCOUNT);
	if (hi == NULL)
		return NULL;
	hi->fd = -1;
	hi->mode = 0;
	hi->dev = 0;
	inode_init_once(&hi->vfs_inode);
	mutex_init(&hi->open_mutex);
	return &hi->vfs_inode;
}

static void hostfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (HOSTFS_I(inode)->fd != -1) {
		close_file(&HOSTFS_I(inode)->fd);
		HOSTFS_I(inode)->fd = -1;
		HOSTFS_I(inode)->dev = 0;
	}
}

static void hostfs_free_inode(struct inode *inode)
{
	kmem_cache_free(hostfs_inode_cache, HOSTFS_I(inode));
}

static int hostfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct hostfs_fs_info *fsi;
	const char *root_path;

	fsi = root->d_sb->s_fs_info;
	root_path = fsi->host_root_path;
	size_t offset = strlen(root_ino) + 1;

	if (strlen(root_path) > offset)
		seq_show_option(seq, root_path + offset, NULL);

	if (append)
		seq_puts(seq, ",append");

	return 0;
}

static const struct super_operations hostfs_sbops = {
	.alloc_inode	= hostfs_alloc_inode,
	.free_inode	= hostfs_free_inode,
	.drop_inode	= generic_delete_inode,
	.evict_inode	= hostfs_evict_inode,
	.statfs		= hostfs_statfs,
	.show_options	= hostfs_show_options,
};

static int hostfs_readdir(struct file *file, struct dir_context *ctx)
{
	void *dir;
	char *name;
	unsigned long long next, ino;
	int error, len;
	unsigned int type;

	name = dentry_name(file->f_path.dentry);
	if (name == NULL)
		return -ENOMEM;
	dir = open_dir(name, &error);
	__putname(name);
	if (dir == NULL)
		return -error;
	next = ctx->pos;
	seek_dir(dir, next);
	while ((name = read_dir(dir, &next, &ino, &len, &type)) != NULL) {
		if (!dir_emit(ctx, name, len, ino, type))
			break;
		ctx->pos = next;
	}
	close_dir(dir);
	return 0;
}

static int hostfs_open(struct inode *ino, struct file *file)
{
	char *name;
	fmode_t mode;
	int err;
	int r, w, fd;

	mode = file->f_mode & (FMODE_READ | FMODE_WRITE);
	if ((mode & HOSTFS_I(ino)->mode) == mode)
		return 0;

	mode |= HOSTFS_I(ino)->mode;

retry:
	r = w = 0;

	if (mode & FMODE_READ)
		r = 1;
	if (mode & FMODE_WRITE)
		r = w = 1;

	name = dentry_name(file_dentry(file));
	if (name == NULL)
		return -ENOMEM;

	fd = open_file(name, r, w, append);
	__putname(name);
	if (fd < 0)
		return fd;

	mutex_lock(&HOSTFS_I(ino)->open_mutex);
	/* somebody else had handled it first? */
	if ((mode & HOSTFS_I(ino)->mode) == mode) {
		mutex_unlock(&HOSTFS_I(ino)->open_mutex);
		close_file(&fd);
		return 0;
	}
	if ((mode | HOSTFS_I(ino)->mode) != mode) {
		mode |= HOSTFS_I(ino)->mode;
		mutex_unlock(&HOSTFS_I(ino)->open_mutex);
		close_file(&fd);
		goto retry;
	}
	if (HOSTFS_I(ino)->fd == -1) {
		HOSTFS_I(ino)->fd = fd;
	} else {
		err = replace_file(fd, HOSTFS_I(ino)->fd);
		close_file(&fd);
		if (err < 0) {
			mutex_unlock(&HOSTFS_I(ino)->open_mutex);
			return err;
		}
	}
	HOSTFS_I(ino)->mode = mode;
	mutex_unlock(&HOSTFS_I(ino)->open_mutex);

	return 0;
}

static int hostfs_file_release(struct inode *inode, struct file *file)
{
	filemap_write_and_wait(inode->i_mapping);

	return 0;
}

static int hostfs_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	struct inode *inode = file->f_mapping->host;
	int ret;

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	inode_lock(inode);
	ret = fsync_file(HOSTFS_I(inode)->fd, datasync);
	inode_unlock(inode);

	return ret;
}

/*
 * fallocate and copy_file_range are forwarded to the host file. Both need a
 * host fd; the inode keeps one for as long as the file is open, and otherwise
 * a temporary descriptor is opened for the duration of the call.
 */
static int hostfs_host_fd(struct inode *inode, struct dentry *dentry, int w)
{
	char *name;
	int fd;

	if (HOSTFS_I(inode)->fd != -1)
		return HOSTFS_I(inode)->fd;
	name = dentry_name(dentry);
	if (name == NULL)
		return -ENOMEM;
	fd = open_file(name, w ? 0 : 1, w ? 1 : 0, 0);
	__putname(name);
	return fd;
}

static int hostfs_release_temp_fd(struct inode *inode, int fd)
{
	if (fd >= 0 && fd != HOSTFS_I(inode)->fd) {
		close_file(&fd);
		return 1;
	}
	return 0;
}

static long hostfs_file_fallocate(struct file *file, int mode, loff_t offset,
				  loff_t len)
{
	struct inode *inode = file_inode(file);
	unsigned int cmd = mode & FALLOC_FL_PUNCH_HOLE;
	int fd, err;

	if (mode & ~FALLOC_FL_MODE_MASK)
		return -EOPNOTSUPP;
	if ((cmd == FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	fd = hostfs_host_fd(inode, file_dentry(file), 1);
	if (fd < 0)
		return fd;
	err = hostfs_fallocate(fd, mode, offset, len);
	if (hostfs_release_temp_fd(inode, fd))
		return err;
	if (err == 0)
		file_update_time(file);
	return err;
}

static ssize_t hostfs_copy_file_range(struct file *file_in, loff_t pos_in,
				      struct file *file_out, loff_t pos_out,
				      size_t len, unsigned int flags)
{
	struct inode *inode_in = file_inode(file_in);
	struct inode *inode_out = file_inode(file_out);
	long long in_off = pos_in, out_off = pos_out;
	int fdin, fdout, rfdin, rfdout;
	long long n;

	if (flags)
		return -EINVAL;

	fdin = hostfs_host_fd(inode_in, file_dentry(file_in), 0);
	if (fdin < 0)
		return fdin;
	fdout = hostfs_host_fd(inode_out, file_dentry(file_out), 1);
	if (fdout < 0) {
		hostfs_release_temp_fd(inode_in, fdin);
		return fdout;
	}

	n = hostfs_copy_range(fdin, &in_off, fdout, &out_off, len, flags);
	rfdin = hostfs_release_temp_fd(inode_in, fdin);
	rfdout = hostfs_release_temp_fd(inode_out, fdout);
	(void) rfdin;

	if (n > 0) {
		i_size_write(inode_out, max_t(loff_t, i_size_read(inode_out),
					      out_off));
		if (!rfdout)
			file_update_time(file_out);
	}
	return n;
}

static const struct file_operations hostfs_file_fops = {
	.llseek		= generic_file_llseek,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= generic_file_mmap,
	.open		= hostfs_open,
	.fsync		= hostfs_fsync,
	.fallocate	= hostfs_file_fallocate,
	.copy_file_range = hostfs_copy_file_range,
	.release	= hostfs_file_release,
};

static const struct file_operations hostfs_dir_fops = {
	.llseek		= generic_file_llseek,
	.iterate_shared	= hostfs_readdir,
	.read		= generic_read_dir,
	.open		= hostfs_open,
	.fsync		= hostfs_fsync,
};

static int hostfs_writepage(struct page *page, struct writeback_control *wbc)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	char *buffer;
	loff_t base = page_offset(page);
	int count = PAGE_SIZE;
	int end_index = inode->i_size >> PAGE_SHIFT;
	int err;

	if (page->index >= end_index)
		count = inode->i_size & (PAGE_SIZE-1);

	buffer = kmap_local_page(page);

	err = write_file(HOSTFS_I(inode)->fd, &base, buffer, count);
	if (err != count) {
		if (err >= 0)
			err = -EIO;
		mapping_set_error(mapping, err);
		goto out;
	}

	if (base > inode->i_size)
		inode->i_size = base;

	err = 0;

 out:
	kunmap_local(buffer);
	unlock_page(page);

	return err;
}

static int hostfs_read_folio(struct file *file, struct folio *folio)
{
	char *buffer;
	loff_t start = folio_pos(folio);
	int bytes_read, ret = 0;

	buffer = kmap_local_folio(folio, 0);
	bytes_read = read_file(FILE_HOSTFS_I(file)->fd, &start, buffer,
			PAGE_SIZE);
	if (bytes_read < 0)
		ret = bytes_read;
	else
		buffer = folio_zero_tail(folio, bytes_read, buffer + bytes_read);
	kunmap_local(buffer);

	folio_end_read(folio, ret == 0);
	return ret;
}

static int hostfs_write_begin(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned len,
			      struct folio **foliop, void **fsdata)
{
	pgoff_t index = pos >> PAGE_SHIFT;

	*foliop = __filemap_get_folio(mapping, index, FGP_WRITEBEGIN,
			mapping_gfp_mask(mapping));
	if (!*foliop)
		return -ENOMEM;
	return 0;
}

static int hostfs_write_end(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned copied,
			    struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	void *buffer;
	size_t from = offset_in_folio(folio, pos);
	int err;

	buffer = kmap_local_folio(folio, from);
	err = write_file(FILE_HOSTFS_I(file)->fd, &pos, buffer, copied);
	kunmap_local(buffer);

	if (!folio_test_uptodate(folio) && err == folio_size(folio))
		folio_mark_uptodate(folio);

	/*
	 * If err > 0, write_file has added err to pos, so we are comparing
	 * i_size against the last byte written.
	 */
	if (err > 0 && (pos > inode->i_size))
		inode->i_size = pos;
	folio_unlock(folio);
	folio_put(folio);

	return err;
}

static const struct address_space_operations hostfs_aops = {
	.writepage 	= hostfs_writepage,
	.read_folio	= hostfs_read_folio,
	.dirty_folio	= filemap_dirty_folio,
	.write_begin	= hostfs_write_begin,
	.write_end	= hostfs_write_end,
};

static int hostfs_inode_update(struct inode *ino, const struct hostfs_stat *st)
{
	set_nlink(ino, st->nlink);
	i_uid_write(ino, hostfs_guest_uid(st->uid));
	i_gid_write(ino, hostfs_guest_gid(st->gid));
	inode_set_atime_to_ts(ino, (struct timespec64){
			st->atime.tv_sec,
			st->atime.tv_nsec,
		});
	inode_set_mtime_to_ts(ino, (struct timespec64){
			st->mtime.tv_sec,
			st->mtime.tv_nsec,
		});
	inode_set_ctime(ino, st->ctime.tv_sec, st->ctime.tv_nsec);
	ino->i_size = st->size;
	ino->i_blocks = st->blocks;
	return 0;
}

static int hostfs_inode_set(struct inode *ino, void *data)
{
	struct hostfs_stat *st = data;
	dev_t dev, rdev;

	/* Reencode maj and min with the kernel encoding.*/
	rdev = MKDEV(st->rdev.maj, st->rdev.min);
	dev = MKDEV(st->dev.maj, st->dev.min);

	switch (st->mode & S_IFMT) {
	case S_IFLNK:
		ino->i_op = &hostfs_link_iops;
		break;
	case S_IFDIR:
		ino->i_op = &hostfs_dir_iops;
		ino->i_fop = &hostfs_dir_fops;
		break;
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
		init_special_inode(ino, st->mode & S_IFMT, rdev);
		ino->i_op = &hostfs_iops;
		break;
	case S_IFREG:
		ino->i_op = &hostfs_iops;
		ino->i_fop = &hostfs_file_fops;
		ino->i_mapping->a_ops = &hostfs_aops;
		break;
	default:
		return -EIO;
	}

	HOSTFS_I(ino)->dev = dev;
	ino->i_ino = st->ino;
	ino->i_mode = st->mode;
	return hostfs_inode_update(ino, st);
}

static int hostfs_inode_test(struct inode *inode, void *data)
{
	const struct hostfs_stat *st = data;
	dev_t dev = MKDEV(st->dev.maj, st->dev.min);

	return inode->i_ino == st->ino && HOSTFS_I(inode)->dev == dev;
}

/* Report a whiteout marker (an empty file carrying HOSTFS_WHITEOUT_XATTR) as
 * what overlayfs expects to see: a character device 0:0. */
static void hostfs_apply_whiteout(const char *name, struct hostfs_stat *st)
{
	if (!S_ISREG(st->mode) || st->size != 0)
		return;
	if (!hostfs_is_whiteout(name))
		return;

	st->mode = S_IFCHR;
	st->rdev.maj = 0;
	st->rdev.min = 0;
	st->nlink = 1;
}

static struct inode *hostfs_iget(struct super_block *sb, char *name)
{
	struct inode *inode;
	struct hostfs_stat st;
	int err = stat_file(name, &st, -1);

	if (err)
		return ERR_PTR(err);

	hostfs_apply_whiteout(name, &st);

	inode = iget5_locked(sb, st.ino, hostfs_inode_test, hostfs_inode_set,
			     &st);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (inode->i_state & I_NEW) {
		inode->i_opflags |= IOP_XATTR;
		unlock_new_inode(inode);
	} else {
		spin_lock(&inode->i_lock);
		hostfs_inode_update(inode, &st);
		spin_unlock(&inode->i_lock);
	}

	return inode;
}

static int hostfs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	char *name;
	int fd;

	name = dentry_name(dentry);
	if (name == NULL)
		return -ENOMEM;

	fd = file_create(name, mode & 0777);
	if (fd < 0) {
		__putname(name);
		return fd;
	}

	inode = hostfs_iget(dir->i_sb, name);
	__putname(name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	HOSTFS_I(inode)->fd = fd;
	HOSTFS_I(inode)->mode = FMODE_READ | FMODE_WRITE;
	d_instantiate(dentry, inode);
	return 0;
}

static struct dentry *hostfs_lookup(struct inode *ino, struct dentry *dentry,
				    unsigned int flags)
{
	struct inode *inode = NULL;
	char *name;

	name = dentry_name(dentry);
	if (name == NULL)
		return ERR_PTR(-ENOMEM);

	inode = hostfs_iget(ino->i_sb, name);
	__putname(name);
	if (inode == ERR_PTR(-ENOENT))
		inode = NULL;

	return d_splice_alias(inode, dentry);
}

/*
 * O_TMPFILE support: create the file anonymously on the host and remember its
 * descriptor, then materialise it on link() through /proc/self/fd. The pod runs
 * unprivileged, so linkat(AT_EMPTY_PATH) with CAP_DAC_READ_SEARCH is not an
 * option; following the /proc fd entry needs no capability.
 */
static int hostfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			  struct file *file, umode_t mode)
{
	struct inode *inode;
	char *name;
	int fd;

	name = dentry_name(file->f_path.dentry);
	if (name == NULL)
		return -ENOMEM;
	fd = hostfs_tmpfile_open(name, mode);
	__putname(name);
	if (fd < 0)
		return fd;

	inode = new_inode(dir->i_sb);
	if (inode == NULL) {
		close_file(&fd);
		return -ENOMEM;
	}

	inode->i_mode = mode;
	inode->i_op = &hostfs_iops;
	inode->i_fop = &hostfs_file_fops;
	inode->i_mapping->a_ops = &hostfs_aops;
	HOSTFS_I(inode)->fd = fd;
	HOSTFS_I(inode)->mode = FMODE_READ | FMODE_WRITE;
	inode->i_size = 0;
	d_tmpfile(file, inode);
	unlock_new_inode(inode);
	return finish_open_simple(file, 0);
}

/* Instantiate a freshly created host entry into its (negative) dentry. */
static int hostfs_iget_instantiate(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode;
	char *name = dentry_name(dentry);
	int err;

	if (name == NULL)
		return -ENOMEM;
	inode = hostfs_iget(dir->i_sb, name);
	__putname(name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static int hostfs_link(struct dentry *to, struct inode *ino,
		       struct dentry *from)
{
	char *from_name, *to_name;
	int err;

	if ((from_name = dentry_name(from)) == NULL)
		return -ENOMEM;

	/* A dentry created by hostfs_tmpfile has no host path of its own; link
	 * the anonymous host file through its descriptor instead. */
	if (d_is_positive(to) && d_unhashed(to) &&
	    HOSTFS_I(d_inode(to))->fd != -1) {
		int fd = HOSTFS_I(d_inode(to))->fd;

		err = hostfs_link_fd(fd, from_name);
		__putname(from_name);
		if (err)
			return err;
		return hostfs_iget_instantiate(d_inode(to), from);
	}

	to_name = dentry_name(to);
	if (to_name == NULL) {
		__putname(from_name);
		return -ENOMEM;
	}
	err = link_file(to_name, from_name);
	if (err) {
		__putname(from_name);
		__putname(to_name);
		return err;
	}
	err = hostfs_iget_instantiate(ino, from);
	__putname(from_name);
	__putname(to_name);
	return err;
}

static int hostfs_unlink(struct inode *ino, struct dentry *dentry)
{
	char *file;
	int err;

	if (append)
		return -EPERM;

	if ((file = dentry_name(dentry)) == NULL)
		return -ENOMEM;

	err = unlink_file(file);
	__putname(file);
	return err;
}

static int hostfs_symlink(struct mnt_idmap *idmap, struct inode *ino,
			  struct dentry *dentry, const char *to)
{
	char *file;
	int err;

	struct inode *inode;

	if ((file = dentry_name(dentry)) == NULL)
		return -ENOMEM;
	err = make_symlink(file, to);
	if (err) {
		__putname(file);
		return err;
	}

	inode = hostfs_iget(ino->i_sb, file);
	__putname(file);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int hostfs_mkdir(struct mnt_idmap *idmap, struct inode *ino,
			struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	char *file;
	int err;

	if ((file = dentry_name(dentry)) == NULL)
		return -ENOMEM;
	/* overlayfs passes S_IFDIR in ia_mode when it creates its work
	 * directory; the host mkdir(2) rejects type bits with EINVAL. */
	err = do_mkdir(file, (mode & 07777) | S_IRWXU);
	if (err) {
		__putname(file);
		return err;
	}

	inode = hostfs_iget(ino->i_sb, file);
	__putname(file);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int hostfs_rmdir(struct inode *ino, struct dentry *dentry)
{
	char *file;
	int err;

	if ((file = dentry_name(dentry)) == NULL)
		return -ENOMEM;
	err = hostfs_do_rmdir(file);
	__putname(file);
	return err;
}

static int hostfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode;
	char *name;
	int err;

	name = dentry_name(dentry);
	if (name == NULL)
		return -ENOMEM;

	if ((mode & S_IFMT) == S_IFCHR && MAJOR(dev) == 0 && MINOR(dev) == 0)
		err = hostfs_whiteout_create(name);
	else
		err = do_mknod(name, mode, MAJOR(dev), MINOR(dev));
	if (err) {
		__putname(name);
		return err;
	}

	inode = hostfs_iget(dir->i_sb, name);
	__putname(name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int hostfs_rename2(struct mnt_idmap *idmap,
			  struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry,
			  unsigned int flags)
{
	char *old_name, *new_name;
	int err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;

	/* A tmpfile has no host path: materialise it at the new name instead of
	 * renaming something that does not exist on the host. overlayfs ends
	 * every copy-up this way. */
	{
		char *on = dentry_name(old_dentry);
		char *nn = dentry_name(new_dentry);
		if (on)
			__putname(on);
		if (nn)
			__putname(nn);
	}
	if (d_is_positive(old_dentry) && d_unhashed(old_dentry) &&
	    HOSTFS_I(d_inode(old_dentry))->fd != -1) {
		char *name = dentry_name(new_dentry);
		int err;

		if (name == NULL)
			return -ENOMEM;
		err = hostfs_link_fd(HOSTFS_I(d_inode(old_dentry))->fd, name);
		__putname(name);
		if (err)
			return err;
		return hostfs_iget_instantiate(new_dir, new_dentry);
	}

	old_name = dentry_name(old_dentry);
	if (old_name == NULL)
		return -ENOMEM;
	new_name = dentry_name(new_dentry);
	if (new_name == NULL) {
		__putname(old_name);
		return -ENOMEM;
	}
	if (!flags)
		err = rename_file(old_name, new_name);
	else
		err = rename2_file(old_name, new_name, flags);

	__putname(old_name);
	__putname(new_name);
	return err;
}

/*
 * Attributes are a passthrough too: re-read them from the host instead of
 * trusting the values cached at lookup time. Without this, a file created or
 * replaced on the host between two stats stays invisible in the guest, which
 * matters when the same directory is shared with the pod's own tooling.
 */
static int hostfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			  struct kstat *stat, u32 request_mask,
			  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct hostfs_stat st;
	char *name;
	int err;

	if (HOSTFS_I(inode)->fd != -1) {
		err = stat_file(NULL, &st, HOSTFS_I(inode)->fd);
	} else {
		name = dentry_name(path->dentry);
		if (name == NULL)
			return -ENOMEM;
		err = stat_file(name, &st, -1);
		if (err == 0)
			hostfs_apply_whiteout(name, &st);
		__putname(name);
	}
	if (err)
		return err;

	spin_lock(&inode->i_lock);
	hostfs_inode_update(inode, &st);
	spin_unlock(&inode->i_lock);

	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	return 0;
}

static int hostfs_permission(struct mnt_idmap *idmap,
			     struct inode *ino, int desired)
{
	char *name;
	int r = 0, w = 0, x = 0, err;

	if (desired & MAY_NOT_BLOCK)
		return -ECHILD;

	if (desired & MAY_READ) r = 1;
	if (desired & MAY_WRITE) w = 1;
	if (desired & MAY_EXEC) x = 1;
	/*
	 * The permission check must be the guest's, not the host access(2)
	 * result: the hosting process owns the tree and the guest sees the
	 * host uid/gid through vcred, so a host access() call rejects what the
	 * guest is entitled to. overlayfs itself relies on this when it creates
	 * its work directory with mode 0 and keeps operating inside it.
	 * The real policy is still enforced by the host kernel on each
	 * forwarded operation, which fails with the host's own EACCES.
	 */
	(void) r;
	(void) w;
	(void) x;
	err = generic_permission(&nop_mnt_idmap, ino, desired);
	return err;
}

static int hostfs_setattr(struct mnt_idmap *idmap,
			  struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct hostfs_iattr attrs;
	char *name;
	int err;

	int fd = HOSTFS_I(inode)->fd;

	err = setattr_prepare(&nop_mnt_idmap, dentry, attr);
	if (err)
		return err;

	if (append)
		attr->ia_valid &= ~ATTR_SIZE;

	attrs.ia_valid = 0;
	if (attr->ia_valid & ATTR_MODE) {
		attrs.ia_valid |= HOSTFS_ATTR_MODE;
		attrs.ia_mode = attr->ia_mode;
		/* A directory without owner rwx cannot be entered by the host
		 * process that performs every hostfs operation, so overlayfs
		 * could not use it as its work directory (it creates it with
		 * mode 0 on purpose and relies on CAP_DAC_OVERRIDE, which an
		 * unprivileged container does not have). Keep owner rwx. */
		if (S_ISDIR(inode->i_mode))
			attrs.ia_mode |= S_IRWXU;
	}
	if (attr->ia_valid & ATTR_UID) {
		attrs.ia_valid |= HOSTFS_ATTR_UID;
		attrs.ia_uid = from_kuid(&init_user_ns, attr->ia_uid);
		if (attrs.ia_uid == 0)
			attrs.ia_uid = hostfs_host_uid();
	}
	if (attr->ia_valid & ATTR_GID) {
		attrs.ia_valid |= HOSTFS_ATTR_GID;
		attrs.ia_gid = from_kgid(&init_user_ns, attr->ia_gid);
		if (attrs.ia_gid == 0)
			attrs.ia_gid = hostfs_host_gid();
	}
	if (attr->ia_valid & ATTR_SIZE) {
		attrs.ia_valid |= HOSTFS_ATTR_SIZE;
		attrs.ia_size = attr->ia_size;
	}
	if (attr->ia_valid & ATTR_ATIME) {
		attrs.ia_valid |= HOSTFS_ATTR_ATIME;
		attrs.ia_atime = (struct hostfs_timespec)
			{ attr->ia_atime.tv_sec, attr->ia_atime.tv_nsec };
	}
	if (attr->ia_valid & ATTR_MTIME) {
		attrs.ia_valid |= HOSTFS_ATTR_MTIME;
		attrs.ia_mtime = (struct hostfs_timespec)
			{ attr->ia_mtime.tv_sec, attr->ia_mtime.tv_nsec };
	}
	if (attr->ia_valid & ATTR_CTIME) {
		attrs.ia_valid |= HOSTFS_ATTR_CTIME;
		attrs.ia_ctime = (struct hostfs_timespec)
			{ attr->ia_ctime.tv_sec, attr->ia_ctime.tv_nsec };
	}
	if (attr->ia_valid & ATTR_ATIME_SET) {
		attrs.ia_valid |= HOSTFS_ATTR_ATIME_SET;
	}
	if (attr->ia_valid & ATTR_MTIME_SET) {
		attrs.ia_valid |= HOSTFS_ATTR_MTIME_SET;
	}
	name = dentry_name(dentry);
	if (name == NULL)
		return -ENOMEM;
	err = set_attr(name, &attrs, fd);
	__putname(name);
	if (err)
		return err;

	if ((attr->ia_valid & ATTR_SIZE) &&
	    attr->ia_size != i_size_read(inode))
		truncate_setsize(inode, attr->ia_size);

	setattr_copy(&nop_mnt_idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

/*
 * Extended attributes: straight passthrough to the host l*xattr calls.
 *
 * overlayfs needs at least one of (trusted|user).overlay.* xattr or
 * RENAME_WHITEOUT to place whiteouts in its upper layer. The pod kboxd runs in
 * has no CAP_SYS_ADMIN on the host, so trusted.* fails with EPERM and overlayfs
 * must be mounted with the "userxattr" option; user.* is then set by the
 * unprivileged uid that owns the corpus. Both namespaces are forwarded here
 * unchanged, and the host kernel decides.
 */
static int hostfs_xattr_get(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *suffix, void *value, size_t size)
{
	const char *name = xattr_full_name(handler, suffix);
	char *path;
	int err;

	if (HOSTFS_I(inode)->fd != -1)
		return hostfs_fgetxattr(HOSTFS_I(inode)->fd, name, value, size);

	path = dentry_name(dentry);
	if (path == NULL)
		return -ENOMEM;
	err = hostfs_getxattr(path, name, value, size);
	__putname(path);
	return err;
}

static int hostfs_xattr_set(const struct xattr_handler *handler,
			    struct mnt_idmap *idmap, struct dentry *dentry,
			    struct inode *inode, const char *suffix,
			    const void *value, size_t size, int flags)
{
	const char *name = xattr_full_name(handler, suffix);
	char *path;
	int err;

	path = dentry_name(dentry);
	if (path == NULL)
		return -ENOMEM;
	if (HOSTFS_I(inode)->fd != -1) {
		if (value == NULL)
			err = hostfs_fremovexattr(HOSTFS_I(inode)->fd, name);
		else
			err = hostfs_fsetxattr(HOSTFS_I(inode)->fd, name, value,
					       size, flags);
		__putname(path);
		return err;
	}

	if (value == NULL)
		err = hostfs_removexattr(path, name);
	else
		err = hostfs_setxattr(path, name, value, size, flags);
	__putname(path);
	return err;
}

static ssize_t hostfs_do_listxattr(struct dentry *dentry, char *list,
				   size_t size)
{
	struct inode *inode = d_inode(dentry);
	char *path;
	int err;

	if (HOSTFS_I(inode)->fd != -1)
		return hostfs_flistxattr(HOSTFS_I(inode)->fd, list, size);

	path = dentry_name(dentry);
	if (path == NULL)
		return -ENOMEM;
	err = hostfs_listxattr(path, list, size);
	__putname(path);
	return err;
}

static const struct xattr_handler hostfs_xattr_user = {
	.prefix = XATTR_USER_PREFIX,
	.get	= hostfs_xattr_get,
	.set	= hostfs_xattr_set,
};

static const struct xattr_handler hostfs_xattr_trusted = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.get	= hostfs_xattr_get,
	.set	= hostfs_xattr_set,
};

static const struct xattr_handler hostfs_xattr_security = {
	.prefix = XATTR_SECURITY_PREFIX,
	.get	= hostfs_xattr_get,
	.set	= hostfs_xattr_set,
};

static const struct xattr_handler hostfs_xattr_system = {
	.prefix = XATTR_SYSTEM_PREFIX,
	.get	= hostfs_xattr_get,
	.set	= hostfs_xattr_set,
};

static const struct xattr_handler *const hostfs_xattr_handlers[] = {
	&hostfs_xattr_user,
	&hostfs_xattr_trusted,
	&hostfs_xattr_security,
	&hostfs_xattr_system,
	NULL,
};

static const struct inode_operations hostfs_iops = {
	.getattr	= hostfs_getattr,
	.permission	= hostfs_permission,
	.setattr	= hostfs_setattr,
	.listxattr	= hostfs_do_listxattr,
};

static const struct inode_operations hostfs_dir_iops = {
	.create		= hostfs_create,
	.lookup		= hostfs_lookup,
	.link		= hostfs_link,
	.unlink		= hostfs_unlink,
	.symlink	= hostfs_symlink,
	.mkdir		= hostfs_mkdir,
	.rmdir		= hostfs_rmdir,
	.mknod		= hostfs_mknod,
	.rename		= hostfs_rename2,
	.permission	= hostfs_permission,
	.setattr	= hostfs_setattr,
	.listxattr	= hostfs_do_listxattr,
	.tmpfile	= hostfs_tmpfile,
};

static const char *hostfs_get_link(struct dentry *dentry,
				   struct inode *inode,
				   struct delayed_call *done)
{
	char *link;
	if (!dentry)
		return ERR_PTR(-ECHILD);
	link = kmalloc(PATH_MAX, GFP_KERNEL);
	if (link) {
		char *path = dentry_name(dentry);
		int err = -ENOMEM;
		if (path) {
			err = hostfs_do_readlink(path, link, PATH_MAX);
			if (err == PATH_MAX)
				err = -E2BIG;
			__putname(path);
		}
		if (err < 0) {
			kfree(link);
			return ERR_PTR(err);
		}
	} else {
		return ERR_PTR(-ENOMEM);
	}

	set_delayed_call(done, kfree_link, link);
	return link;
}

static const struct inode_operations hostfs_link_iops = {
	.get_link	= hostfs_get_link,
	.listxattr	= hostfs_do_listxattr,
};

static int hostfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct hostfs_fs_info *fsi = sb->s_fs_info;
	struct inode *root_inode;
	int err;

	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->s_magic = HOSTFS_SUPER_MAGIC;
	sb->s_op = &hostfs_sbops;
	sb->s_xattr = hostfs_xattr_handlers;
	sb->s_d_op = &simple_dentry_operations;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	err = super_setup_bdi(sb);
	if (err)
		return err;

	root_inode = hostfs_iget(sb, fsi->host_root_path);
	if (IS_ERR(root_inode))
		return PTR_ERR(root_inode);

	if (S_ISLNK(root_inode->i_mode)) {
		char *name;

		iput(root_inode);
		name = follow_link(fsi->host_root_path);
		if (IS_ERR(name))
			return PTR_ERR(name);

		root_inode = hostfs_iget(sb, name);
		kfree(name);
		if (IS_ERR(root_inode))
			return PTR_ERR(root_inode);
	}

	sb->s_root = d_make_root(root_inode);
	if (sb->s_root == NULL)
		return -ENOMEM;

	return 0;
}

enum hostfs_parma {
	Opt_hostfs,
};

static const struct fs_parameter_spec hostfs_param_specs[] = {
	fsparam_string_empty("hostfs",		Opt_hostfs),
	{}
};

static int hostfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct hostfs_fs_info *fsi = fc->s_fs_info;
	struct fs_parse_result result;
	char *host_root;
	int opt;

	opt = fs_parse(fc, hostfs_param_specs, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_hostfs:
		host_root = param->string;
		if (!*host_root)
			host_root = "";
		fsi->host_root_path =
			kasprintf(GFP_KERNEL, "%s/%s", root_ino, host_root);
		if (fsi->host_root_path == NULL)
			return -ENOMEM;
		break;
	}

	return 0;
}

static int hostfs_parse_monolithic(struct fs_context *fc, void *data)
{
	struct hostfs_fs_info *fsi = fc->s_fs_info;
	char *host_root = (char *)data;

	/* NULL is printed as '(null)' by printf(): avoid that. */
	if (host_root == NULL)
		host_root = "";

	/* Accept both the UML form ("-o /path/on/host") and the keyed form
	 * used by kbox ("-o hostfs=/path/on/host"). */
	if (strncmp(host_root, "hostfs=", 7) == 0)
		host_root += 7;

	fsi->host_root_path =
		kasprintf(GFP_KERNEL, "%s/%s", root_ino, host_root);
	if (fsi->host_root_path == NULL)
		return -ENOMEM;

	return 0;
}

static int hostfs_fc_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, hostfs_fill_super);
}

static void hostfs_fc_free(struct fs_context *fc)
{
	struct hostfs_fs_info *fsi = fc->s_fs_info;

	if (!fsi)
		return;

	kfree(fsi->host_root_path);
	kfree(fsi);
}

static const struct fs_context_operations hostfs_context_ops = {
	.parse_monolithic = hostfs_parse_monolithic,
	.parse_param	= hostfs_parse_param,
	.get_tree	= hostfs_fc_get_tree,
	.free		= hostfs_fc_free,
};

static int hostfs_init_fs_context(struct fs_context *fc)
{
	struct hostfs_fs_info *fsi;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	fc->s_fs_info = fsi;
	fc->ops = &hostfs_context_ops;
	return 0;
}

static void hostfs_kill_sb(struct super_block *s)
{
	kill_anon_super(s);
	kfree(s->s_fs_info);
}

static struct file_system_type hostfs_type = {
	.owner			= THIS_MODULE,
	.name			= "hostfs",
	.init_fs_context	= hostfs_init_fs_context,
	.kill_sb		= hostfs_kill_sb,
	.fs_flags		= 0,
};
MODULE_ALIAS_FS("hostfs");

static int __init init_hostfs(void)
{
	hostfs_inode_cache = KMEM_CACHE(hostfs_inode_info, 0);
	if (!hostfs_inode_cache)
		return -ENOMEM;
	return register_filesystem(&hostfs_type);
}

static void __exit exit_hostfs(void)
{
	unregister_filesystem(&hostfs_type);
	kmem_cache_destroy(hostfs_inode_cache);
}

module_init(init_hostfs)
module_exit(exit_hostfs)
MODULE_DESCRIPTION("User-Mode Linux Host filesystem");
MODULE_LICENSE("GPL");
