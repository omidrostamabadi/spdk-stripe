/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"

struct raid0_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;

	/* base delay b/w two i/o chunks for this raid device */
	uint64_t delay_tsc;
};

struct raid0_io_channel {
	/* Array of per-base_bdev counters of outstanding read blocks on this channel */
	struct raid_bdev_timewheel time_wheel;
};

/* Prototypes */
static size_t
raid_bdev_timewheel_init(struct raid_bdev_timewheel *tw, uint64_t res_tsc, uint64_t num_bkts);
static void
raid_bdev_timewheel_add_request(struct raid_bdev_timewheel *tw, struct raid_bdev_io *raid_io, uint64_t delay_idx);
static int
raid_bdev_run_timewheel(void *arg);

/*
 * brief:
 * raid0_bdev_io_completion function is called by lower layers to notify raid
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context (parent raid_bdev_io)
 * returns:
 * none
 */
static void
raid0_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;
	int rc;

	if (success) {
		if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
				  spdk_bdev_get_dif_type(bdev_io->bdev) != SPDK_DIF_DISABLE &&
				  bdev_io->bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {

			rc = raid_bdev_verify_dix_reftag(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
							 bdev_io->u.bdev.md_buf, bdev_io->u.bdev.num_blocks, bdev_io->bdev,
							 bdev_io->u.bdev.offset_blocks);
			if (rc != 0) {
				SPDK_ERRLOG("Reftag verify failed.\n");
				raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	spdk_bdev_free_io(bdev_io);
}

static void raid0_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid0_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid0_submit_rw_request(raid_io);
}

/*
 * brief:
 * raid0_submit_rw_request function is used to submit I/O to the correct
 * member disk for raid0 bdevs.
 * params:
 * raid_io
 * returns:
 * none
 */
static void
raid0_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			pd_strip;
	uint32_t			offset_in_strip;
	uint64_t			pd_lba;
	uint64_t			pd_blocks;
	uint8_t				pd_idx;
	int				ret = 0;
	uint64_t			start_strip;
	uint64_t			end_strip;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		assert(false);
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	pd_strip = start_strip / raid_bdev->num_base_bdevs;
	pd_idx = start_strip % raid_bdev->num_base_bdevs;
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;
	pd_blocks = raid_io->num_blocks;
	base_info = &raid_bdev->base_bdev_info[pd_idx];
	if (base_info->desc == NULL) {
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and function callback context
	 */
	assert(raid_ch != NULL);
	base_ch = raid_bdev_channel_get_base_channel(raid_ch, pd_idx);

	io_opts.size = sizeof(io_opts);
	io_opts.memory_domain = raid_io->memory_domain;
	io_opts.memory_domain_ctx = raid_io->memory_domain_ctx;
	io_opts.metadata = raid_io->md_buf;

	if (raid_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch,
						 raid_io->iovs, raid_io->iovcnt,
						 pd_lba, pd_blocks, raid0_bdev_io_completion,
						 raid_io, &io_opts);
	} else if (raid_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		struct spdk_bdev *bdev = &base_info->raid_bdev->bdev;

		if (spdk_unlikely(spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE &&
				  bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
			ret = raid_bdev_verify_dix_reftag(raid_io->iovs, raid_io->iovcnt, io_opts.metadata,
							  pd_blocks, bdev, raid_io->offset_blocks);
			if (ret != 0) {
				SPDK_ERRLOG("bdev io submit error due to DIX verify failure\n");
				raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		ret = raid_bdev_writev_blocks_ext(base_info, base_ch,
						  raid_io->iovs, raid_io->iovcnt,
						  pd_lba, pd_blocks, raid0_bdev_io_completion,
						  raid_io, &io_opts);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", raid_io->type);
		assert(0);
	}

	if (ret == -ENOMEM) {
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid0_submit_rw_request);
	} else if (ret != 0) {
		SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
* submission entry point into the raid0 module
* this is only to support the delay functionality, it only calculates the delay and queues the 
* request to be submitted later
*/
static void
raid0_queue_rw_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint8_t				pd_idx;
	uint64_t			start_strip;
	uint64_t			end_strip;
	struct raid0_io_channel		*raid0_ch;
	struct raid0_info 			*r0info;
	uint64_t delay_tsc;

	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		assert(false);
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	pd_idx = start_strip % raid_bdev->num_base_bdevs;

	raid0_ch = raid_bdev_channel_get_module_ctx(raid_ch);
	
	r0info = raid_bdev->module_private;

	delay_tsc = pd_idx * r0info->delay_tsc;
	
	if (delay_tsc == 0)
		raid0_submit_rw_request(raid_io);
	else
		raid_bdev_timewheel_add_request(&raid0_ch->time_wheel, raid_io, delay_tsc);
}

/* raid0 IO range */
struct raid_bdev_io_range {
	uint64_t	strip_size;
	uint64_t	start_strip_in_disk;
	uint64_t	end_strip_in_disk;
	uint64_t	start_offset_in_strip;
	uint64_t	end_offset_in_strip;
	uint8_t		start_disk;
	uint8_t		end_disk;
	uint8_t		n_disks_involved;
};

static inline void
_raid0_get_io_range(struct raid_bdev_io_range *io_range,
		    uint8_t num_base_bdevs, uint64_t strip_size, uint64_t strip_size_shift,
		    uint64_t offset_blocks, uint64_t num_blocks)
{
	uint64_t	start_strip;
	uint64_t	end_strip;
	uint64_t	total_blocks;

	io_range->strip_size = strip_size;
	total_blocks = offset_blocks + num_blocks - (num_blocks > 0);

	/* The start and end strip index in raid0 bdev scope */
	start_strip = offset_blocks >> strip_size_shift;
	end_strip = total_blocks >> strip_size_shift;
	io_range->start_strip_in_disk = start_strip / num_base_bdevs;
	io_range->end_strip_in_disk = end_strip / num_base_bdevs;

	/* The first strip may have unaligned start LBA offset.
	 * The end strip may have unaligned end LBA offset.
	 * Strips between them certainly have aligned offset and length to boundaries.
	 */
	io_range->start_offset_in_strip = offset_blocks % strip_size;
	io_range->end_offset_in_strip = total_blocks % strip_size;

	/* The base bdev indexes in which start and end strips are located */
	io_range->start_disk = start_strip % num_base_bdevs;
	io_range->end_disk = end_strip % num_base_bdevs;

	/* Calculate how many base_bdevs are involved in io operation.
	 * Number of base bdevs involved is between 1 and num_base_bdevs.
	 * It will be 1 if the first strip and last strip are the same one.
	 */
	io_range->n_disks_involved = spdk_min((end_strip - start_strip + 1), num_base_bdevs);
}

static inline void
_raid0_split_io_range(struct raid_bdev_io_range *io_range, uint8_t disk_idx,
		      uint64_t *_offset_in_disk, uint64_t *_nblocks_in_disk)
{
	uint64_t n_strips_in_disk;
	uint64_t start_offset_in_disk;
	uint64_t end_offset_in_disk;
	uint64_t offset_in_disk;
	uint64_t nblocks_in_disk;
	uint64_t start_strip_in_disk;
	uint64_t end_strip_in_disk;

	start_strip_in_disk = io_range->start_strip_in_disk;
	if (disk_idx < io_range->start_disk) {
		start_strip_in_disk += 1;
	}

	end_strip_in_disk = io_range->end_strip_in_disk;
	if (disk_idx > io_range->end_disk) {
		end_strip_in_disk -= 1;
	}

	assert(end_strip_in_disk >= start_strip_in_disk);
	n_strips_in_disk = end_strip_in_disk - start_strip_in_disk + 1;

	if (disk_idx == io_range->start_disk) {
		start_offset_in_disk = io_range->start_offset_in_strip;
	} else {
		start_offset_in_disk = 0;
	}

	if (disk_idx == io_range->end_disk) {
		end_offset_in_disk = io_range->end_offset_in_strip;
	} else {
		end_offset_in_disk = io_range->strip_size - 1;
	}

	offset_in_disk = start_offset_in_disk + start_strip_in_disk * io_range->strip_size;
	nblocks_in_disk = (n_strips_in_disk - 1) * io_range->strip_size
			  + end_offset_in_disk - start_offset_in_disk + 1;

	SPDK_DEBUGLOG(bdev_raid0,
		      "raid_bdev (strip_size 0x%" PRIx64 ") splits IO to base_bdev (%u) at (0x%" PRIx64 ", 0x%" PRIx64
		      ").\n",
		      io_range->strip_size, disk_idx, offset_in_disk, nblocks_in_disk);

	*_offset_in_disk = offset_in_disk;
	*_nblocks_in_disk = nblocks_in_disk;
}

static void raid0_submit_null_payload_request(struct raid_bdev_io *raid_io);

static void
_raid0_submit_null_payload_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid0_submit_null_payload_request(raid_io);
}

static void
raid0_base_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	spdk_bdev_free_io(bdev_io);
}

/*
 * brief:
 * raid0_submit_null_payload_request function submits the next batch of
 * io requests with range but without payload, like FLUSH and UNMAP, to member disks;
 * it will submit as many as possible unless one base io request fails with -ENOMEM,
 * in which case it will queue itself for later submission.
 * params:
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
raid0_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	struct raid_bdev_io_range	io_range;
	int				ret;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	raid_bdev = raid_io->raid_bdev;

	_raid0_get_io_range(&io_range, raid_bdev->num_base_bdevs,
			    raid_bdev->strip_size, raid_bdev->strip_size_shift,
			    raid_io->offset_blocks, raid_io->num_blocks);

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_io->base_bdev_io_remaining = io_range.n_disks_involved;
	}

	while (raid_io->base_bdev_io_submitted < io_range.n_disks_involved) {
		uint8_t disk_idx;
		uint64_t offset_in_disk;
		uint64_t nblocks_in_disk;

		/* base_bdev is started from start_disk to end_disk.
		 * It is possible that index of start_disk is larger than end_disk's.
		 */
		disk_idx = (io_range.start_disk + raid_io->base_bdev_io_submitted) % raid_bdev->num_base_bdevs;
		base_info = &raid_bdev->base_bdev_info[disk_idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, disk_idx);

		_raid0_split_io_range(&io_range, disk_idx, &offset_in_disk, &nblocks_in_disk);

		switch (raid_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			ret = raid_bdev_unmap_blocks(base_info, base_ch,
						     offset_in_disk, nblocks_in_disk,
						     raid0_base_io_complete, raid_io);
			break;

		case SPDK_BDEV_IO_TYPE_FLUSH:
			ret = raid_bdev_flush_blocks(base_info, base_ch,
						     offset_in_disk, nblocks_in_disk,
						     raid0_base_io_complete, raid_io);
			break;

		default:
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", raid_io->type);
			assert(false);
			ret = -EIO;
		}

		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _raid0_submit_null_payload_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

static void
raid0_ioch_destroy(void *io_device, void *ctx_buf)
{
}

static inline uint64_t 
_us_to_ticks(uint64_t us) {
    return us * spdk_get_ticks_hz() / 1000000ULL;
}

static uint64_t
_get_delay_tsc_from_env(void)
{
	uint64_t delay_tsc;
	uint64_t delay_us;
	char *dstr = getenv("RAID_DELAY_US");

	if (!dstr)
		return 0;
	
	delay_us = strtoull(dstr, NULL, 10);
	
	delay_tsc = _us_to_ticks(delay_us);
	
	return delay_tsc;
}

static int
raid0_ioch_create(void *io_device, void *ctx_buf)
{
	struct raid0_io_channel *raid0_ch = ctx_buf;
	struct raid0_info *r0info = io_device;

	uint64_t dtsc = _get_delay_tsc_from_env();
	r0info->delay_tsc = dtsc;

	if (dtsc != 0) {
		raid_bdev_timewheel_init(&raid0_ch->time_wheel, dtsc, RAID_TW_NUM_BKTS);

		SPDK_POLLER_REGISTER(raid_bdev_run_timewheel, raid0_ch, 0);
	}

	return 0;
}

static void
raid0_io_device_unregister_done(void *io_device)
{
	struct raid0_info *r0info = io_device;

	raid_bdev_module_stop_done(r0info->raid_bdev);

	free(r0info);
}

static int
raid0_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	uint64_t base_bdev_data_size;
	struct raid_base_bdev_info *base_info;

	struct raid0_info *r0info;
	char name[256];

	r0info = calloc(1, sizeof(*r0info));
	if (!r0info) {
		SPDK_ERRLOG("Failed to allocate RAID0 info device structure\n");
		return -ENOMEM;
	}
	r0info->raid_bdev = raid_bdev;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/* Calculate minimum block count from all base bdevs */
		min_blockcnt = spdk_min(min_blockcnt, base_info->data_size);
	}

	base_bdev_data_size = (min_blockcnt >> raid_bdev->strip_size_shift) << raid_bdev->strip_size_shift;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}

	/*
	 * Take the minimum block count based approach where total block count
	 * of raid bdev is the number of base bdev times the minimum block count
	 * of any base bdev.
	 */
	SPDK_DEBUGLOG(bdev_raid0, "min blockcount %" PRIu64 ",  numbasedev %u, strip size shift %u\n",
		      min_blockcnt, raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);

	raid_bdev->bdev.blockcnt = base_bdev_data_size * raid_bdev->num_base_bdevs;

	if (raid_bdev->num_base_bdevs > 1) {
		raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
		raid_bdev->bdev.split_on_optimal_io_boundary = true;
	} else {
		/* Do not need to split reads/writes on single bdev RAID modules. */
		raid_bdev->bdev.optimal_io_boundary = 0;
		raid_bdev->bdev.split_on_optimal_io_boundary = false;
	}

	raid_bdev->module_private = r0info;
	snprintf(name, sizeof(name), "raid0_%s", raid_bdev->bdev.name);
	spdk_io_device_register(r0info, raid0_ioch_create, raid0_ioch_destroy,
				sizeof(struct raid0_io_channel) + RAID_TW_NUM_BKTS * sizeof(struct raid_bdev_tw_bucket),
				name);
	
	SPDK_NOTICELOG("register i/o device named %s\n", name);
	return 0;
}

static bool
raid0_stop(struct raid_bdev *raid_bdev)
{
	struct raid0_info *r0info = raid_bdev->module_private;

	spdk_io_device_unregister(r0info, raid0_io_device_unregister_done);

	return false;
}

static struct spdk_io_channel *
raid0_get_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid0_info *r0info = raid_bdev->module_private;

	return spdk_get_io_channel(r0info);
}

static bool
raid0_resize(struct raid_bdev *raid_bdev)
{
	uint64_t blockcnt;
	int rc;
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	uint64_t base_bdev_data_size;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);

		min_blockcnt = spdk_min(min_blockcnt, base_bdev->blockcnt - base_info->data_offset);
	}

	base_bdev_data_size = (min_blockcnt >> raid_bdev->strip_size_shift) << raid_bdev->strip_size_shift;
	blockcnt = base_bdev_data_size * raid_bdev->num_base_bdevs;

	if (blockcnt == raid_bdev->bdev.blockcnt) {
		return false;
	}

	rc = spdk_bdev_notify_blockcnt_change(&raid_bdev->bdev, blockcnt);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to notify blockcount change\n");
		return false;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}

	return true;
}

/* ---------- Timewheel handler functions - later may need to add a separate C file ----------- */
/*
* Initialize a timewheel and return the *TOTAL* size of the struct in bytes
*/
static size_t
raid_bdev_timewheel_init(struct raid_bdev_timewheel *tw, uint64_t res_tsc, uint64_t num_bkts) {
    tw->res_tsc = res_tsc;
    tw->num_bkts = num_bkts;
	tw->cur_tsc = spdk_get_ticks();
	tw->cur_idx = tw->cur_tsc / res_tsc % num_bkts;
	
    for (uint64_t i = 0; i < num_bkts; i++) {
        TAILQ_INIT(&tw->buckets[i].requests);
    }

	return sizeof(*tw) + num_bkts * sizeof(*tw->buckets);
}

static size_t
raid_bdev_timewheel_process_tick(struct raid_bdev_timewheel *tw) {
    struct raid_bdev_io *raid_io;
	struct raid_bdev_tw_bucket *bucket;
	bucket = &tw->buckets[tw->cur_idx];
	size_t n = 0;
	
	while (!TAILQ_EMPTY(&bucket->requests)) {
		raid_io = TAILQ_FIRST(&bucket->requests);
		TAILQ_REMOVE(&bucket->requests, raid_io, tw_link);
		
		raid0_submit_rw_request(raid_io);
		n++;
	}

	return n;
}

static size_t
raid_bdev_timewheel_process_ticks(struct raid_bdev_timewheel *tw, uint64_t end_tsc) {
	size_t n = 0;
	while (tw->cur_tsc < end_tsc) {
		n += raid_bdev_timewheel_process_tick(tw);
		tw->cur_tsc += tw->res_tsc;
		tw->cur_idx++;
		if (tw->cur_idx >= tw->num_bkts)
			tw->cur_idx = 0;
	}

	return n;
}

static void
raid_bdev_timewheel_add_request(struct raid_bdev_timewheel *tw, struct raid_bdev_io *raid_io, uint64_t delay_tsc) {
    uint64_t tsc = spdk_get_ticks() + delay_tsc;
    uint64_t idx = tsc / tw->res_tsc % tw->num_bkts;
    TAILQ_INSERT_TAIL(&tw->buckets[idx].requests, raid_io, tw_link);
}

static int
raid_bdev_run_timewheel(void *arg) {
	struct raid0_io_channel *raid0_ch = arg;
	
	size_t nreq = 0;
	uint64_t now_tsc = spdk_get_ticks();

	if (now_tsc >= raid0_ch->time_wheel.cur_tsc) {
		nreq = raid_bdev_timewheel_process_ticks(&raid0_ch->time_wheel, now_tsc);
	}

	return nreq ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}


static struct raid_bdev_module g_raid0_module = {
	.level = RAID0,
	.base_bdevs_min = 1,
	.memory_domains_supported = true,
	.dif_supported = true,
	.start = raid0_start,
	.stop = raid0_stop,
	.get_io_channel = raid0_get_io_channel,
	.submit_rw_request = raid0_queue_rw_request,
	.submit_null_payload_request = raid0_submit_null_payload_request,
	.resize = raid0_resize,
};
RAID_MODULE_REGISTER(&g_raid0_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid0)
