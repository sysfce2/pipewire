/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <spa/utils/type.h>
#include <spa/utils/result.h>

#include "pipewire/log.h"
#include "pipewire/work-queue.h"

PW_LOG_TOPIC_EXTERN(log_work_queue);
#define PW_LOG_TOPIC_DEFAULT log_work_queue

/** \cond */
struct work_item {
	void *obj;
	uint32_t id;
	uint32_t seq;
	pw_work_func_t func;
	void *data;
	struct spa_list link;
	int res;
};

struct pw_work_queue {
	struct pw_loop *loop;

	struct spa_source *wakeup;

	struct spa_list work_list;
	struct spa_list free_list;
	uint32_t counter;
	uint32_t n_queued;
};
/** \endcond */

static void process_work_queue(void *data, uint64_t count)
{
	struct pw_work_queue *this = data;
	struct work_item *item, *tmp;

	spa_list_for_each_safe(item, tmp, &this->work_list, link) {
		if (item->seq != SPA_ID_INVALID) {
			pw_log_debug("%p: n_queued:%d waiting for item %p seq:%d id:%u", this,
				     this->n_queued, item->obj, item->seq, item->id);
			continue;
		}

		if (item->res == -EBUSY &&
		    item != spa_list_first(&this->work_list, struct work_item, link)) {
			pw_log_debug("%p: n_queued:%d sync item %p not head id:%u", this,
				     this->n_queued, item->obj, item->id);
			continue;
		}

		spa_list_remove(&item->link);
		this->n_queued--;

		if (item->func) {
			pw_log_debug("%p: n_queued:%d process work item %p seq:%d res:%d id:%u",
					this, this->n_queued, item->obj, item->seq, item->res,
					item->id);
			item->func(item->obj, item->data, item->res, item->id);
		}
		spa_list_append(&this->free_list, &item->link);
	}
}

/** Create a new \ref pw_work_queue
 *
 * \param loop the loop to use
 * \return a newly allocated work queue
 *
 */
struct pw_work_queue *pw_work_queue_new(struct pw_loop *loop)
{
	struct pw_work_queue *this;
	int res;

	this = calloc(1, sizeof(struct pw_work_queue));
	if (this == NULL)
		return NULL;

	pw_log_debug("%p: new", this);

	this->loop = loop;

	this->wakeup = pw_loop_add_event(this->loop, process_work_queue, this);
	if (this->wakeup == NULL) {
		res = -errno;
		goto error_free;
	}

	spa_list_init(&this->work_list);
	spa_list_init(&this->free_list);

	return this;

error_free:
	free(this);
	errno = -res;
	return NULL;
}

/** Destroy a work queue
 * \param queue the work queue to destroy
 *
 */
void pw_work_queue_destroy(struct pw_work_queue *queue)
{
	struct work_item *item, *tmp;

	pw_log_debug("%p: destroy", queue);

	pw_loop_destroy_source(queue->loop, queue->wakeup);

	spa_list_for_each_safe(item, tmp, &queue->work_list, link) {
		pw_log_debug("%p: cancel work item %p seq:%d res:%d id:%u",
				queue, item->obj, item->seq, item->res, item->id);
		free(item);
	}
	spa_list_for_each_safe(item, tmp, &queue->free_list, link)
		free(item);

	free(queue);
}

/** Add an item to the work queue
 *
 * \param queue the work queue
 * \param obj the object owning the work item
 * \param res a result code
 * \param func a work function
 * \param data passed to \a func
 *
 */
SPA_EXPORT
uint32_t
pw_work_queue_add(struct pw_work_queue *queue, void *obj, int res, pw_work_func_t func, void *data)
{
	struct work_item *item;
	bool have_work = false;

	if (!spa_list_is_empty(&queue->free_list)) {
		item = spa_list_first(&queue->free_list, struct work_item, link);
		spa_list_remove(&item->link);
	} else {
		item = malloc(sizeof(struct work_item));
		if (item == NULL)
			return SPA_ID_INVALID;
	}
	item->id = ++queue->counter;
	if (item->id == SPA_ID_INVALID)
		item->id = ++queue->counter;

	item->obj = obj;
	item->func = func;
	item->data = data;

	if (SPA_RESULT_IS_ASYNC(res)) {
		item->seq = SPA_RESULT_ASYNC_SEQ(res);
		item->res = res;
		pw_log_debug("%p: defer async %d for object %p id:%d",
				queue, item->seq, obj, item->id);
	} else if (res == -EBUSY) {
		pw_log_debug("%p: wait sync object %p id:%u",
				queue, obj, item->id);
		item->seq = SPA_ID_INVALID;
		item->res = res;
		have_work = true;
	} else {
		item->seq = SPA_ID_INVALID;
		item->res = res;
		have_work = true;
		pw_log_debug("%p: defer object %p id:%u", queue, obj, item->id);
	}
	spa_list_append(&queue->work_list, &item->link);
	queue->n_queued++;

	if (have_work)
		pw_loop_signal_event(queue->loop, queue->wakeup);

	return item->id;
}

/** Cancel a work item
 * \param queue the work queue
 * \param obj the owner object
 * \param id the work id to cancel
 *
 */
SPA_EXPORT
int pw_work_queue_cancel(struct pw_work_queue *queue, void *obj, uint32_t id)
{
	bool have_work = false;
	struct work_item *item;

	spa_list_for_each(item, &queue->work_list, link) {
		if ((id == SPA_ID_INVALID || item->id == id) && (obj == NULL || item->obj == obj)) {
			pw_log_debug("%p: cancel defer %d for object %p id:%u", queue,
				     item->seq, item->obj, id);
			item->seq = SPA_ID_INVALID;
			item->func = NULL;
			have_work = true;
		}
	}
	if (!have_work) {
		pw_log_debug("%p: no deferred found for object %p id:%u", queue, obj, id);
		return -EINVAL;
	}

	pw_loop_signal_event(queue->loop, queue->wakeup);
	return 0;
}

/** Complete a work item
 * \param queue the work queue
 * \param obj the owner object
 * \param seq the sequence number that completed
 * \param res 0 if the item was found, < 0 on error
 *
 */
SPA_EXPORT
int pw_work_queue_complete(struct pw_work_queue *queue, void *obj, uint32_t seq, int res)
{
	struct work_item *item;
	bool have_work = false;

	spa_list_for_each(item, &queue->work_list, link) {
		if (item->obj == obj && item->seq == seq) {
			pw_log_debug("%p: found deferred %d for object %p res:%d id:%u",
					queue, seq, obj, res, item->id);
			item->seq = SPA_ID_INVALID;
			item->res = res;
			have_work = true;
		}
	}
	if (!have_work) {
		pw_log_trace("%p: no deferred %d found for object %p", queue, seq, obj);
		return -EINVAL;
	}

	pw_loop_signal_event(queue->loop, queue->wakeup);
	return 0;
}
