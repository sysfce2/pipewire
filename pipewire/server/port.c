/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "pipewire/client/pipewire.h"

#include "pipewire/server/port.h"

/** \cond */
struct impl {
	struct pw_port this;
};
/** \endcond */


static void port_update_state(struct pw_port *port, enum pw_port_state state)
{
	if (port->state != state) {
		pw_log_debug("port %p: state %d -> %d", port, port->state, state);
		port->state = state;
		pw_signal_emit(&port->state_changed, port);
	}
}

static int schedule_tee_input(struct spa_graph_node *node, void *user_data)
{
        int res;
        struct pw_port *this = user_data;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	if (spa_list_is_empty(&node->ports[SPA_DIRECTION_OUTPUT])) {
		io->status = SPA_RESULT_NEED_BUFFER;
		res = SPA_RESULT_NEED_BUFFER;
	}
	else {
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
			*p->io = *io;
		io->status = SPA_RESULT_OK;
		io->buffer_id = SPA_ID_INVALID;
		res = SPA_RESULT_HAVE_BUFFER;
	}
        return res;
}
static int schedule_tee_output(struct spa_graph_node *node, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
		*io = *p->io;
	io->status = SPA_RESULT_NEED_BUFFER;

	return SPA_RESULT_NEED_BUFFER;
}

static int schedule_tee_reuse_buffer(struct spa_graph_port *port, uint32_t buffer_id, void *user_data)
{
	return SPA_RESULT_OK;
}

static const struct spa_graph_node_methods schedule_tee = {
	SPA_VERSION_GRAPH_NODE_METHODS,
	schedule_tee_input,
	schedule_tee_output,
	schedule_tee_reuse_buffer,
};

static int schedule_mix_input(struct spa_graph_node *node, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		*io = *p->io;
		p->io->status = SPA_RESULT_OK;
		p->io->buffer_id = SPA_ID_INVALID;
	}
	return SPA_RESULT_HAVE_BUFFER;
}

static int schedule_mix_output(struct spa_graph_node *node, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	io->status = SPA_RESULT_NEED_BUFFER;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link)
		*p->io = *io;
	io->buffer_id = SPA_ID_INVALID;

	return SPA_RESULT_NEED_BUFFER;
}

static int schedule_mix_reuse_buffer(struct spa_graph_port *port, uint32_t buffer_id, void *user_data)
{
	return SPA_RESULT_OK;
}

static const struct spa_graph_node_methods schedule_mix = {
	SPA_VERSION_GRAPH_NODE_METHODS,
	schedule_mix_input,
	schedule_mix_output,
	schedule_mix_reuse_buffer,
};

struct pw_port *pw_port_new(enum pw_direction direction,
			    uint32_t port_id,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_port *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("port %p: new", this);

	this->direction = direction;
	this->port_id = port_id;
	this->state = PW_PORT_STATE_INIT;
	this->io.status = SPA_RESULT_OK;
	this->io.buffer_id = SPA_ID_INVALID;

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	spa_list_init(&this->links);

	pw_signal_init(&this->state_changed);
	pw_signal_init(&this->destroy_signal);

	spa_graph_port_init(&this->rt.port,
			    this->direction,
			    this->port_id,
			    0,
			    &this->io);
	spa_graph_node_init(&this->rt.mix_node,
			    this->direction == PW_DIRECTION_INPUT ?
				&schedule_mix :
				&schedule_tee,
			    this);
	spa_graph_port_init(&this->rt.mix_port,
			    pw_direction_reverse(this->direction),
			    0,
			    0,
			    &this->io);
	return this;
}

static int do_add_port(struct spa_loop *loop,
		       bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
        struct pw_port *this = user_data;

	spa_graph_port_add(&this->node->rt.node, &this->rt.port);
	spa_graph_node_add(this->rt.graph, &this->rt.mix_node);
	spa_graph_port_add(&this->rt.mix_node, &this->rt.mix_port);
	spa_graph_port_link(&this->rt.port, &this->rt.mix_port);

	return SPA_RESULT_OK;
}

void pw_port_add(struct pw_port *port, struct pw_node *node)
{
	port->node = node;

	pw_log_debug("port %p: add to node %p", port, node);
	if (port->direction == PW_DIRECTION_INPUT) {
		spa_list_insert(&node->input_ports, &port->link);
		pw_map_insert_at(&node->input_port_map, port->port_id, port);
		node->info.n_input_ports++;
		node->info.change_mask |= 1 << 1;
	}
	else {
		spa_list_insert(&node->output_ports, &port->link);
		pw_map_insert_at(&node->output_port_map, port->port_id, port);
		node->info.n_output_ports++;
		node->info.change_mask |= 1 << 3;
	}

	port->rt.graph = node->rt.sched->graph;
	pw_loop_invoke(node->data_loop->loop, do_add_port, SPA_ID_INVALID, 0, NULL, false, port);

	port_update_state(port, PW_PORT_STATE_CONFIGURE);

	pw_signal_emit(&node->port_added, node, port);
}

static int do_remove_port(struct spa_loop *loop,
			  bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;

	spa_graph_port_unlink(&this->rt.port);
	spa_graph_port_remove(&this->rt.port);

	spa_list_for_each(p, &this->rt.mix_node.ports[this->direction], link)
		spa_graph_port_remove(p);

	spa_graph_port_remove(&this->rt.mix_port);
	spa_graph_node_remove(&this->rt.mix_node);

	return SPA_RESULT_OK;
}

void pw_port_destroy(struct pw_port *port)
{
	struct pw_node *node = port->node;

	pw_log_debug("port %p: destroy", port);

	pw_signal_emit(&port->destroy_signal, port);

	if (node) {
		pw_loop_invoke(port->node->data_loop->loop, do_remove_port, SPA_ID_INVALID, 0, NULL, true, port);

		if (port->direction == PW_DIRECTION_INPUT) {
			pw_map_remove(&node->input_port_map, port->port_id);
			node->info.n_input_ports--;
		}
		else {
			pw_map_remove(&node->output_port_map, port->port_id);
			node->info.n_output_ports--;
		}
		spa_list_remove(&port->link);
		pw_signal_emit(&node->port_removed, node, port);
	}

	if (port->destroy)
		port->destroy(port);

	free(port);
}

static int
do_port_pause(struct spa_loop *loop,
              bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
        struct pw_port *port = user_data;

        return port->implementation->send_command(port,
		&SPA_COMMAND_INIT(port->node->core->type.command_node.Pause));
}

int pw_port_enum_formats(struct pw_port *port,
                         struct spa_format **format,
                         const struct spa_format *filter,
                         int32_t index)
{
	return port->implementation->enum_formats(port, format, filter, index);
}

int pw_port_set_format(struct pw_port *port, uint32_t flags, struct spa_format *format)
{
	int res;

	res = port->implementation->set_format(port, flags, format);
	pw_log_debug("port %p: set format %d", port, res);

	if (!SPA_RESULT_IS_ASYNC(res)) {
		if (format == NULL) {
			port->buffers = NULL;
			port->n_buffers = 0;
			if (port->allocated)
				pw_memblock_free(&port->buffer_mem);
			port->allocated = false;
			port_update_state (port, PW_PORT_STATE_CONFIGURE);
		}
		else {
			port_update_state (port, PW_PORT_STATE_READY);
		}
	}
	return res;
}

int pw_port_get_format(struct pw_port *port, const struct spa_format **format)
{
	return port->implementation->get_format(port, format);
}

int pw_port_get_info(struct pw_port *port, const struct spa_port_info **info)
{
	return port->implementation->get_info(port, info);
}

int pw_port_enum_params(struct pw_port *port, uint32_t index, struct spa_param **param)
{
	return port->implementation->enum_params(port, index, param);
}

int pw_port_set_param(struct pw_port *port, struct spa_param *param)
{
	return port->implementation->set_param(port, param);
}

int pw_port_use_buffers(struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers)
{
	int res;

	if (n_buffers == 0 && port->state <= PW_PORT_STATE_READY)
		return SPA_RESULT_OK;

	if (n_buffers > 0 && port->state < PW_PORT_STATE_READY)
		return SPA_RESULT_NO_FORMAT;

	if (port->state > PW_PORT_STATE_PAUSED) {
		pw_loop_invoke(port->node->data_loop->loop,
			       do_port_pause, 0, 0, NULL, true, port);
		port_update_state (port, PW_PORT_STATE_PAUSED);
	}

	pw_log_debug("port %p: use %d buffers", port, n_buffers);
	res = port->implementation->use_buffers(port, buffers, n_buffers);

	port->buffers = buffers;
	port->n_buffers = n_buffers;
	if (port->allocated)
		pw_memblock_free(&port->buffer_mem);
	port->allocated = false;

	if (port->n_buffers == 0)
		port_update_state (port, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}

int pw_port_alloc_buffers(struct pw_port *port,
			  struct spa_param **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers)
{
	int res;

	if (port->state < PW_PORT_STATE_READY)
		return SPA_RESULT_NO_FORMAT;

	if (port->state > PW_PORT_STATE_PAUSED) {
		pw_loop_invoke(port->node->data_loop->loop,
			       do_port_pause, 0, 0, NULL, true, port);
		port_update_state (port, PW_PORT_STATE_PAUSED);
	}

	pw_log_debug("port %p: alloc %d buffers", port, *n_buffers);
	res = port->implementation->alloc_buffers(port, params, n_params, buffers, n_buffers);

	port->buffers = buffers;
	port->n_buffers = *n_buffers;
	port->allocated = true;

	if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}
