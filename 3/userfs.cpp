#include "userfs.h"

#include "rlist.h"

#include <stddef.h>
#include <string>
#include <vector>
#include "unit.h"
enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char memory[BLOCK_SIZE];
	/** A link in the block list of the owner-file. */
	rlist in_block_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/**
	 * Doubly-linked intrusive list of file blocks. Intrusiveness of the
	 * list gives you the full control over the lifetime of the items in the
	 * list without having to use double pointers with performance penalty.
	 */
	rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
	/** How many file descriptors are opened on the file. */
	int refs = 0;
	/** File name. */
	std::string name;
	/** A link in the global file list. */
	rlist in_file_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */
	size_t size = 0;
	size_t block_count = 0;
};

/**
 * Intrusive list of all files. In this case the intrusiveness of the list also
 * grants the ability to remove items from any position in O(1) complexity
 * without having to know their iterator.
 */
static rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

struct filedesc {
	file *atfile;

	/* PUT HERE OTHER MEMBERS */
	size_t pos = 0;
	open_flags flags;

	block *last_block = NULL;
	size_t last_block_idx = 0;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static std::vector<filedesc*> file_descriptors;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
	file *cur_file = NULL;
	file *find_file = NULL;
	rlist_foreach_entry(find_file, &file_list, in_file_list) {
		if (find_file->name == filename) {
			cur_file = find_file;
			break;
		}
	}
	if (!cur_file && !(flags & UFS_CREATE)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	else if (!cur_file && (flags & UFS_CREATE)) {
		file *new_file = new file();
		new_file->name = filename;
		rlist_add_tail(&file_list, &new_file->in_file_list);
		cur_file = new_file;
	}
	filedesc *cur_desc = new filedesc();
	cur_desc->atfile = cur_file;
	cur_file->refs++;

	if ((flags & UFS_READ_WRITE) == UFS_READ_WRITE)
		cur_desc->flags = UFS_READ_WRITE;
	else if ((flags & UFS_READ_ONLY) == UFS_READ_ONLY)
		cur_desc->flags = UFS_READ_ONLY;
	else if ((flags & UFS_WRITE_ONLY) == UFS_WRITE_ONLY)
		cur_desc->flags = UFS_WRITE_ONLY;
	else
		cur_desc->flags = UFS_READ_WRITE;

	file_descriptors.push_back(cur_desc);

	return static_cast<int>(file_descriptors.size());
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	if (fd < 1 || fd > static_cast<int>(file_descriptors.size()) || file_descriptors[fd-1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	filedesc *cur_desc = file_descriptors[fd-1];

	if (cur_desc->flags != UFS_WRITE_ONLY && cur_desc->flags != UFS_READ_WRITE) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (size > MAX_FILE_SIZE - cur_desc->pos) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	size_t write_left = size;
	size_t bytes_written = 0;

	while (write_left > 0) {
		size_t cur_block_idx = cur_desc->pos / BLOCK_SIZE;
		size_t chunk = BLOCK_SIZE - cur_desc->pos % BLOCK_SIZE;
		if (chunk > write_left) {
			chunk = write_left;
		}

		while (cur_block_idx >= cur_desc->atfile->block_count) {
			block *new_block = new block();
			memset(new_block->memory, 0, BLOCK_SIZE);
			rlist_add_tail(&cur_desc->atfile->blocks, &new_block->in_block_list);
			cur_desc->atfile->block_count++;
		}

		block *cur_block = NULL;
		size_t search_idx = 0;

		if (cur_desc->last_block != NULL && cur_block_idx >= cur_desc->last_block_idx) {
			cur_block = cur_desc->last_block;
			search_idx = cur_desc->last_block_idx;
		}
		else {
			cur_block = rlist_first_entry(&cur_desc->atfile->blocks, block, in_block_list);
		}

		for (size_t i = search_idx; i < cur_block_idx; i++) {
			cur_block = rlist_next_entry(cur_block, in_block_list);
		}
		cur_desc->last_block = cur_block;
		cur_desc->last_block_idx = cur_block_idx;

		memcpy(cur_block->memory +  cur_desc->pos % BLOCK_SIZE, buf, chunk);
		
		buf += chunk;
		cur_desc->pos += chunk;
		write_left -= chunk;
		bytes_written += chunk;
	}

	
	if (cur_desc->pos > cur_desc->atfile->size) {
    	cur_desc->atfile->size = cur_desc->pos;
	}

	return bytes_written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	if (fd < 1 || fd > static_cast<int>(file_descriptors.size()) || file_descriptors[fd-1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	filedesc *cur_desc = file_descriptors[fd-1];

	if (cur_desc->flags != UFS_READ_ONLY && cur_desc->flags != UFS_READ_WRITE) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (cur_desc->pos >= cur_desc->atfile->size) {
		return 0;
	}

	size_t read_left = size;
	if (read_left > cur_desc->atfile->size - cur_desc->pos) {
		read_left = cur_desc->atfile->size - cur_desc->pos;
	}
	size_t bytes_read = 0;

	while (read_left > 0) {
		size_t cur_block_idx = cur_desc->pos / BLOCK_SIZE;
		size_t chunk = BLOCK_SIZE - cur_desc->pos % BLOCK_SIZE;
		if (chunk > read_left) {
			chunk = read_left;
		}

		block *cur_block = NULL;
		size_t search_idx = 0;

		if (cur_desc->last_block != NULL && cur_block_idx >= cur_desc->last_block_idx) {
			cur_block = cur_desc->last_block;
			search_idx = cur_desc->last_block_idx;
		}
		else {
			cur_block = rlist_first_entry(&cur_desc->atfile->blocks, block, in_block_list);
		}
		
		for (size_t i = search_idx; i < cur_block_idx; i++) {
			cur_block = rlist_next_entry(cur_block, in_block_list);
		}
		cur_desc->last_block = cur_block;
		cur_desc->last_block_idx = cur_block_idx;

		memcpy(buf, cur_block->memory +  cur_desc->pos % BLOCK_SIZE, chunk);
		
		buf += chunk;
		cur_desc->pos += chunk;
		read_left -= chunk;
		bytes_read += chunk;
	}

	return bytes_read;
}

int
ufs_close(int fd)
{
	if (fd < 1 || fd > static_cast<int>(file_descriptors.size()) || file_descriptors[fd-1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	filedesc *cur_desc = file_descriptors[fd-1];
	file *cur_file = cur_desc->atfile;
	cur_file->refs--;

	if (cur_file->refs == 0 && rlist_empty(&cur_file->in_file_list)) {
		block *cur_block;
		block *tmp;
		rlist_foreach_entry_safe(cur_block, &cur_file->blocks, in_block_list, tmp) {
			rlist_del(&cur_block->in_block_list);
			delete cur_block;
		}
		delete cur_file;
	}

	delete cur_desc;
	file_descriptors[fd-1] = NULL;

	return 0;
}

int
ufs_delete(const char *filename)
{
	file *cur_file;
	rlist_foreach_entry(cur_file, &file_list, in_file_list) {
		if (cur_file->name == filename) {
			break;
		}
	}
	if (!cur_file) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	rlist_del(&cur_file->in_file_list);

	if (cur_file->refs == 0) {
		block *cur_block;
		block *tmp;
		rlist_foreach_entry_safe(cur_block, &cur_file->blocks, in_block_list, tmp) {
			rlist_del(&cur_block->in_block_list);
			delete cur_block;
		}
		delete cur_file;
	}

	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	if (fd < 1 || fd > static_cast<int>(file_descriptors.size()) || file_descriptors[fd-1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	filedesc *cur_desc = file_descriptors[fd-1];

	if (cur_desc->flags != UFS_WRITE_ONLY && cur_desc->flags != UFS_READ_WRITE) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}

	if (new_size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	size_t new_block_count = new_size / BLOCK_SIZE;
	if (new_size % BLOCK_SIZE != 0)
		new_block_count++;
	
	if (new_size > cur_desc->atfile->size) { 
		while (cur_desc->atfile->block_count < new_block_count) {
			block *new_block = new block();
			memset(new_block->memory, 0, BLOCK_SIZE);
			rlist_add_tail(&cur_desc->atfile->blocks, &new_block->in_block_list);
			cur_desc->atfile->block_count++;
		}
	}
	else if (new_size < cur_desc->atfile->size) {
		while (cur_desc->atfile->block_count > new_block_count) {
			block *del_block = rlist_last_entry(&cur_desc->atfile->blocks, block, in_block_list);
			rlist_del(&del_block->in_block_list);
			delete del_block;
			cur_desc->atfile->block_count--;
		}

		for (size_t i = 0; i < file_descriptors.size(); i++) {
			if (file_descriptors[i] != NULL && 
				file_descriptors[i]->atfile == cur_desc->atfile &&
				file_descriptors[i]->pos > new_size
			) {
				file_descriptors[i]->pos = new_size;
			}
		}
	}
	cur_desc->atfile->size = new_size;

	return 0;
}

#endif

void
ufs_destroy(void)
{
	/*
	 * The file_descriptors array is likely to leak even if
	 * you resize it to zero or call clear(). This is because
	 * the vector keeps memory reserved in case more elements
	 * would be added.
	 *
	 * The recommended way of freeing the memory is to swap()
	 * the vector with a temporary empty vector.
	 */
	for (size_t i = 0; i < file_descriptors.size(); i++) {
		if (file_descriptors[i] != NULL) {
			filedesc *cur_desc = file_descriptors[i];
			file *cur_file = cur_desc->atfile;
			cur_file->refs--;

			if (cur_file->refs == 0 && rlist_empty(&cur_file->in_file_list)) {
				block *cur_block;
				block *tmp;
				rlist_foreach_entry_safe(cur_block, &cur_file->blocks, in_block_list, tmp) {
					rlist_del(&cur_block->in_block_list);
					delete cur_block;
				}
				delete cur_file;
			}

			delete cur_desc;
			file_descriptors[i] = NULL;
		}
	}

	std::vector<filedesc*> tmp_empty;
	file_descriptors.swap(tmp_empty);

	while (!rlist_empty(&file_list)) {
		file *cur_file = rlist_entry(file_list.next, file, in_file_list);
		rlist_del(file_list.next);

		block *cur_block;
		block *tmp;
		rlist_foreach_entry_safe(cur_block, &cur_file->blocks, in_block_list, tmp) {
			rlist_del(&cur_block->in_block_list);
			delete cur_block;
		}
		delete cur_file;
	}
}
