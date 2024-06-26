/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sys/resource.h>

#include "pipewire/log.h"
#include "pipewire/data-loop.h"
#include "pipewire/private.h"
#include "pipewire/thread.h"
#include "pipewire/utils.h"

PW_LOG_TOPIC_EXTERN(log_data_loop);
#define PW_LOG_TOPIC_DEFAULT log_data_loop

SPA_EXPORT
int pw_data_loop_wait(struct pw_data_loop *this, int timeout)
{
	int res;

	while (true) {
		if (SPA_UNLIKELY(!this->running)) {
			res = -ECANCELED;
			break;
		}
		if (SPA_UNLIKELY((res = pw_loop_iterate(this->loop, timeout)) < 0)) {
			if (res == -EINTR)
				continue;
		}
		break;
	}
	return res;
}

SPA_EXPORT
void pw_data_loop_exit(struct pw_data_loop *this)
{
	this->running = false;
}

static void thread_cleanup(void *arg)
{
	struct pw_data_loop *this = arg;
	pw_log_debug("%p: leave thread", this);
	this->running = false;
	pw_loop_leave(this->loop);
}

static void *do_loop(void *user_data)
{
	struct pw_data_loop *this = user_data;
	int res;
	struct spa_callbacks *cb = &this->loop->control->iface.cb;
	const struct spa_loop_control_methods *m = cb->funcs;
	void *data = cb->data;
	int (*iterate) (void *object, int timeout) = m->iterate;

	pw_log_debug("%p: enter thread", this);
	pw_loop_enter(this->loop);

	pthread_cleanup_push(thread_cleanup, this);

	while (SPA_LIKELY(this->running)) {
		if (SPA_UNLIKELY((res = iterate(data, -1)) < 0)) {
			if (res == -EINTR)
				continue;
			pw_log_error("%p: iterate error %d (%s)",
					this, res, spa_strerror(res));
		}
	}
	pthread_cleanup_pop(1);

	return NULL;
}

static int do_stop(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_data_loop *this = user_data;
	pw_log_debug("%p: stopping", this);
	this->running = false;
	return 0;
}

static struct pw_data_loop *loop_new(struct pw_loop *loop, const struct spa_dict *props)
{
	struct pw_data_loop *this;
	const char *str, *name = NULL, *class = NULL;
	int res;

	this = calloc(1, sizeof(struct pw_data_loop));
	if (this == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	pw_log_debug("%p: new", this);

	if (loop == NULL) {
		loop = pw_loop_new(props);
		this->created = true;
	}
	if (loop == NULL) {
		res = -errno;
		pw_log_error("%p: can't create loop: %m", this);
		goto error_free;
	}
	this->loop = loop;
	this->rt_prio = -1;

	if (props != NULL) {
		if ((str = spa_dict_lookup(props, PW_KEY_LOOP_CANCEL)) != NULL)
			this->cancel = pw_properties_parse_bool(str);
		if ((str = spa_dict_lookup(props, PW_KEY_LOOP_CLASS)) != NULL)
			class = str;
		if ((str = spa_dict_lookup(props, PW_KEY_LOOP_RT_PRIO)) != NULL)
			this->rt_prio = atoi(str);
		if ((str = spa_dict_lookup(props, SPA_KEY_THREAD_NAME)) != NULL)
			name = str;
		if ((str = spa_dict_lookup(props, SPA_KEY_THREAD_AFFINITY)) != NULL)
			this->affinity = strdup(str);
	}
	if (class == NULL)
		class = this->rt_prio != 0 ? "data.rt" : "data";
	if (name == NULL)
		name = "data-loop";

	this->class = strdup(class);
	this->classes = pw_strv_parse(class, strlen(class), INT_MAX, NULL);
	if (!this->loop->name[0])
		pw_loop_set_name(this->loop, name);
	spa_hook_list_init(&this->listener_list);

	return this;

error_free:
	free(this);
error_cleanup:
	errno = -res;
	return NULL;
}

/** Create a new \ref pw_data_loop.
 * \return a newly allocated data loop
 *
 */
SPA_EXPORT
struct pw_data_loop *pw_data_loop_new(const struct spa_dict *props)
{
	return loop_new(NULL, props);
}


/** Destroy a data loop
 * \param loop the data loop to destroy
 */
SPA_EXPORT
void pw_data_loop_destroy(struct pw_data_loop *loop)
{
	pw_log_debug("%p: destroy", loop);

	pw_data_loop_emit_destroy(loop);

	pw_data_loop_stop(loop);

	if (loop->created)
		pw_loop_destroy(loop->loop);

	spa_hook_list_clean(&loop->listener_list);

	free(loop->affinity);
	free(loop->class);
	pw_free_strv(loop->classes);
	free(loop);
}

SPA_EXPORT
void pw_data_loop_add_listener(struct pw_data_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_data_loop_events *events,
			       void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SPA_EXPORT
struct pw_loop *
pw_data_loop_get_loop(struct pw_data_loop *loop)
{
	return loop->loop;
}

/** Get the loop name
 * \param loop the data loop to query
 * \return the data loop name
 *
 * Get the name of the data loop. The data loop name is a unique name that
 * identifies this data loop.
 *
 * \since 1.1.0
 */
SPA_EXPORT
const char * pw_data_loop_get_name(struct pw_data_loop *loop)
{
	return loop->loop->name;
}

/** Get the loop class
 * \param loop the data loop to query
 * \return the data loop class
 *
 * Get the class of the data loop. Multiple data loop can have the same class
 * and processing can be assigned to any data loop from the same class.
 *
 * \since 1.1.0
 */
SPA_EXPORT
const char * pw_data_loop_get_class(struct pw_data_loop *loop)
{
	return loop->class;
}

/** Start a data loop
 * \param loop the data loop to start
 * \return 0 if ok, -1 on error
 *
 * This will start the realtime thread that manages the loop.
 *
 */
SPA_EXPORT
int pw_data_loop_start(struct pw_data_loop *loop)
{
	if (!loop->running) {
		struct spa_thread_utils *utils;
		struct spa_thread *thr;
		struct spa_dict_item items[2];
		uint32_t n_items = 0;

		loop->running = true;

		if ((utils = loop->thread_utils) == NULL)
			utils = pw_thread_utils_get();

		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_NAME, loop->loop->name);
		if (loop->affinity)
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_AFFINITY,
					loop->affinity);

		thr = spa_thread_utils_create(utils, &SPA_DICT_INIT(items, n_items), do_loop, loop);
		loop->thread = (pthread_t)thr;
		if (thr == NULL) {
			pw_log_error("%p: can't create thread: %m", loop);
			loop->running = false;
			return -errno;
		}
		if (loop->rt_prio != 0)
			spa_thread_utils_acquire_rt(utils, thr, loop->rt_prio);
	}
	return 0;
}

/** Stop a data loop
 * \param loop the data loop to Stop
 * \return 0
 *
 * This will stop and join the realtime thread that manages the loop.
 *
 */
SPA_EXPORT
int pw_data_loop_stop(struct pw_data_loop *loop)
{
	pw_log_debug("%p stopping", loop);
	if (loop->running) {
		struct spa_thread_utils *utils;
		if (loop->cancel) {
			pw_log_debug("%p cancel", loop);
			pthread_cancel(loop->thread);
		} else {
			pw_log_debug("%p signal", loop);
			pw_loop_invoke(loop->loop, do_stop, 1, NULL, 0, false, loop);
		}
		pw_log_debug("%p join", loop);
		if ((utils = loop->thread_utils) == NULL)
			utils = pw_thread_utils_get();
		spa_thread_utils_join(utils, (struct spa_thread*)loop->thread, NULL);
		pw_log_debug("%p joined", loop);
	}
	pw_log_debug("%p stopped", loop);
	return 0;
}

/** Check if we are inside the data loop
 * \param loop the data loop to check
 * \return true is the current thread is the data loop thread
 *
 */
SPA_EXPORT
bool pw_data_loop_in_thread(struct pw_data_loop * loop)
{
	return loop->running && pthread_equal(loop->thread, pthread_self());
}

/** Get the thread object.
 * \param loop the data loop to get the thread of
 * \return the thread object or NULL when the thread is not running
 *
 * On posix based systems this returns a pthread_t
 */
SPA_EXPORT
struct spa_thread *pw_data_loop_get_thread(struct pw_data_loop * loop)
{
	return loop->running ? (struct spa_thread*)loop->thread : NULL;
}

SPA_EXPORT
int pw_data_loop_invoke(struct pw_data_loop *loop,
		spa_invoke_func_t func, uint32_t seq, const void *data, size_t size,
		bool block, void *user_data)
{
	return pw_loop_invoke(loop->loop, func, seq, data, size, block, user_data);
}

/** Set a thread utils implementation.
 * \param loop the data loop to set the thread utils on
 * \param impl the thread utils implementation
 *
 * This configures a custom spa_thread_utils implementation for this data
 * loop. Use NULL to restore the system default implementation.
 */
SPA_EXPORT
void pw_data_loop_set_thread_utils(struct pw_data_loop *loop,
		struct spa_thread_utils *impl)
{
	loop->thread_utils = impl;
}
