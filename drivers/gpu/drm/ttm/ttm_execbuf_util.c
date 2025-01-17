/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/**************************************************************************
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <drm/ttm/ttm_execbuf_util.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_placement.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/module.h>

static void ttm_eu_backoff_reservation_reverse(struct list_head *list,
					      struct ttm_validate_buffer *entry)
{
	list_for_each_entry_continue_reverse(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		dma_resv_unlock(amdkcl_ttm_resvp(bo));
	}
}

void ttm_eu_backoff_reservation(struct ww_acquire_ctx *ticket,
				struct list_head *list)
{
	struct ttm_validate_buffer *entry;

	if (list_empty(list))
		return;

	spin_lock(&ttm_bo_glob.lru_lock);
	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		ttm_bo_move_to_lru_tail(bo, NULL);
		dma_resv_unlock(amdkcl_ttm_resvp(bo));
	}
	spin_unlock(&ttm_bo_glob.lru_lock);

	if (ticket)
		ww_acquire_fini(ticket);
}
EXPORT_SYMBOL(ttm_eu_backoff_reservation);

/*
 * Reserve buffers for validation.
 *
 * If a buffer in the list is marked for CPU access, we back off and
 * wait for that buffer to become free for GPU access.
 *
 * If a buffer is reserved for another validation, the validator with
 * the highest validation sequence backs off and waits for that buffer
 * to become unreserved. This prevents deadlocks when validating multiple
 * buffers in different orders.
 */

int ttm_eu_reserve_buffers(struct ww_acquire_ctx *ticket,
			   struct list_head *list, bool intr,
			   struct list_head *dups)
{
	struct ttm_validate_buffer *entry;
	int ret;

	if (list_empty(list))
		return 0;

	if (ticket)
		ww_acquire_init(ticket, &reservation_ww_class);

	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		ret = __ttm_bo_reserve(bo, intr, (ticket == NULL), ticket);
		if (ret == -EALREADY && dups) {
			struct ttm_validate_buffer *safe = entry;
			entry = list_prev_entry(entry, head);
			list_del(&safe->head);
			list_add(&safe->head, dups);
			continue;
		}

		if (!ret) {
			if (!entry->num_shared)
				continue;

			ret = dma_resv_reserve_shared(amdkcl_ttm_resvp(bo),
								entry->num_shared);
			if (!ret)
				continue;
		}

		/* uh oh, we lost out, drop every reservation and try
		 * to only reserve this buffer, then start over if
		 * this succeeds.
		 */
		ttm_eu_backoff_reservation_reverse(list, entry);

		if (ret == -EDEADLK) {
			if (intr) {
				ret = dma_resv_lock_slow_interruptible(amdkcl_ttm_resvp(bo),
										 ticket);
			} else {
				dma_resv_lock_slow(amdkcl_ttm_resvp(bo), ticket);
				ret = 0;
			}
		}

		if (!ret && entry->num_shared)
			ret = dma_resv_reserve_shared(amdkcl_ttm_resvp(bo),
								entry->num_shared);

		if (unlikely(ret != 0)) {
			if (ret == -EINTR)
				ret = -ERESTARTSYS;
			if (ticket) {
				ww_acquire_done(ticket);
				ww_acquire_fini(ticket);
			}
			return ret;
		}

		/* move this item to the front of the list,
		 * forces correct iteration of the loop without keeping track
		 */
		list_del(&entry->head);
		list_add(&entry->head, list);
	}

	return 0;
}
EXPORT_SYMBOL(ttm_eu_reserve_buffers);

void ttm_eu_fence_buffer_objects(struct ww_acquire_ctx *ticket,
				 struct list_head *list,
				 struct dma_fence *fence)
{
	struct ttm_validate_buffer *entry;

	if (list_empty(list))
		return;

	spin_lock(&ttm_bo_glob.lru_lock);
	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		if (entry->num_shared)
			dma_resv_add_shared_fence(amdkcl_ttm_resvp(bo), fence);
		else
			dma_resv_add_excl_fence(amdkcl_ttm_resvp(bo), fence);
		ttm_bo_move_to_lru_tail(bo, NULL);
		dma_resv_unlock(amdkcl_ttm_resvp(bo));
	}
	spin_unlock(&ttm_bo_glob.lru_lock);
	if (ticket)
		ww_acquire_fini(ticket);
}
EXPORT_SYMBOL(ttm_eu_fence_buffer_objects);
