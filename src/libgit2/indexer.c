/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "indexer.h"
#include "pack.h"
#include "packfile_parser.h"
#include "repository.h"

#include "git2_util.h"

#include "git2/indexer.h"

/* TODO: tweak? */
#define READ_CHUNK_SIZE (1024 * 256)

size_t git_indexer__max_objects = UINT32_MAX;

struct object_entry {
	git_oid id;
	size_t position;
};

struct delta_entry {
	git_object_t type;
	union {
		git_oid ref;
		size_t offset;
	} base;
};

struct git_indexer {
	git_odb *odb;
	git_oid_t oid_type;

	/* TODO: verify! connectivity checks! */
	unsigned int do_fsync:1,
	             do_verify:1;
	unsigned int mode;

	git_indexer_progress_cb progress_cb;
	void *progress_payload;

	git_str path;
	int fd;

	git_packfile_parser parser;

	uint32_t entries;
	bool complete;

	/* Current object / delta being parsed */
	size_t current_position;
	git_object_t current_type;
	size_t current_size;
	git_oid current_ref; /* current ref delta base */
	size_t current_offset; /* current ofs delta base */

	git_vector objects; /* vector of `struct object_entry` */
	git_vector deltas;  /* vector of `struct delta_entry` */

	git_oid trailer_oid;
	char name[(GIT_HASH_MAX_SIZE * 2) + 1];
};

int git_indexer_options_init(
	git_indexer_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(opts,
		version, git_indexer_options, GIT_INDEXER_OPTIONS_INIT);

	return 0;
}

static int objects_cmp(const void *a, const void *b)
{
	const struct object_entry *entry_a = a;
	const struct object_entry *entry_b = b;

	return git_oid__cmp(&entry_a->id, &entry_b->id);
}

static int parse_packfile_header(
	uint32_t version,
	uint32_t entries,
	void *data)
{
	git_indexer *indexer = (git_indexer *)data;

	GIT_UNUSED(version);

	printf("--> header cb: %d %d\n", (int)version, (int)entries);

	/* TODO: is 50% a good number? not convinced. */
	if (git_vector_init(&indexer->objects, entries, objects_cmp) < 0 ||
	    git_vector_init(&indexer->deltas, entries / 2, NULL) < 0)
		return -1;

	indexer->entries = entries;

	return 0;
}

static int parse_object_start(
	size_t position,
	git_object_t type,
	size_t size,
	void *data)
{
	git_indexer *indexer = (git_indexer *)data;

	printf("--> object start: %d %d %d\n", (int)position, (int)type, (int)size);

	indexer->current_position = position;
	indexer->current_type = type;
	indexer->current_size = size;

	return 0;
}

static int parse_object_complete(
	size_t compressed_size,
	git_oid *oid,
	void *data)
{
	git_indexer *indexer = (git_indexer *)data;
	struct object_entry *entry;

	printf("--> object complete: %d %d %d %s\n", (int)indexer->current_size, (int)compressed_size, (int)indexer->current_position, git_oid_tostr_s(oid));

	/* TODO: pool? */
	entry = git__malloc(sizeof(struct object_entry));
	GIT_ERROR_CHECK_ALLOC(entry);

	git_oid_cpy(&entry->id, oid);
	entry->position = indexer->current_position;

	return git_vector_insert(&indexer->objects, entry);
}

static int parse_delta_start(
	size_t position,
	git_object_t type,
	size_t size,
	git_oid *delta_ref,
	size_t delta_offset,
	void *data)
{
	git_indexer *indexer = (git_indexer *)data;

	printf("--> DELTA start: %d %d %d\n", (int)position, (int)type, (int)size);
	if (type == GIT_OBJECT_REF_DELTA)
		printf("--> DELTA start: %d %d %d / %s\n", (int)position, (int)type, (int)size, git_oid_tostr_s(delta_ref));
	else
		printf("--> DELTA start: %d %d %d / %d = %d\n", (int)position, (int)type, (int)size, (int)delta_offset, (int)(position - delta_offset));


	GIT_UNUSED(delta_ref);
	GIT_UNUSED(delta_offset);

	/* TODO: avoid double copy - preallocate the entry */
	indexer->current_position = position;
	indexer->current_type = type;
	indexer->current_size = size;

	if (type == GIT_OBJECT_REF_DELTA)
		git_oid_cpy(&indexer->current_ref, delta_ref);
	else
		indexer->current_offset = delta_offset;

	return 0;
}

static int parse_delta_complete(
	size_t compressed_size,
	void *data)
{
	git_indexer *indexer = (git_indexer *)data;
	struct delta_entry *entry;

	printf("--> DELTA complete: %d\n", (int)compressed_size);

	/* TODO: pool? */
	entry = git__malloc(sizeof(struct delta_entry));
	GIT_ERROR_CHECK_ALLOC(entry);

	entry->type = indexer->current_type;

	if (entry->type == GIT_OBJECT_REF_DELTA)
		git_oid_cpy(&entry->base.ref, &indexer->current_ref);
	else
		entry->base.offset = indexer->current_offset;

	return git_vector_insert(&indexer->deltas, entry);
}

static int parse_packfile_complete(
	const unsigned char *checksum,
	size_t checksum_len,
	void *data)
{
	git_indexer *indexer = (git_indexer *)data;

	GIT_UNUSED(checksum);
	GIT_UNUSED(checksum_len);

	indexer->complete = true;
	return 0;
}

static int indexer_new(
	git_indexer **out,
	const char *parent_path,
	git_oid_t oid_type,
	unsigned int mode,
	git_odb *odb,
	git_indexer_options *in_opts)
{
	git_indexer_options opts = GIT_INDEXER_OPTIONS_INIT;
	git_indexer *indexer;
	git_str path = GIT_STR_INIT;
	int error;

	if (in_opts)
		memcpy(&opts, in_opts, sizeof(opts));

	indexer = git__calloc(1, sizeof(git_indexer));
	GIT_ERROR_CHECK_ALLOC(indexer);

	indexer->oid_type = oid_type;
	indexer->odb = odb;
	indexer->progress_cb = opts.progress_cb;
	indexer->progress_payload = opts.progress_cb_payload;
	indexer->mode = mode ? mode : GIT_PACK_FILE_MODE;
	indexer->do_verify = opts.verify;

	if (git_repository__fsync_gitdir)
		indexer->do_fsync = 1;

	if ((error = git_packfile_parser_init(&indexer->parser, indexer->oid_type)) < 0 ||
	    (error = git_str_joinpath(&path, parent_path, "pack")) < 0 ||
	    (error = indexer->fd = git_futils_mktmp(&indexer->path, path.ptr, indexer->mode)) < 0)
		goto done;

	indexer->parser.packfile_header = parse_packfile_header;
	indexer->parser.object_start = parse_object_start;
	indexer->parser.object_complete = parse_object_complete;
	indexer->parser.delta_start = parse_delta_start;
	indexer->parser.delta_complete = parse_delta_complete;
	indexer->parser.packfile_complete = parse_packfile_complete;
	indexer->parser.callback_data = indexer;

done:
	git_str_dispose(&path);

	if (error < 0) {
		git__free(indexer);
		return -1;
	}

	*out = indexer;
	return 0;
}

#ifdef GIT_EXPERIMENTAL_SHA256
int git_indexer_new(
	git_indexer **out,
	const char *path,
	git_oid_t oid_type,
	git_indexer_options *opts)
{
	unsigned int mode = opts ? opts->mode : 0;
	git_odb *odb = opts ? opts->odb : NULL;

	return indexer_new(out, path, oid_type, mode, odb, opts);
}
#else
int git_indexer_new(
	git_indexer **out,
	const char *path,
	unsigned int mode,
	git_odb *odb,
	git_indexer_options *opts)
{
	return indexer_new(out, path, GIT_OID_SHA1, mode, odb, opts);
}
#endif

void git_indexer__set_fsync(git_indexer *indexer, int do_fsync)
{
	indexer->do_fsync = !!do_fsync;
}

const char *git_indexer_name(const git_indexer *indexer)
{
	return indexer->name;
}

#ifndef GIT_DEPRECATE_HARD
const git_oid *git_indexer_hash(const git_indexer *indexer)
{
	return &indexer->trailer_oid;
}
#endif

static int append_data(
	git_indexer *indexer,
	const void *data,
	size_t len)
{
	size_t chunk_len;

	while (len > 0) {
		chunk_len = min(len, SSIZE_MAX);

		if ((p_write(indexer->fd, data, chunk_len)) < 0)
			return -1;

		data += chunk_len;
		len -= chunk_len;
	}

	return 0;
}

int git_indexer_append(
	git_indexer *indexer,
	const void *data,
	size_t len,
	git_indexer_progress *stats)
{
	GIT_ASSERT_ARG(indexer && (!len || data));

	printf("appending %d...\n", (int)len);



	GIT_UNUSED(stats);


	/*
	 * Take two passes with the data given to us: first, actually do the
	 * appending to the packfile. Next, do whatever parsing we can.
	 */

	if (append_data(indexer, data, len) < 0)
		return -1;

	if (git_packfile_parser_parse(&indexer->parser, data, len) < 0)
		return -1;

	return 0;
}

static int resolve_ofs_delta(git_indexer *indexer, size_t offset)
{
	/*
	 * TODO: we should cache small delta bases?
	 *
	 * How often are delta bases re-used? How far back will git look for
	 * delta bases - is it constant? If so, we should call _all_ objects
	 * and expire them off the back as we continue to read them.
	 *
	 * Does git have a size limit on delta bases? Probably not, but maybe
	 * a size limit on the postimage. But we can probably assume that a
	 * delta base is roughly the same size as postimage and avoid caching
	 * those objects.
	 */

	GIT_UNUSED(indexer);
	printf("--> DELTA resolve: ofs %d\n", (int)offset);

	return 0;
}

static int resolve_final_deltas(git_indexer *indexer)
{
	size_t deltas_len = git_vector_length(&indexer->deltas);
	struct delta_entry *delta_entry;
	size_t i;

	while (deltas_len > 0) {
		git_vector_foreach(&indexer->deltas, i, delta_entry) {
			if (delta_entry->type == GIT_OBJECT_REF_DELTA) {
				printf("--> DELTA removed: ref %s\n", git_oid_tostr_s(&delta_entry->base.ref));
			} else {
				if (resolve_ofs_delta(indexer, delta_entry->base.offset) < 0)
					return -1;

				printf("--> DELTA removed: ofs %d\n", (int)delta_entry->base.offset);
			}

			deltas_len--;
		}
	}

	return 0;
}

int git_indexer_commit(git_indexer *indexer, git_indexer_progress *stats)
{
	struct object_entry *entry;
	size_t i;

	GIT_ASSERT_ARG(indexer);

	GIT_UNUSED(stats);

	if (!indexer->complete) {
		git_error_set(GIT_ERROR_INDEXER, "incomplete packfile");
		return -1;
	}

	if (resolve_final_deltas(indexer) < 0)
		return -1;

	git_vector_sort(&indexer->objects);

	git_vector_foreach(&indexer->objects, i, entry) {
		printf("--> sorted: %s\n", git_oid_tostr_s(&entry->id));
	}

	return 0;
}

void git_indexer_free(git_indexer *indexer)
{
	if (!indexer)
		return;

	git_vector_free_deep(&indexer->objects);
	git_packfile_parser_dispose(&indexer->parser);
	git__free(indexer);
}
