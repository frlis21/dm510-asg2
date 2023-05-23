#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define DM510_MAJOR 255
#define IOCTL_GET_MAX_READERS _IOR(DM510_MAJOR, 0, ssize_t)
#define IOCTL_SET_MAX_READERS _IOW(DM510_MAJOR, 1, size_t)
#define IOCTL_GET_BUFFER_SIZE _IOR(DM510_MAJOR, 2, ssize_t)
#define IOCTL_SET_BUFFER_SIZE _IOW(DM510_MAJOR, 3, size_t)
#define IOCTL_MAX_NR 3

unsigned long ioctl_cmds[] = {
	IOCTL_GET_MAX_READERS,
	IOCTL_SET_MAX_READERS,
	IOCTL_GET_BUFFER_SIZE,
	IOCTL_SET_BUFFER_SIZE,
};

char *cmd_names[] = {
	"get_max_readers",
	"set_max_readers",
	"get_buffer_size",
	"set_buffer_size",
};

int main(int argc, char *argv[])
{
	if (argc < 3) {
		return 1;
	}

	int fd;
	long result;
	char *cmd_name;
	unsigned long cmd, arg;

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	cmd_name = argv[2];
	if (argc > 3)
		arg = strtol(argv[3], NULL, 10);

	for (int i = 0; i <= IOCTL_MAX_NR; i++) {
		if (strcmp(cmd_name, cmd_names[i]) == 0) {
			cmd = ioctl_cmds[i];
			break;
		}
	}

	result = argc > 3 ? ioctl(fd, cmd, arg) : ioctl(fd, cmd);
	close(fd);

	if (result < 0) {
		perror("ioctl");
		return result;
	}

	if (argc == 3)
		printf("%ld\n", result);

	return 0;
}
