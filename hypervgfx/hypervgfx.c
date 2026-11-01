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
 * Driver for the Hyper-V synthetic video device.
 *
 * The device shows up as a vmbus channel with the HV_SYNTHVID_GUID type
 * GUID.  This driver speaks the synthetic video protocol on top of it and
 * manages the framebuffer the hypervisor renders from.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/time.h>
#include <sys/types.h>

#include <machine/metadata.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/hyperv/include/hyperv.h>
#include <dev/hyperv/include/vmbus.h>

#include "hypervgfx.h"

#define HYPERVGFX_RING_SIZE	(256 * 1024)
#define HYPERVGFX_PACKET_SIZE	(16 * 1024)

static void hypervgfx_read_channel(struct hypervgfx_softc *);
static void hypervgfx_chan_callback(struct vmbus_channel *, void *);
static int hypervgfx_probe(device_t);
static int hypervgfx_attach(device_t);
static int hypervgfx_detach(device_t);

/*
 * There is at most one synthetic video device per guest; keep a global
 * pointer so that the sysctl handlers can find it.
 */
static struct hypervgfx_softc *hypervgfx_softc;

/*
 * Is this the synthetic video device?
 */
static bool
hypervgfx_is_synthvid(struct vmbus_channel *chan)
{
	static const uint8_t guid[16] = { HYPERVGFX_SYNTHVID_GUID_BYTES };
	struct hyperv_guid *type;

	type = vmbus_chan_guid_type(chan);
	if (type == NULL)
		return (false);

	return (memcmp(type->hv_guid, guid, sizeof(guid)) == 0);
}

static int
hypervgfx_probe(device_t dev)
{
	struct vmbus_channel *chan;

	chan = vmbus_get_channel(dev);
	if (chan == NULL || !hypervgfx_is_synthvid(chan))
		return (ENXIO);

	device_set_desc(dev, "Hyper-V Synthetic Video");
	return (BUS_PROBE_DEFAULT);
}

/*
 * Handle a single message received from the host.
 */
static void
hypervgfx_process_msg(struct hypervgfx_softc *sc,
    const struct hypervgfx_msg *msg)
{

	switch (msg->vid_hdr.type) {
	case HYPERVGFX_MSG_VERSION_RESPONSE:
	case HYPERVGFX_MSG_VRAM_LOCATION_ACK:
	case HYPERVGFX_MSG_RESOLUTION_RESPONSE:
	case HYPERVGFX_MSG_SITUATION_UPDATE_ACK:
		mtx_lock(&sc->lock);
		if (sc->resp_expect == msg->vid_hdr.type) {
			memcpy(sc->respbuf, msg, sizeof(*msg));
			sc->resp_ready = true;
			wakeup(sc);
		}
		mtx_unlock(&sc->lock);
		break;
	case HYPERVGFX_MSG_FEATURE_CHANGE:
		sc->dirt_needed = msg->feature_chg.is_dirt_needed != 0;
		if (sc->dirt_needed)
			hypervgfx_hide_hw_ptr(sc);
		break;
	case HYPERVGFX_MSG_SITUATION_UPDATE:
		/*
		 * The host tells us about the resolution it is showing.
		 */
		if (msg->situ.video_output_count > 0) {
			const struct hypervgfx_video_output_situation *vo;

			vo = &msg->situ.video_output[0];
			device_printf(sc->dev, "host situation: %ux%u, "
			    "%u bpp, pitch %u\n", vo->width_pixels,
			    vo->height_pixels, vo->depth_bits,
			    vo->pitch_bytes);
		}
		break;
	default:
		device_printf(sc->dev, "unhandled message type %u\n",
		    msg->vid_hdr.type);
		break;
	}
}

/*
 * Drain everything the host has sent us.
 */
static void
hypervgfx_read_channel(struct hypervgfx_softc *sc)
{
	struct vmbus_channel *chan = sc->chan;
	struct vmbus_chanpkt_hdr *pkt;
	const struct hypervgfx_msg *msg;
	int error, pktlen, hlen;

	for (;;) {
		pkt = (struct vmbus_chanpkt_hdr *)sc->rxbuf;
		pktlen = sc->rxbuflen;
		error = vmbus_chan_recv_pkt(chan, pkt, &pktlen);
		if (error == EAGAIN || error == ENOENT)
			break;			/* no more packets */
		if (error == ENOBUFS) {
			device_printf(sc->dev, "receive buffer too small\n");
			break;
		}
		if (error != 0) {
			device_printf(sc->dev, "channel receive failed: %d\n",
			    error);
			break;
		}

		if (pkt->cph_type != VMBUS_CHANPKT_TYPE_INBAND)
			continue;

		hlen = VMBUS_CHANPKT_GETLEN(pkt->cph_hlen);
		if (pktlen < hlen + (int)sizeof(struct hypervgfx_pipe_msg_hdr))
			continue;

		msg = (const struct hypervgfx_msg *)((const uint8_t *)pkt + hlen);
		if (msg->pipe_hdr.type != HYPERVGFX_PIPE_MSG_DATA)
			continue;

		hypervgfx_process_msg(sc, msg);
	}
}

static void
hypervgfx_chan_callback(struct vmbus_channel *chan __unused, void *arg)
{
	struct hypervgfx_softc *sc = arg;

	hypervgfx_read_channel(sc);
}

static int
hypervgfx_attach(device_t dev)
{
	struct hypervgfx_softc *sc;
	struct vmbus_channel *chan;
	int error;

	chan = vmbus_get_channel(dev);
	if (chan == NULL)
		return (ENXIO);

	if (!hypervgfx_is_synthvid(chan))
		return (ENXIO);

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->chan = chan;
	mtx_init(&sc->lock, "hypervgfx", "hypervgfx lock", MTX_DEF);

	sc->rxbuflen = HYPERVGFX_PACKET_SIZE;
	sc->rxbuf = malloc(sc->rxbuflen, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->respbuf = malloc(sizeof(struct hypervgfx_msg), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	if (sc->rxbuf == NULL || sc->respbuf == NULL) {
		error = ENOMEM;
		goto fail;
	}

	device_set_desc(dev, "Hyper-V Synthetic Video");

	/*
	 * Messages are processed in a task, not in the interrupt handler,
	 * so that we may sleep and take locks.
	 */
	vmbus_chan_set_readbatch(chan, false);

	error = vmbus_chan_open(chan, HYPERVGFX_RING_SIZE, HYPERVGFX_RING_SIZE,
	    NULL, 0, hypervgfx_chan_callback, sc);
	if (error != 0) {
		device_printf(dev, "unable to open channel: %d\n", error);
		goto fail;
	}

	/*
	 * Negotiate the protocol version.  Windows 10 and later speak
	 * version 3.5 which adds the resolution negotiation.
	 */
	error = hypervgfx_negotiate_version(sc, HYPERVGFX_VERSION_WIN10);
	if (error != 0) {
		device_printf(dev, "unable to negotiate protocol version\n");
		goto fail_chan;
	}

	sc->screen_depth = HYPERVGFX_DEPTH_WIN8;

	if (hypervgfx_get_supported_resolution(sc) != 0) {
		device_printf(dev, "host did not report any resolution, "
		    "assuming %ux%u\n", HYPERVGFX_WIDTH_WIN8,
		    HYPERVGFX_HEIGHT_WIN8);
		sc->screen_width_max = HYPERVGFX_WIDTH_WIN8;
		sc->screen_height_max = HYPERVGFX_HEIGHT_WIN8;
		sc->preferred_width = HYPERVGFX_WIDTH_WIN8;
		sc->preferred_height = HYPERVGFX_HEIGHT_WIN8;
	}

	/*
	 * Set up the framebuffer and tell the host about it.
	 */
	error = hypervgfx_fb_attach(sc);
	if (error != 0)
		goto fail_chan;

	error = hypervgfx_update_vram_location(sc, sc->fb_pbase);
	if (error != 0)
		goto fail_fb;

	error = hypervgfx_update_situation(sc, 1, sc->fb_bpp, sc->fb_width,
	    sc->fb_height, sc->fb_stride);
	if (error != 0)
		goto fail_fb;

	hypervgfx_hide_hw_ptr(sc);

	device_printf(dev, "attached: framebuffer %ux%u at 0x%016jx\n",
	    sc->fb_width, sc->fb_height, (uintmax_t)sc->fb_pbase);

	hypervgfx_softc = sc;

	return (0);

fail_fb:
	hypervgfx_fb_detach(sc);
fail_chan:
	vmbus_chan_close(chan);
fail:
	if (sc->rxbuf != NULL)
		free(sc->rxbuf, M_DEVBUF);
	if (sc->respbuf != NULL)
		free(sc->respbuf, M_DEVBUF);
	mtx_destroy(&sc->lock);
	return (error);
}

static int
hypervgfx_detach(device_t dev)
{
	struct hypervgfx_softc *sc = device_get_softc(dev);

	if (hypervgfx_softc == sc)
		hypervgfx_softc = NULL;

	vmbus_chan_close(sc->chan);
	hypervgfx_fb_detach(sc);

	free(sc->rxbuf, M_DEVBUF);
	free(sc->respbuf, M_DEVBUF);
	mtx_destroy(&sc->lock);

	return (0);
}

static int
hypervgfx_status_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct hypervgfx_softc *sc = hypervgfx_softc;
	char buf[512];

	if (sc == NULL)
		return (ENODEV);

	snprintf(buf, sizeof(buf),
	    "protocol version: %u.%u\n"
	    "screen depth: %u\n"
	    "max resolution: %ux%u\n"
	    "preferred resolution: %ux%u\n"
	    "framebuffer: %ux%u, stride %u, %zu bytes at 0x%016jx\n"
	    "dirt needed: %s\n",
	    HYPERVGFX_VERSION_MAJOR(sc->synthvid_version),
	    HYPERVGFX_VERSION_MINOR(sc->synthvid_version),
	    sc->screen_depth,
	    sc->screen_width_max, sc->screen_height_max,
	    sc->preferred_width, sc->preferred_height,
	    sc->fb_width, sc->fb_height, sc->fb_stride, sc->fb_size,
	    (uintmax_t)sc->fb_pbase,
	    sc->dirt_needed ? "yes" : "no");

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

static int
hypervgfx_test_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct hypervgfx_softc *sc = hypervgfx_softc;
	volatile uint32_t *line;
	uint32_t color;
	struct hypervgfx_rect full;
	int i, j, error;

	if (sc == NULL || sc->fb_vbase == NULL)
		return (ENODEV);

	for (i = 0; i < (int)sc->fb_height; i++) {
		line = (volatile uint32_t *)((uintptr_t)sc->fb_vbase +
		    (size_t)i * sc->fb_stride);
		color = (i % 2) ? 0x00ff00ff : 0x0000ffff;
		for (j = 0; j < (int)sc->fb_width; j++)
			line[j] = color;
	}

	/* Tell the host the whole screen has changed. */
	full.x1 = 0;
	full.y1 = 0;
	full.x2 = sc->fb_width;
	full.y2 = sc->fb_height;
	error = hypervgfx_update_dirt(sc, &full);

	return (error);
}

SYSCTL_NODE(_hw, OID_AUTO, hypervgfx, CTLFLAG_RW, 0,
    "Hyper-V synthetic video");
SYSCTL_PROC(_hw_hypervgfx, OID_AUTO, status,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    hypervgfx_status_sysctl, "A", "Hyper-V synthetic video status");
SYSCTL_PROC(_hw_hypervgfx, OID_AUTO, test_pattern,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    hypervgfx_test_sysctl, "A",
    "Fill the framebuffer with a test pattern");

static device_method_t hypervgfx_methods[] = {
	DEVMETHOD(device_probe,		hypervgfx_probe),
	DEVMETHOD(device_attach,	hypervgfx_attach),
	DEVMETHOD(device_detach,	hypervgfx_detach),
	{ 0, 0 }
};

static driver_t hypervgfx_driver = {
	"hypervgfx",
	hypervgfx_methods,
	sizeof(struct hypervgfx_softc)
};

DRIVER_MODULE(hypervgfx, vmbus, hypervgfx_driver, NULL, NULL);
MODULE_DEPEND(hypervgfx, vmbus, 1, 1, 1);
