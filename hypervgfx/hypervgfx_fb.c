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
 * Framebuffer handling for the Hyper-V synthetic video device.
 *
 * The hypervisor maps the video device's memory window into the guest
 * physical address space.  The firmware already set up a GOP framebuffer
 * inside that window, and the kernel knows about it through the preloaded
 * EFI framebuffer metadata.  For the time being we use that buffer as our
 * framebuffer: it is a region of guest physical memory the host is able to
 * render from, which is exactly what the protocol requires.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/linker.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/types.h>

#include <machine/metadata.h>
#include <machine/vmparam.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/hyperv/include/hyperv.h>
#include <dev/hyperv/include/vmbus.h>

#include "hypervgfx.h"

/*
 * Attach the framebuffer.  Returns zero on success.
 */
int
hypervgfx_fb_attach(struct hypervgfx_softc *sc)
{
	struct efi_fb *efifb;
	vm_memattr_t memattr;

	efifb = (struct efi_fb *)preload_search_info(preload_kmdp,
	    MODINFO_METADATA | MODINFOMD_EFI_FB);
	if (efifb == NULL) {
		device_printf(sc->dev, "no EFI framebuffer information found\n");
		return (ENODEV);
	}

	/*
	 * The GOP framebuffer is 32 bits per pixel; the stride is given in
	 * pixels.
	 */
	sc->fb_bpp = 32;
	sc->fb_pbase = efifb->fb_addr;
	sc->fb_size = efifb->fb_size;
	sc->fb_width = efifb->fb_width;
	sc->fb_height = efifb->fb_height;
	sc->fb_stride = efifb->fb_stride * (sc->fb_bpp / 8);

	device_printf(sc->dev, "EFI framebuffer: %ux%u, stride %u, "
	    "%zu bytes at 0x%016jx\n", sc->fb_width, sc->fb_height,
	    sc->fb_stride, sc->fb_size, (uintmax_t)sc->fb_pbase);

	if (sc->fb_size < (size_t)sc->fb_stride * sc->fb_height) {
		device_printf(sc->dev, "framebuffer is too small for its "
		    "geometry\n");
		return (EINVAL);
	}

#ifdef VM_MEMATTR_WRITE_COMBINING
	memattr = VM_MEMATTR_WRITE_COMBINING;
#else
	memattr = VM_MEMATTR_UNCACHEABLE;
#endif
	sc->fb_vbase = pmap_mapdev_attr(sc->fb_pbase, sc->fb_size, memattr);
	if (sc->fb_vbase == NULL) {
		device_printf(sc->dev, "unable to map the framebuffer\n");
		return (ENOMEM);
	}

	return (0);
}

void
hypervgfx_fb_detach(struct hypervgfx_softc *sc)
{

	if (sc->fb_vbase != NULL) {
		pmap_unmapdev(sc->fb_vbase, sc->fb_size);
		sc->fb_vbase = NULL;
	}
	sc->fb_size = 0;
}

/*
 * Fill the framebuffer with a test pattern so that we can tell whether
 * the host is actually rendering what we draw.
 */
int
hypervgfx_fb_test_pattern(struct hypervgfx_softc *sc)
{
	volatile uint32_t *line;
	uint32_t color;
	int i, j;

	if (sc->fb_vbase == NULL)
		return (ENODEV);

	for (i = 0; i < (int)sc->fb_height; i++) {
		line = (volatile uint32_t *)((uintptr_t)sc->fb_vbase +
		    (size_t)i * sc->fb_stride);
		color = (i % 2) ? 0x00ff00ff : 0x0000ffff;
		for (j = 0; j < (int)sc->fb_width; j++)
			line[j] = color;
	}

	return (0);
}
