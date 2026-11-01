#include <sys/fbio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int
main(void)
{
	struct fbtype t;
	unsigned int *p;
	int fd, i;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, FBIOGTYPE, &t) < 0) { perror("FBIOGTYPE"); return 1; }
	printf("fb %dx%d depth %d size %d\n", t.fb_width, t.fb_height,
	    t.fb_depth, t.fb_size);
	p = mmap(NULL, t.fb_size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	/* sample a grid of pixels */
	for (i = 0; i < 6; i++) {
		long off = ((long)(t.fb_height / 6) * i + t.fb_height / 12) *
		    t.fb_width + t.fb_width / 2;
		printf("row%d @%ld = 0x%08x\n", i, off, p[off]);
	}
	return 0;
}
