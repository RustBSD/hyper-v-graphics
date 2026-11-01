/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The RustBSD Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Implementation of the Hyper-V synthetic video protocol.
 *
 * All messages are exchanged as vmbus in-band packets.  A message is
 * prefixed by a pipe header, followed by the message header and the
 * message payload.  Requests which expect an answer are answered by the
 * host with a message of the corresponding *_RESPONSE / *_ACK type; the
 * receive task in hypervgfx.c stashes those away and wakes up whoever is
 * waiting for them.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <dev/hyperv/include/hyperv.h>
#include <dev/hyperv/include/vmbus.h>

#include "hypervgfx.h"

static int
hypervgfx_sendmsg(struct hypervgfx_softc *sc, struct hypervgfx_msg *msg,
    size_t len)
{
	int error;

	msg->pipe_hdr.type = HYPERVGFX_PIPE_MSG_DATA;
	msg->pipe_hdr.size = msg->vid_hdr.size;

	mtx_lock(&sc->lock);
	sc->xactid++;
	error = vmbus_chan_send(sc->chan, VMBUS_CHANPKT_TYPE_INBAND,
	    VMBUS_CHANPKT_FLAG_NONE, msg, len, sc->xactid);
	mtx_unlock(&sc->lock);
	if (error != 0)
		device_printf(sc->dev, "unable to send message: %d\n", error);

	return (error);
}

/*
 * Wait for the host to answer the request we just sent.  The receive
 * task copies the answer into sc->respbuf and wakes us up.
 */
static int
hypervgfx_wait_response(struct hypervgfx_softc *sc, int timeout)
{
	int error;

	mtx_lock(&sc->lock);
	while (!sc->resp_ready) {
		error = mtx_sleep(sc, &sc->lock, 0, "hypervgfx", timeout);
		if (error != 0)
			break;
	}
	sc->resp_ready = false;
	mtx_unlock(&sc->lock);

	return (error);
}

/*
 * Negotiate the protocol version with the host.
 */
int
hypervgfx_negotiate_version(struct hypervgfx_softc *sc, uint32_t version)
{
	struct hypervgfx_msg *msg;
	int error;

	msg = malloc(sizeof(*msg), M_DEVBUF, M_WAITOK | M_ZERO);
	if (msg == NULL)
		return (ENOMEM);

	msg->vid_hdr.type = HYPERVGFX_MSG_VERSION_REQUEST;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_version_req);
	msg->ver_req.version = version;

	mtx_lock(&sc->lock);
	sc->resp_expect = HYPERVGFX_MSG_VERSION_RESPONSE;
	mtx_unlock(&sc->lock);

	error = hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));
	if (error != 0)
		goto out;

	error = hypervgfx_wait_response(sc, 10 * hz);
	if (error != 0) {
		device_printf(sc->dev, "timeout waiting for version response\n");
		goto out;
	}

	mtx_lock(&sc->lock);
	memcpy(msg, sc->respbuf, sizeof(*msg));
	mtx_unlock(&sc->lock);

	if (msg->ver_resp.is_accepted == 0) {
		device_printf(sc->dev, "host rejected protocol version %u.%u\n",
		    HYPERVGFX_VERSION_MAJOR(version),
		    HYPERVGFX_VERSION_MINOR(version));
		error = ENOTTY;
		goto out;
	}

	sc->synthvid_version = msg->ver_resp.version;
	device_printf(sc->dev, "synthetic video protocol version %u.%u "
	    "accepted, max outputs %u\n",
	    HYPERVGFX_VERSION_MAJOR(sc->synthvid_version),
	    HYPERVGFX_VERSION_MINOR(sc->synthvid_version),
	    msg->ver_resp.max_video_outputs);
	error = 0;

out:
	free(msg, M_DEVBUF);
	return (error);
}

/*
 * Tell the host where our framebuffer lives.  The framebuffer has to be
 * backed by guest physical memory the host is able to map, so we pass the
 * physical address of the buffer.
 */
int
hypervgfx_update_vram_location(struct hypervgfx_softc *sc, uint64_t gpa)
{
	struct hypervgfx_msg *msg;
	int error;

	msg = malloc(sizeof(*msg), M_DEVBUF, M_WAITOK | M_ZERO);
	if (msg == NULL)
		return (ENOMEM);

	msg->vid_hdr.type = HYPERVGFX_MSG_VRAM_LOCATION;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_vram_location);
	msg->vram.user_ctx = gpa;
	msg->vram.vram_gpa = gpa;
	msg->vram.is_vram_gpa_specified = 1;

	mtx_lock(&sc->lock);
	sc->resp_expect = HYPERVGFX_MSG_VRAM_LOCATION_ACK;
	mtx_unlock(&sc->lock);

	error = hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));
	if (error != 0)
		goto out;

	error = hypervgfx_wait_response(sc, 10 * hz);
	if (error != 0) {
		device_printf(sc->dev, "timeout waiting for vram ack\n");
		goto out;
	}

	mtx_lock(&sc->lock);
	memcpy(msg, sc->respbuf, sizeof(*msg));
	mtx_unlock(&sc->lock);

	if (msg->vram_ack.user_ctx != gpa) {
		device_printf(sc->dev, "host did not accept vram location\n");
		error = EINVAL;
		goto out;
	}

	device_printf(sc->dev, "host accepted vram location at 0x%016jx\n",
	    (uintmax_t)gpa);
	error = 0;

out:
	free(msg, M_DEVBUF);
	return (error);
}

/*
 * Report the current display situation (mode) to the host.
 */
int
hypervgfx_update_situation(struct hypervgfx_softc *sc, uint8_t active,
    uint32_t bpp, uint32_t width, uint32_t height, uint32_t pitch)
{
	struct hypervgfx_msg *msg;
	int error;

	msg = malloc(sizeof(*msg), M_DEVBUF, M_WAITOK | M_ZERO);
	if (msg == NULL)
		return (ENOMEM);

	msg->vid_hdr.type = HYPERVGFX_MSG_SITUATION_UPDATE;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_situation_update);
	msg->situ.user_ctx = 0;
	msg->situ.video_output_count = 1;
	msg->situ.video_output[0].active = active;
	msg->situ.video_output[0].vram_offset = 0;
	msg->situ.video_output[0].depth_bits = bpp;
	msg->situ.video_output[0].width_pixels = width;
	msg->situ.video_output[0].height_pixels = height;
	msg->situ.video_output[0].pitch_bytes = pitch;

	error = hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));

	free(msg, M_DEVBUF);
	return (error);
}

/*
 * Ask the host which resolutions it supports.
 */
int
hypervgfx_get_supported_resolution(struct hypervgfx_softc *sc)
{
	struct hypervgfx_msg *msg;
	uint8_t index;
	int error, i;

	msg = malloc(sizeof(*msg), M_DEVBUF, M_WAITOK | M_ZERO);
	if (msg == NULL)
		return (ENOMEM);

	msg->vid_hdr.type = HYPERVGFX_MSG_RESOLUTION_REQUEST;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_supported_resolution_req);
	msg->resolution_req.maximum_resolution_count =
	    HYPERVGFX_MAX_RESOLUTION_COUNT;

	mtx_lock(&sc->lock);
	sc->resp_expect = HYPERVGFX_MSG_RESOLUTION_RESPONSE;
	mtx_unlock(&sc->lock);

	error = hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));
	if (error != 0)
		goto out;

	error = hypervgfx_wait_response(sc, 10 * hz);
	if (error != 0) {
		device_printf(sc->dev,
		    "timeout waiting for resolution response\n");
		goto out;
	}

	mtx_lock(&sc->lock);
	memcpy(msg, sc->respbuf, sizeof(*msg));
	mtx_unlock(&sc->lock);

	if (msg->resolution_resp.resolution_count == 0) {
		device_printf(sc->dev, "host reports no supported resolution\n");
		error = ENODEV;
		goto out;
	}

	index = msg->resolution_resp.default_resolution_index;
	if (index >= msg->resolution_resp.resolution_count) {
		device_printf(sc->dev, "host reported invalid default "
		    "resolution index %u\n", index);
		error = EINVAL;
		goto out;
	}

	sc->screen_width_max = 0;
	sc->screen_height_max = 0;
	for (i = 0; i < msg->resolution_resp.resolution_count; i++) {
		uint16_t w = msg->resolution_resp.supported_resolution[i].width;
		uint16_t h = msg->resolution_resp.supported_resolution[i].height;

		if (i < 64) {
			sc->modes[i].w = w;
			sc->modes[i].h = h;
		}
		if (w > sc->screen_width_max)
			sc->screen_width_max = w;
		if (h > sc->screen_height_max)
			sc->screen_height_max = h;
	}
	sc->nmodes = min(msg->resolution_resp.resolution_count, 64u);
	sc->is_standard = msg->resolution_resp.is_standard != 0;

	for (i = 0; i < sc->nmodes; i++)
		device_printf(sc->dev, "host mode %u: %ux%u%s\n", i,
		    sc->modes[i].w, sc->modes[i].h,
		    i == index ? " (default)" : "");

	sc->preferred_width =
	    msg->resolution_resp.supported_resolution[index].width;
	sc->preferred_height =
	    msg->resolution_resp.supported_resolution[index].height;

	device_printf(sc->dev, "host supports up to %ux%u, default %ux%u "
	    "(%u modes)\n", sc->screen_width_max, sc->screen_height_max,
	    sc->preferred_width, sc->preferred_height,
	    msg->resolution_resp.resolution_count);
	error = 0;

out:
	free(msg, M_DEVBUF);
	return (error);
}

/*
 * Switch the display mode.  The mode must fit into the framebuffer
 * window the hypervisor gave us; the host reacts to the SITUATION_UPDATE
 * by reconfiguring the virtual monitor.
 */
int
hypervgfx_set_mode(struct hypervgfx_softc *sc, uint32_t width,
    uint32_t height)
{
	struct hypervgfx_rect full;
	int i, error;

	if (width == 0 || height == 0 || width > 4096 || height > 4096)
		return (EINVAL);

	/* The mode should be one the host advertised. */
	for (i = 0; i < sc->nmodes; i++) {
		if (sc->modes[i].w == width && sc->modes[i].h == height)
			break;
	}
	if (i == sc->nmodes)
		device_printf(sc->dev, "warning: %ux%u is not in the host "
		    "mode list, trying anyway\n", width, height);

	if ((size_t)width * height * 4 > sc->fb_size)
		return (EFBIG);

	sc->fb_width = width;
	sc->fb_height = height;
	sc->fb_stride = width * 4;

	/* Keep the fbdev view of the buffer in sync. */
	sc->fbi.fb_width = width;
	sc->fbi.fb_height = height;
	sc->fbi.fb_stride = sc->fb_stride;

	error = hypervgfx_update_situation(sc, 1, sc->fb_bpp, width, height,
	    sc->fb_stride);
	if (error != 0)
		return (error);

	full.x1 = 0;
	full.y1 = 0;
	full.x2 = width;
	full.y2 = height;

	device_printf(sc->dev, "switched to %ux%u\n", width, height);
	return (hypervgfx_update_dirt(sc, &full));
}

/*
 * The hypervisor draws a hardware cursor of its own.  Point it at a
 * single transparent pixel so that it does not show up on top of the
 * guest cursor.
 */
void
hypervgfx_hide_hw_ptr(struct hypervgfx_softc *sc)
{
	struct hypervgfx_msg *msg;

	msg = malloc(sizeof(*msg), M_DEVBUF, M_WAITOK | M_ZERO);
	if (msg == NULL)
		return;

	msg->vid_hdr.type = HYPERVGFX_MSG_POINTER_POSITION;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_pointer_position);
	msg->ptr_pos.is_visible = 1;
	msg->ptr_pos.video_output = 0;
	msg->ptr_pos.image_x = 0;
	msg->ptr_pos.image_y = 0;
	hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));

	memset(msg, 0, sizeof(*msg));
	msg->vid_hdr.type = HYPERVGFX_MSG_POINTER_SHAPE;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_pointer_shape);
	msg->ptr_shape.part_idx = HYPERVGFX_CURSOR_COMPLETE;
	msg->ptr_shape.is_argb = 1;
	msg->ptr_shape.width = 1;
	msg->ptr_shape.height = 1;
	msg->ptr_shape.hot_x = 0;
	msg->ptr_shape.hot_y = 0;
	msg->ptr_shape.data[0] = 0;
	msg->ptr_shape.data[1] = 1;
	msg->ptr_shape.data[2] = 1;
	msg->ptr_shape.data[3] = 1;
	hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));

	free(msg, M_DEVBUF);
}

/*
 * Tell the host which part of the framebuffer has been damaged.
 */
int
hypervgfx_update_dirt(struct hypervgfx_softc *sc,
    const struct hypervgfx_rect *rect)
{
	struct hypervgfx_msg *msg;
	int error;

	if (!sc->dirt_needed)
		return (0);

	msg = malloc(sizeof(*msg), M_DEVBUF, M_WAITOK | M_ZERO);
	if (msg == NULL)
		return (ENOMEM);

	msg->vid_hdr.type = HYPERVGFX_MSG_DIRT;
	msg->vid_hdr.size = sizeof(struct hypervgfx_msg_hdr) +
	    sizeof(struct hypervgfx_dirt);
	msg->dirt.video_output = 0;
	msg->dirt.dirt_count = 1;
	msg->dirt.rect[0].x1 = rect->x1;
	msg->dirt.rect[0].y1 = rect->y1;
	msg->dirt.rect[0].x2 = rect->x2;
	msg->dirt.rect[0].y2 = rect->y2;

	error = hypervgfx_sendmsg(sc, msg, msg->vid_hdr.size +
	    sizeof(struct hypervgfx_pipe_msg_hdr));

	free(msg, M_DEVBUF);
	return (error);
}
