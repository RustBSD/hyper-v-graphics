#include <sys/consio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int fd, n;

	if (argc != 2) {
		fprintf(stderr, "usage: vtswitch <vty-number>\n");
		return (1);
	}
	n = atoi(argv[1]);
	fd = open("/dev/ttyv0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/ttyv0");
		return (1);
	}
	if (ioctl(fd, VT_ACTIVATE, n) < 0) {
		perror("VT_ACTIVATE");
		return (1);
	}
	if (ioctl(fd, VT_WAITACTIVE, n) < 0) {
		perror("VT_WAITACTIVE");
		return (1);
	}
	printf("switched to vt%d\n", n);
	return (0);
}
