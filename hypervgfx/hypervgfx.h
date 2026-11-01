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
 * Hyper-V Synthetic Video device (SYNTHVID) support for FreeBSD.
 *
 * The device is exposed by the hypervisor as a vmbus channel whose type
 * GUID is HV_SYNTHVID_GUID.  It speaks a small, message based protocol
 * (the "synthetic video" protocol) over the regular vmbus in-band
 * channel.  The framebuffer itself lives in a host provided MMIO window
 * which the hypervisor maps into the guest physical address space; the
 * guest tells the host where that window lives with a VRAM_LOCATION
 * message.
 */

#ifndef	_HYPERVGFX_H_
#define	_HYPERVGFX_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/fbio.h>
#include <sys/mutex.h>
#include <sys/taskqueue.h>
#include <sys/types.h>

#include <dev/hyperv/include/hyperv.h>
#include <dev/hyperv/include/vmbus.h>

/*
 * HV_SYNTHVID_GUID, the type GUID of the synthetic video channel.
 * The GUID is stored the way the hypervisor hands it to us: Data1..3
 * little-endian, Data4 as-is.
 */
#define HYPERVGFX_SYNTHVID_GUID_BYTES					\
	0x02, 0x78, 0x0a, 0xda, 0x77, 0xe3, 0xac, 0x4a,		\
	0x8e, 0x77, 0x05, 0x58, 0xeb, 0x10, 0x73, 0xf8

/*
 * Pipe level message types.
 */
#define HYPERVGFX_PIPE_MSG_INVALID	0
#define HYPERVGFX_PIPE_MSG_DATA		1
#define HYPERVGFX_PIPE_MSG_MAX

/*
 * Protocol versions.  The version is encoded the same way as the vmbus
 * version: major in the low 16 bits, minor in the high 16 bits.
 */
#define HYPERVGFX_VERSION(major, minor)	(((minor) << 16) | (major))
#define HYPERVGFX_VERSION_MAJOR(ver)	((ver) & 0xffff)
#define HYPERVGFX_VERSION_MINOR(ver)	(((ver) >> 16) & 0xffff)

#define HYPERVGFX_VERSION_WIN8		HYPERVGFX_VERSION(3, 2)
#define HYPERVGFX_VERSION_WIN10		HYPERVGFX_VERSION(3, 5)

#define HYPERVGFX_DEPTH_WIN8		32
#define HYPERVGFX_WIDTH_WIN8		1600
#define HYPERVGFX_HEIGHT_WIN8		1200

/*
 * sysctl(9) nodes.
 */
#define HYPERVGFX_STATUS	1
#define HYPERVGFX_TEST		2

/*
 * Synthetic video message types.
 */
enum hypervgfx_msg_type {
	HYPERVGFX_MSG_ERROR			= 0,
	HYPERVGFX_MSG_VERSION_REQUEST		= 1,
	HYPERVGFX_MSG_VERSION_RESPONSE		= 2,
	HYPERVGFX_MSG_VRAM_LOCATION		= 3,
	HYPERVGFX_MSG_VRAM_LOCATION_ACK	= 4,
	HYPERVGFX_MSG_SITUATION_UPDATE		= 5,
	HYPERVGFX_MSG_SITUATION_UPDATE_ACK	= 6,
	HYPERVGFX_MSG_POINTER_POSITION		= 7,
	HYPERVGFX_MSG_POINTER_SHAPE		= 8,
	HYPERVGFX_MSG_FEATURE_CHANGE		= 9,
	HYPERVGFX_MSG_DIRT			= 10,
	HYPERVGFX_MSG_RESOLUTION_REQUEST	= 13,
	HYPERVGFX_MSG_RESOLUTION_RESPONSE	= 14,
	HYPERVGFX_MSG_MAX			= 15
};

struct hypervgfx_pipe_msg_hdr {
	uint32_t type;
	uint32_t size;
} __packed;

struct hypervgfx_msg_hdr {
	uint32_t type;
	uint32_t size;
} __packed;

struct hypervgfx_version_req {
	uint32_t version;
} __packed;

struct hypervgfx_version_resp {
	uint32_t version;
	uint8_t is_accepted;
	uint8_t max_video_outputs;
} __packed;

struct hypervgfx_vram_location {
	uint64_t user_ctx;
	uint8_t is_vram_gpa_specified;
	uint64_t vram_gpa;
} __packed;

struct hypervgfx_vram_location_ack {
	uint64_t user_ctx;
} __packed;

struct hypervgfx_video_output_situation {
	uint8_t active;
	uint32_t vram_offset;
	uint8_t depth_bits;
	uint32_t width_pixels;
	uint32_t height_pixels;
	uint32_t pitch_bytes;
} __packed;

struct hypervgfx_situation_update {
	uint64_t user_ctx;
	uint8_t video_output_count;
	struct hypervgfx_video_output_situation video_output[1];
} __packed;

struct hypervgfx_situation_update_ack {
	uint64_t user_ctx;
} __packed;

struct hypervgfx_pointer_position {
	uint8_t is_visible;
	uint8_t video_output;
	int32_t image_x;
	int32_t image_y;
} __packed;

#define HYPERVGFX_CURSOR_MAX_X		96
#define HYPERVGFX_CURSOR_MAX_Y		96
#define HYPERVGFX_CURSOR_ARGB_PIXEL_SIZE	4
#define HYPERVGFX_CURSOR_MAX_SIZE					\
	(HYPERVGFX_CURSOR_MAX_X * HYPERVGFX_CURSOR_MAX_Y *		\
	 HYPERVGFX_CURSOR_ARGB_PIXEL_SIZE)
#define HYPERVGFX_CURSOR_COMPLETE	(-1)

struct hypervgfx_pointer_shape {
	uint8_t part_idx;
	uint8_t is_argb;
	uint32_t width;
	uint32_t height;
	uint32_t hot_x;
	uint32_t hot_y;
	uint8_t data[4];
} __packed;

struct hypervgfx_feature_change {
	uint8_t is_dirt_needed;
	uint8_t is_ptr_pos_needed;
	uint8_t is_ptr_shape_needed;
	uint8_t is_situ_needed;
} __packed;

struct hypervgfx_rect {
	int32_t x1, y1;
	int32_t x2, y2;
} __packed;

struct hypervgfx_dirt {
	uint8_t video_output;
	uint8_t dirt_count;
	struct hypervgfx_rect rect[1];
} __packed;

#define HYPERVGFX_EDID_BLOCK_SIZE	128
#define HYPERVGFX_MAX_RESOLUTION_COUNT	64

struct hypervgfx_screen_info {
	uint16_t width;
	uint16_t height;
} __packed;

struct hypervgfx_supported_resolution_req {
	uint8_t maximum_resolution_count;
} __packed;

struct hypervgfx_supported_resolution_resp {
	uint8_t edid_block[HYPERVGFX_EDID_BLOCK_SIZE];
	uint8_t resolution_count;
	uint8_t default_resolution_index;
	uint8_t is_standard;
	struct hypervgfx_screen_info
	    supported_resolution[HYPERVGFX_MAX_RESOLUTION_COUNT];
} __packed;

struct hypervgfx_msg {
	struct hypervgfx_pipe_msg_hdr pipe_hdr;
	struct hypervgfx_msg_hdr vid_hdr;
	union {
		struct hypervgfx_version_req ver_req;
		struct hypervgfx_version_resp ver_resp;
		struct hypervgfx_vram_location vram;
		struct hypervgfx_vram_location_ack vram_ack;
		struct hypervgfx_situation_update situ;
		struct hypervgfx_situation_update_ack situ_ack;
		struct hypervgfx_pointer_position ptr_pos;
		struct hypervgfx_pointer_shape ptr_shape;
		struct hypervgfx_feature_change feature_chg;
		struct hypervgfx_dirt dirt;
		struct hypervgfx_supported_resolution_req resolution_req;
		struct hypervgfx_supported_resolution_resp resolution_resp;
	};
} __packed;

struct hypervgfx_softc {
	device_t dev;
	struct vmbus_channel *chan;
	struct task rxtask;

	/* Protects the protocol state below. */
	struct mtx lock;

	/* Set when the expected response has been received. */
	bool resp_ready;
	/* Message type of the response we are waiting for. */
	uint32_t resp_expect;

	/* Negotiated protocol state. */
	uint32_t synthvid_version;
	uint32_t screen_depth;
	uint32_t screen_width_max;
	uint32_t screen_height_max;
	uint32_t preferred_width;
	uint32_t preferred_height;
	bool dirt_needed;

	/* Framebuffer. */
	vm_paddr_t fb_pbase;
	void *fb_vbase;
	size_t fb_size;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t fb_stride;
	uint32_t fb_bpp;
	vm_memattr_t fb_memattr;

	/* fbdev frontend (/dev/fb0). */
	struct fb_info fbi;
	bool fbi_registered;

	/* Receive buffer and bookkeeping. */
	uint8_t *rxbuf;
	size_t rxbuflen;
	/* Buffer holding the last response from the host. */
	uint8_t *respbuf;
	uint64_t xactid;
};

/* hypervgfx_proto.c */
int	hypervgfx_negotiate_version(struct hypervgfx_softc *, uint32_t);
int	hypervgfx_update_vram_location(struct hypervgfx_softc *, uint64_t);
int	hypervgfx_update_situation(struct hypervgfx_softc *, uint8_t, uint32_t,
	    uint32_t, uint32_t, uint32_t);
int	hypervgfx_get_supported_resolution(struct hypervgfx_softc *);
void	hypervgfx_hide_hw_ptr(struct hypervgfx_softc *);
int	hypervgfx_update_dirt(struct hypervgfx_softc *, const struct
	    hypervgfx_rect *);

/* hypervgfx_fb.c */
int	hypervgfx_fb_attach(struct hypervgfx_softc *);
void	hypervgfx_fb_detach(struct hypervgfx_softc *);
int	hypervgfx_fb_test_pattern(struct hypervgfx_softc *);

#endif	/* !_HYPERVGFX_H_ */
