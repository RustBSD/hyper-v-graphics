#include <sys/consio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int
main(void)
{
	int fd, i, active;
	vtmode_t vm;
	char path[32];

	fd = open("/dev/ttyv0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, VT_GETACTIVE, &active) == 0)
		printf("active vt: %d\n", active);
	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/ttyv%d", i);
		int t = open(path, O_RDWR);
		if (t < 0) continue;
		if (ioctl(t, VT_GETMODE, &vm) == 0)
			printf("ttyv%d: mode=%s waitv=%d relsig=%d acqsig=%d frsig=%d\n",
			    i, vm.mode == VT_AUTO ? "AUTO" : vm.mode == VT_PROCESS ? "PROCESS" : "?",
			    vm.waitv, vm.relsig, vm.acqsig, vm.frsig);
		close(t);
	}
	return 0;
}
