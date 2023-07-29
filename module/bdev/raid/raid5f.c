/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/accel.h"

/* Maximum concurrent full stripe writes per io channel */
#define RAID5F_MAX_STRIPES 32
#define NUM_PARITY 2 /* TODO will come from config space */

struct chunk {
	/* Corresponds to base_bdev index */
	uint8_t index;

	/* Array of iovecs */
	struct iovec *iovs;

	/* Number of used iovecs */
	int iovcnt;

	/* Total number of available iovecs in the array */
	int iovcnt_max;

	/* Pointer to buffer with I/O metadata */
	void *md_buf;

	/* Shallow copy of IO request parameters */
	struct spdk_bdev_ext_io_opts ext_opts;
};

struct stripe_request;
typedef void (*stripe_req_xor_cb)(struct stripe_request *stripe_req, int status);

struct stripe_request {
	enum stripe_request_type {
		STRIPE_REQ_WRITE,
	} type;

	struct raid5f_io_channel *r5ch;

	/* The associated raid_bdev_io */
	struct raid_bdev_io *raid_io;

	/* The stripe's index in the raid array. */
	uint64_t stripe_index;

	/* The stripe's parity chunk */
	struct chunk *parity_chunk;

	union {
		struct {
			/* Buffer for stripe parity */
			void **parity_buf;

			/* Buffer for stripe io metadata parity */
			void *parity_md_buf;
		} write;
	};

	/* Array of iovec iterators for each chunk */
	struct spdk_ioviter *chunk_iov_iters;

	/* Array of source buffer pointers for parity calculation */
	void **chunk_xor_buffers;

	/* Array of source buffer pointers for parity calculation of io metadata */
	void **chunk_xor_md_buffers;

	struct {
		size_t len;
		size_t remaining;
		size_t remaining_md;
		int status;
		stripe_req_xor_cb cb;
	} xor;

	TAILQ_ENTRY(stripe_request) link;
        int num_parity;

	/* Array of chunks corresponding to base_bdevs */
	struct chunk chunks[0];
};

struct raid5f_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;

	/* Number of data blocks in a stripe (without parity) */
	uint64_t stripe_blocks;

	/* Number of stripes on this array */
	uint64_t total_stripes;

	/* Alignment for buffer allocation */
	size_t buf_alignment;
};

struct raid5f_io_channel {
	/* All available stripe requests on this channel */
	struct {
		TAILQ_HEAD(, stripe_request) write;
	} free_stripe_requests;

	/* accel_fw channel */
	struct spdk_io_channel *accel_ch;

	/* For retrying xor if accel_ch runs out of resources */
	TAILQ_HEAD(, stripe_request) xor_retry_queue;

	/* For iterating over chunk iovecs during xor calculation */
	void **chunk_xor_buffers;
	struct iovec **chunk_xor_iovs;
	size_t *chunk_xor_iovcnt;
};

#define __CHUNK_IN_RANGE(req, c) \
	c < req->chunks + raid5f_ch_to_r5f_info(req->r5ch)->raid_bdev->num_base_bdevs

#define FOR_EACH_CHUNK_FROM(req, c, from) \
	for (c = from; __CHUNK_IN_RANGE(req, c); c++)

#define FOR_EACH_CHUNK(req, c) \
	FOR_EACH_CHUNK_FROM(req, c, req->chunks)

#define __NEXT_DATA_CHUNK(req, c) \
	c == req->parity_chunk ? c+1 : c

#define FOR_EACH_DATA_CHUNK(req, c) \
	for (c = __NEXT_DATA_CHUNK(req, req->chunks); __CHUNK_IN_RANGE(req, c); \
	     c = __NEXT_DATA_CHUNK(req, c+1))

static inline struct raid5f_info *
raid5f_ch_to_r5f_info(struct raid5f_io_channel *r5ch)
{
	return spdk_io_channel_get_io_device(spdk_io_channel_from_ctx(r5ch));
}

static inline struct stripe_request *
raid5f_chunk_stripe_req(struct chunk *chunk)
{
	return SPDK_CONTAINEROF((chunk - chunk->index), struct stripe_request, chunks);
}

static inline uint8_t
raid5f_stripe_data_chunks_num(const struct raid_bdev *raid_bdev)
{
	return raid_bdev->min_base_bdevs_operational;
}

static inline uint8_t
raid5f_stripe_parity_chunk_index(const struct raid_bdev *raid_bdev, uint64_t stripe_index)
{
	return raid5f_stripe_data_chunks_num(raid_bdev) - stripe_index % raid_bdev->num_base_bdevs;
}

static inline void
raid5f_stripe_request_release(struct stripe_request *stripe_req)
{
	if (spdk_likely(stripe_req->type == STRIPE_REQ_WRITE)) {
		TAILQ_INSERT_HEAD(&stripe_req->r5ch->free_stripe_requests.write, stripe_req, link);
	} else {
		assert(false);
	}
}

static void raid5f_xor_stripe_retry(struct stripe_request *stripe_req);

static void
raid5f_xor_stripe_done(struct stripe_request *stripe_req)
{
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;

	if (stripe_req->xor.status != 0) {
		SPDK_ERRLOG("stripe xor failed: %s\n", spdk_strerror(-stripe_req->xor.status));
	}

	stripe_req->xor.cb(stripe_req, stripe_req->xor.status);

	if (!TAILQ_EMPTY(&r5ch->xor_retry_queue)) {
		stripe_req = TAILQ_FIRST(&r5ch->xor_retry_queue);
		TAILQ_REMOVE(&r5ch->xor_retry_queue, stripe_req, link);
		raid5f_xor_stripe_retry(stripe_req);
	}
}

static void raid5f_xor_stripe_continue(struct stripe_request *stripe_req);

static void
_raid5f_xor_stripe_cb(struct stripe_request *stripe_req, int status)
{
	if (status != 0) {
		stripe_req->xor.status = status;
	}

	if (stripe_req->xor.remaining + stripe_req->xor.remaining_md == 0) {
		raid5f_xor_stripe_done(stripe_req);
	}
}

static void
raid5f_xor_stripe_cb(void *_stripe_req, int status)
{
	struct stripe_request *stripe_req = _stripe_req;

	stripe_req->xor.remaining -= stripe_req->xor.len;

	if (stripe_req->xor.remaining > 0) {
		stripe_req->xor.len = spdk_ioviter_nextv(stripe_req->chunk_iov_iters,
				      stripe_req->r5ch->chunk_xor_buffers);
		raid5f_xor_stripe_continue(stripe_req);
	}

	_raid5f_xor_stripe_cb(stripe_req, status);
}

static void
raid5f_xor_stripe_md_cb(void *_stripe_req, int status)
{
	struct stripe_request *stripe_req = _stripe_req;

	stripe_req->xor.remaining_md = 0;

	_raid5f_xor_stripe_cb(stripe_req, status);
}

static void
raid5f_xor_stripe_continue(struct stripe_request *stripe_req)
{
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	uint8_t n_src = raid5f_stripe_data_chunks_num(raid_bdev);
	uint8_t i;
	int ret;

	assert(stripe_req->xor.len > 0);

	for (i = 0; i < n_src; i++) {
		stripe_req->chunk_xor_buffers[i] = r5ch->chunk_xor_buffers[i];
	}

	ret = spdk_accel_submit_ec(r5ch->accel_ch, r5ch->chunk_xor_buffers[n_src], NUM_PARITY,
				    stripe_req->chunk_xor_buffers, n_src, stripe_req->xor.len,
				    raid5f_xor_stripe_cb, stripe_req);
	if (spdk_unlikely(ret)) {
		if (ret == -ENOMEM) {
			TAILQ_INSERT_HEAD(&r5ch->xor_retry_queue, stripe_req, link);
		} else {
			stripe_req->xor.status = ret;
			raid5f_xor_stripe_done(stripe_req);
		}
	}
}

static void
raid5f_xor_stripe(struct stripe_request *stripe_req, stripe_req_xor_cb cb)
{
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	void *raid_md = spdk_bdev_io_get_md_buf(bdev_io);
	uint32_t raid_md_size = spdk_bdev_get_md_size(&raid_bdev->bdev);
	struct chunk *chunk;
	uint8_t c;

	assert(cb != NULL);
	assert(stripe_req->type == STRIPE_REQ_WRITE);

	c = 0;
	FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
		r5ch->chunk_xor_iovs[c] = chunk->iovs;
		r5ch->chunk_xor_iovcnt[c] = chunk->iovcnt;
		c++;
	}
	r5ch->chunk_xor_iovs[c] = stripe_req->parity_chunk->iovs;
	r5ch->chunk_xor_iovcnt[c] = stripe_req->parity_chunk->iovcnt;

	stripe_req->xor.len = spdk_ioviter_firstv(stripe_req->chunk_iov_iters,
			      raid_bdev->num_base_bdevs,
			      r5ch->chunk_xor_iovs,
			      r5ch->chunk_xor_iovcnt,
			      r5ch->chunk_xor_buffers);
	stripe_req->xor.remaining = raid_bdev->strip_size << raid_bdev->blocklen_shift;
	stripe_req->xor.status = 0;
	stripe_req->xor.cb = cb;

	if (raid_md != NULL) {
		uint8_t n_src = raid5f_stripe_data_chunks_num(raid_bdev);
		uint64_t len = raid_bdev->strip_size * raid_md_size;
		int ret;

		stripe_req->xor.remaining_md = len;

		c = 0;
		FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
			stripe_req->chunk_xor_md_buffers[c] = chunk->md_buf;
			c++;
		}

		ret = spdk_accel_submit_xor(stripe_req->r5ch->accel_ch, stripe_req->write.parity_md_buf,
					    stripe_req->chunk_xor_md_buffers, n_src, len,
					    raid5f_xor_stripe_md_cb, stripe_req);
		if (spdk_unlikely(ret)) {
			if (ret == -ENOMEM) {
				TAILQ_INSERT_HEAD(&stripe_req->r5ch->xor_retry_queue, stripe_req, link);
			} else {
				stripe_req->xor.status = ret;
				raid5f_xor_stripe_done(stripe_req);
			}
			return;
		}
	}

	raid5f_xor_stripe_continue(stripe_req);
}

static void
raid5f_xor_stripe_retry(struct stripe_request *stripe_req)
{
	if (stripe_req->xor.remaining_md) {
		raid5f_xor_stripe(stripe_req, stripe_req->xor.cb);
	} else {
		raid5f_xor_stripe_continue(stripe_req);
	}
}

static void
raid5f_stripe_request_chunk_write_complete(struct stripe_request *stripe_req,
		enum spdk_bdev_io_status status)
{
	if (raid_bdev_io_complete_part(stripe_req->raid_io, 1, status)) {
		raid5f_stripe_request_release(stripe_req);
	}
}

static void
raid5f_chunk_complete_bdev_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct chunk *chunk = cb_arg;
	struct stripe_request *stripe_req = raid5f_chunk_stripe_req(chunk);
	enum spdk_bdev_io_status status = success ? SPDK_BDEV_IO_STATUS_SUCCESS :
					  SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_free_io(bdev_io);

	if (spdk_likely(stripe_req->type == STRIPE_REQ_WRITE)) {
		raid5f_stripe_request_chunk_write_complete(stripe_req, status);
	} else {
		assert(false);
	}
}

static void raid5f_stripe_request_submit_chunks(struct stripe_request *stripe_req);

static void
raid5f_chunk_submit_retry(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;
	struct stripe_request *stripe_req = raid_io->module_private;

	raid5f_stripe_request_submit_chunks(stripe_req);
}

static inline void
raid5f_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = bdev_io->u.bdev.md_buf;
}

static int
raid5f_chunk_submit(struct chunk *chunk)
{
	struct stripe_request *stripe_req = raid5f_chunk_stripe_req(chunk);
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk->index];
	struct spdk_io_channel *base_ch = raid_io->raid_ch->base_channel[chunk->index];
	uint64_t base_offset_blocks = (stripe_req->stripe_index << raid_bdev->strip_size_shift);
	int ret;

	raid5f_init_ext_io_opts(bdev_io, &chunk->ext_opts);
	chunk->ext_opts.metadata = chunk->md_buf;

	switch (stripe_req->type) {
	case STRIPE_REQ_WRITE:
		ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch, chunk->iovs, chunk->iovcnt,
						  base_offset_blocks, raid_bdev->strip_size,
						  raid5f_chunk_complete_bdev_io, chunk,
						  &chunk->ext_opts);
		break;
	default:
		assert(false);
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret)) {
		if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, raid5f_chunk_submit_retry);
		} else {
			/*
			 * Implicitly complete any I/Os not yet submitted as FAILED. If completing
			 * these means there are no more to complete for the stripe request, we can
			 * release the stripe request as well.
			 */
			uint64_t base_bdev_io_not_submitted = raid_bdev->num_base_bdevs -
							      raid_io->base_bdev_io_submitted;

			if (raid_bdev_io_complete_part(stripe_req->raid_io, base_bdev_io_not_submitted,
						       SPDK_BDEV_IO_STATUS_FAILED)) {
				raid5f_stripe_request_release(stripe_req);
			}
		}
	}

	return ret;
}

static int
raid5f_chunk_set_iovcnt(struct chunk *chunk, int iovcnt)
{
	if (iovcnt > chunk->iovcnt_max) {
		struct iovec *iovs = chunk->iovs;

		iovs = realloc(iovs, iovcnt * sizeof(*iovs));
		if (!iovs) {
			return -ENOMEM;
		}
		chunk->iovs = iovs;
		chunk->iovcnt_max = iovcnt;
	}
	chunk->iovcnt = iovcnt;

	return 0;
}

static int
raid5f_stripe_request_map_iovecs(struct stripe_request *stripe_req)
{
	struct raid_bdev *raid_bdev = stripe_req->raid_io->raid_bdev;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(stripe_req->raid_io);
	const struct iovec *raid_io_iovs = bdev_io->u.bdev.iovs;
	int raid_io_iovcnt = bdev_io->u.bdev.iovcnt;
	void *raid_io_md = spdk_bdev_io_get_md_buf(bdev_io);
	uint32_t raid_io_md_size = spdk_bdev_get_md_size(&raid_bdev->bdev);
	struct chunk *chunk;
	int raid_io_iov_idx = 0;
	size_t raid_io_offset = 0;
	size_t raid_io_iov_offset = 0;
	int i;

	FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
		int chunk_iovcnt = 0;
		uint64_t len = raid_bdev->strip_size << raid_bdev->blocklen_shift;
		size_t off = raid_io_iov_offset;
		int ret;

		for (i = raid_io_iov_idx; i < raid_io_iovcnt; i++) {
			chunk_iovcnt++;
			off += raid_io_iovs[i].iov_len;
			if (off >= raid_io_offset + len) {
				break;
			}
		}

		assert(raid_io_iov_idx + chunk_iovcnt <= raid_io_iovcnt);

		ret = raid5f_chunk_set_iovcnt(chunk, chunk_iovcnt);
		if (ret) {
			return ret;
		}

		if (raid_io_md) {
			chunk->md_buf = raid_io_md +
					(raid_io_offset >> raid_bdev->blocklen_shift) * raid_io_md_size;
		}

		for (i = 0; i < chunk_iovcnt; i++) {
			struct iovec *chunk_iov = &chunk->iovs[i];
			const struct iovec *raid_io_iov = &raid_io_iovs[raid_io_iov_idx];
			size_t chunk_iov_offset = raid_io_offset - raid_io_iov_offset;

			chunk_iov->iov_base = raid_io_iov->iov_base + chunk_iov_offset;
			chunk_iov->iov_len = spdk_min(len, raid_io_iov->iov_len - chunk_iov_offset);
			raid_io_offset += chunk_iov->iov_len;
			len -= chunk_iov->iov_len;

			if (raid_io_offset >= raid_io_iov_offset + raid_io_iov->iov_len) {
				raid_io_iov_idx++;
				raid_io_iov_offset += raid_io_iov->iov_len;
			}
		}

		if (spdk_unlikely(len > 0)) {
			return -EINVAL;
		}
	}
        SPDK_ERRLOG("populate parity chunk.\n");
        for (i = 0; i < NUM_PARITY; i++) {
            stripe_req->parity_chunk->iovs[i].iov_base = stripe_req->write.parity_buf[i];
            stripe_req->parity_chunk->iovs[i].iov_len = raid_bdev->strip_size << raid_bdev->blocklen_shift;
        }
        stripe_req->parity_chunk->iovcnt = NUM_PARITY;
	stripe_req->parity_chunk->md_buf = stripe_req->write.parity_md_buf;

	return 0;
}

static void
raid5f_stripe_request_submit_chunks(struct stripe_request *stripe_req)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct chunk *start = &stripe_req->chunks[raid_io->base_bdev_io_submitted];
	struct chunk *chunk;

	FOR_EACH_CHUNK_FROM(stripe_req, chunk, start) {
		if (spdk_unlikely(raid5f_chunk_submit(chunk) != 0)) {
			break;
		}
		raid_io->base_bdev_io_submitted++;
	}
}

static void
raid5f_stripe_write_request_xor_done(struct stripe_request *stripe_req, int status)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;

	if (status != 0) {
		raid5f_stripe_request_release(stripe_req);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		raid5f_stripe_request_submit_chunks(stripe_req);
	}
}

static int
raid5f_submit_write_request(struct raid_bdev_io *raid_io, uint64_t stripe_index)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid5f_io_channel *r5ch = spdk_io_channel_get_ctx(raid_io->raid_ch->module_channel);
	struct stripe_request *stripe_req;
	int ret;

	stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests.write);
	if (!stripe_req) {
		return -ENOMEM;
	}

	stripe_req->stripe_index = stripe_index;
	stripe_req->parity_chunk = stripe_req->chunks + raid5f_stripe_parity_chunk_index(raid_bdev,
				   stripe_req->stripe_index);
	stripe_req->raid_io = raid_io;
    	stripe_req->num_parity = NUM_PARITY;

	ret = raid5f_stripe_request_map_iovecs(stripe_req);
	if (spdk_unlikely(ret)) {
		return ret;
	}

	TAILQ_REMOVE(&r5ch->free_stripe_requests.write, stripe_req, link);

	raid_io->module_private = stripe_req;
	raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;

	raid5f_xor_stripe(stripe_req, raid5f_stripe_write_request_xor_done);

	return 0;
}

static void
raid5f_chunk_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete(raid_io, success ? SPDK_BDEV_IO_STATUS_SUCCESS :
			      SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid5f_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid5f_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid5f_submit_rw_request(raid_io);
}

static int
raid5f_submit_read_request(struct raid_bdev_io *raid_io, uint64_t stripe_index,
			   uint64_t stripe_offset)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	uint8_t chunk_data_idx = stripe_offset >> raid_bdev->strip_size_shift;
	uint8_t p_idx = raid5f_stripe_parity_chunk_index(raid_bdev, stripe_index);
	uint8_t chunk_idx = chunk_data_idx < p_idx ? chunk_data_idx : chunk_data_idx + 1;
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk_idx];
	struct spdk_io_channel *base_ch = raid_io->raid_ch->base_channel[chunk_idx];
	uint64_t chunk_offset = stripe_offset - (chunk_data_idx << raid_bdev->strip_size_shift);
	uint64_t base_offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + chunk_offset;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts io_opts;
	int ret;

	raid5f_init_ext_io_opts(bdev_io, &io_opts);
	ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch, bdev_io->u.bdev.iovs,
					 bdev_io->u.bdev.iovcnt,
					 base_offset_blocks, bdev_io->u.bdev.num_blocks, raid5f_chunk_read_complete, raid_io,
					 &io_opts);

	if (spdk_unlikely(ret == -ENOMEM)) {
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5f_submit_rw_request);
		return 0;
	}

	return ret;
}

static void
raid5f_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
	uint64_t stripe_index = offset_blocks / r5f_info->stripe_blocks;
	uint64_t stripe_offset = offset_blocks % r5f_info->stripe_blocks;
	int ret;
	SPDK_ERRLOG("raid5f_submit_rw_request.\n");
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		assert(bdev_io->u.bdev.num_blocks <= raid_bdev->strip_size);
		ret = raid5f_submit_read_request(raid_io, stripe_index, stripe_offset);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
                SPDK_ERRLOG("SPDK_BDEV_IO_TYPE_WRITE .\n");
		assert(stripe_offset == 0);
		assert(bdev_io->u.bdev.num_blocks == r5f_info->stripe_blocks);
		ret = raid5f_submit_write_request(raid_io, stripe_index);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret)) {
		raid_bdev_io_complete(raid_io, ret == -ENOMEM ? SPDK_BDEV_IO_STATUS_NOMEM :
				      SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
raid5f_stripe_request_free(struct stripe_request *stripe_req)
{
	struct chunk *chunk;

	FOR_EACH_CHUNK(stripe_req, chunk) {
		free(chunk->iovs);
	}

	if (stripe_req->type == STRIPE_REQ_WRITE) {
		spdk_dma_free(stripe_req->write.parity_buf);
		spdk_dma_free(stripe_req->write.parity_md_buf);
	} else {
		assert(false);
	}

	free(stripe_req->chunk_xor_buffers);
	free(stripe_req->chunk_xor_md_buffers);
	free(stripe_req->chunk_iov_iters);

	free(stripe_req);
}

static struct stripe_request *
raid5f_stripe_request_alloc(struct raid5f_io_channel *r5ch, enum stripe_request_type type)
{
	struct raid5f_info *r5f_info = raid5f_ch_to_r5f_info(r5ch);
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t raid_io_md_size = spdk_bdev_get_md_size(&raid_bdev->bdev);
	struct stripe_request *stripe_req;
	struct chunk *chunk;

	stripe_req = calloc(1, sizeof(*stripe_req) + sizeof(*chunk) * raid_bdev->num_base_bdevs);
	if (!stripe_req) {
		return NULL;
	}

	stripe_req->r5ch = r5ch;
	stripe_req->type = type;

	FOR_EACH_CHUNK(stripe_req, chunk) {
		chunk->index = chunk - stripe_req->chunks;
		chunk->iovcnt_max = 4;
		chunk->iovs = calloc(chunk->iovcnt_max, sizeof(chunk->iovs[0]));
		if (!chunk->iovs) {
			goto err;
		}
	}

	if (type == STRIPE_REQ_WRITE) {
		stripe_req->write.parity_buf = spdk_dma_malloc(NUM_PARITY*(raid_bdev->strip_size << raid_bdev->blocklen_shift),
					       r5f_info->buf_alignment, NULL);
		if (!stripe_req->write.parity_buf) {
			goto err;
		}

		if (raid_io_md_size != 0) {
			stripe_req->write.parity_md_buf = spdk_dma_malloc(NUM_PARITY*(raid_bdev->strip_size * raid_io_md_size),
							  r5f_info->buf_alignment, NULL);
			if (!stripe_req->write.parity_md_buf) {
				goto err;
			}
		}
	} else {
		assert(false);
		return NULL;
	}

	stripe_req->chunk_iov_iters = malloc(SPDK_IOVITER_SIZE(raid_bdev->num_base_bdevs));
	if (!stripe_req->chunk_iov_iters) {
		goto err;
	}

	stripe_req->chunk_xor_buffers = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
					       sizeof(stripe_req->chunk_xor_buffers[0]));
	if (!stripe_req->chunk_xor_buffers) {
		goto err;
	}

	stripe_req->chunk_xor_md_buffers = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
					   sizeof(stripe_req->chunk_xor_md_buffers[0]));
	if (!stripe_req->chunk_xor_md_buffers) {
		goto err;
	}

	return stripe_req;
err:
	raid5f_stripe_request_free(stripe_req);
	return NULL;
}

static void
raid5f_ioch_destroy(void *io_device, void *ctx_buf)
{
	struct raid5f_io_channel *r5ch = ctx_buf;
	struct stripe_request *stripe_req;

	assert(TAILQ_EMPTY(&r5ch->xor_retry_queue));

	while ((stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests.write))) {
		TAILQ_REMOVE(&r5ch->free_stripe_requests.write, stripe_req, link);
		raid5f_stripe_request_free(stripe_req);
	}

	if (r5ch->accel_ch) {
		spdk_put_io_channel(r5ch->accel_ch);
	}

	free(r5ch->chunk_xor_buffers);
	free(r5ch->chunk_xor_iovs);
	free(r5ch->chunk_xor_iovcnt);
}

static int
raid5f_ioch_create(void *io_device, void *ctx_buf)
{
	struct raid5f_io_channel *r5ch = ctx_buf;
	struct raid5f_info *r5f_info = io_device;
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	int i;

	TAILQ_INIT(&r5ch->free_stripe_requests.write);
	TAILQ_INIT(&r5ch->xor_retry_queue);

	for (i = 0; i < RAID5F_MAX_STRIPES; i++) {
		struct stripe_request *stripe_req;

		stripe_req = raid5f_stripe_request_alloc(r5ch, STRIPE_REQ_WRITE);
		if (!stripe_req) {
			goto err;
		}

		TAILQ_INSERT_HEAD(&r5ch->free_stripe_requests.write, stripe_req, link);
	}
	SPDK_ERRLOG("num_base_bdev %d\n",raid_bdev->num_base_bdevs);
	r5ch->accel_ch = spdk_accel_get_io_channel();
	if (!r5ch->accel_ch) {
		SPDK_ERRLOG("Failed to get accel framework's IO channel\n");
		goto err;
	}

	r5ch->chunk_xor_buffers = calloc(raid_bdev->num_base_bdevs, sizeof(*r5ch->chunk_xor_buffers));
	if (!r5ch->chunk_xor_buffers) {
		goto err;
	}

	r5ch->chunk_xor_iovs = calloc(raid_bdev->num_base_bdevs, sizeof(*r5ch->chunk_xor_iovs));
	if (!r5ch->chunk_xor_iovs) {
		goto err;
	}

	r5ch->chunk_xor_iovcnt = calloc(raid_bdev->num_base_bdevs, sizeof(*r5ch->chunk_xor_iovcnt));
	if (!r5ch->chunk_xor_iovcnt) {
		goto err;
	}
	SPDK_ERRLOG("io channel create successful.\n");
	return 0;
err:
	SPDK_ERRLOG("Failed to initialize io channel\n");
	raid5f_ioch_destroy(r5f_info, r5ch);
	return -ENOMEM;
}

static int
raid5f_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	struct raid5f_info *r5f_info;
	size_t alignment = 0;

	r5f_info = calloc(1, sizeof(*r5f_info));
	if (!r5f_info) {
		SPDK_ERRLOG("Failed to allocate r5f_info\n");
		return -ENOMEM;
	}
	r5f_info->raid_bdev = raid_bdev;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *base_bdev;

		base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		min_blockcnt = spdk_min(min_blockcnt, base_bdev->blockcnt);
		alignment = spdk_max(alignment, spdk_bdev_get_buf_align(base_bdev));
	}

	r5f_info->total_stripes = min_blockcnt / raid_bdev->strip_size;
	r5f_info->stripe_blocks = raid_bdev->strip_size * raid5f_stripe_data_chunks_num(raid_bdev);
	r5f_info->buf_alignment = alignment;

	raid_bdev->bdev.blockcnt = r5f_info->stripe_blocks * r5f_info->total_stripes;
	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
	raid_bdev->bdev.split_on_optimal_io_boundary = true;
	raid_bdev->bdev.write_unit_size = r5f_info->stripe_blocks;
	raid_bdev->bdev.split_on_write_unit = true;

	raid_bdev->module_private = r5f_info;

	spdk_io_device_register(r5f_info, raid5f_ioch_create, raid5f_ioch_destroy,
				sizeof(struct raid5f_io_channel), NULL);

	return 0;
}

static void
raid5f_io_device_unregister_done(void *io_device)
{
	struct raid5f_info *r5f_info = io_device;

	raid_bdev_module_stop_done(r5f_info->raid_bdev);

	free(r5f_info);
}

static bool
raid5f_stop(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;

	spdk_io_device_unregister(r5f_info, raid5f_io_device_unregister_done);
        SPDK_ERRLOG("raid5f_stop.\n");
	return false;
}

static struct spdk_io_channel *
raid5f_get_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;

	return spdk_get_io_channel(r5f_info);
}

static struct raid_bdev_module g_raid5f_module = {
	.level = RAID5F,
	.base_bdevs_min = 3,
	.base_bdevs_constraint = {CONSTRAINT_MAX_BASE_BDEVS_REMOVED, 1},
	.start = raid5f_start,
	.stop = raid5f_stop,
	.submit_rw_request = raid5f_submit_rw_request,
	.get_io_channel = raid5f_get_io_channel,
};
RAID_MODULE_REGISTER(&g_raid5f_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid5f)
