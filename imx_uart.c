/*
 * imx_usb:
 *
 * Program to download and execute an image over the USB boot protocol
 * on i.MX series processors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#include <unistd.h>
#include <ctype.h>
#include <sys/io.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>

#include <fcntl.h>
#include <termios.h>

#include <sys/ioctl.h>
#include <linux/serial.h>

#include "imx_sdp.h"

#define get_min(a, b) (((a) < (b)) ? (a) : (b))

int transfer_uart(struct sdp_dev *dev, int report, unsigned char *p, unsigned size,
		unsigned int expected, int* last_trans)
{
	int fd = *(int *)dev->priv;

	if (report < 3) {
		*last_trans = write(fd, p, size);
	} else {
		// Read...
		int ret;
		*last_trans = 0;
		while (*last_trans < expected)
		{
			ret = read(fd, p, expected - *last_trans);
			if (ret < 0)
				return ret;

			// err is transfered bytes...
			*last_trans += ret;
			p += ret;
		}
	}

	return 0;
}

int connect_uart(int *uart_fd, char const *tty, int usectsrts)
{
	int err = 0, count = 0;
	int i;
	int flags = O_RDWR | O_NOCTTY | O_SYNC;
	struct termios key;
	struct serial_struct ser_info; 
	char magic[] = { 0x23, 0x45, 0x45, 0x23 };
	char magic_response[4];
	char *buf;
	memset(&key,0,sizeof(key));
	memset(&magic_response,0,sizeof(magic_response));

	*uart_fd = open(tty, flags);
	if (*uart_fd < 0) {
		printf("tty %s\n", tty);
		fprintf(stdout, "open() failed: %s\n", strerror(errno));
		return *uart_fd;
	}

	/* 8 data bits */
	key.c_cflag |= CS8;
	key.c_cflag |= CLOCAL | CREAD;
	if (usectsrts)
		key.c_cflag |= CRTSCTS;
	key.c_cflag |= B115200;

	/* Enable blocking read, 0.5s timeout.. */
	key.c_cc[VMIN] = 1;
	key.c_cc[VTIME] = 5;

	err = tcsetattr(*uart_fd, TCSAFLUSH, &key);
	if (err < 0) {
		fprintf(stdout, "tcsetattr() failed: %s\n", strerror(errno));
		close(*uart_fd);
		return err;
	}

	err = tcflush(*uart_fd, TCIOFLUSH);

	// Association phase, send and receive 0x23454523
	printf("starting associating phase\n");
	write(*uart_fd, magic, sizeof(magic));
	err = tcflush(*uart_fd, TCIOFLUSH);
	
	buf = magic_response;
	while (count < 4) {
		err = read(*uart_fd, buf, 4 - count);

		if (err < 0) {
			fprintf(stderr, "magic timeout, make sure the device "
			       "is in recovery mode\n");
			return err;
		}

		count += err;
		buf += err;
	}
	err = 0;

	for (i = 0; i < sizeof(magic); i++) {
		if (magic[i] != magic_response[i]) {
			fprintf(stderr, "magic missmatch, response was 0x%08x\n",
					*(uint32_t *)magic_response);
			return -1;
		}
	}

	fprintf(stderr, "association phase succeeded, response was 0x%08x\n",
				*(uint32_t *)magic_response);

	return err;
}

void print_usage(void)
{
	printf("Usage: imx_uart [OPTIONS...] UART CONFIG [JOBS...]\n"
		"  e.g. imx_uart -n /dev/ttyUSB0 vybrid_usb_work.conf u-boot.imx\n"
		"Load data on target connected to UART using serial download protocol as\n"
		"configured in CONFIG file.\n"
		"\n"
		"Where OPTIONS are\n"
		"   -h --help		Show this help\n"
		"   -v --verify		Verify downloaded data\n"
		"   -n --no-ctsrts	Do not use CTS/RTS flow control\n"
		"			Default is to use CTS/RTS, Vybrid requires them\n"
		"\n"
		"And where [JOBS...] are\n"
		"   FILE [-lLOADADDR] [-sSIZE] ...\n"
		"Multiple jobs can be configured. The first job is treated special, load\n"
		"address, jump address, and length are read from the IVT header. If no job\n"
		"is specified, the jobs definied in the target specific configuration file\n"
		"is being used.\n");
}

int parse_opts(int argc, char * const *argv, char const **ttyfile,
		char const **conffile, int *verify, int *usectsrts,
		struct sdp_work **cmd_head)
{
	char c;
	*conffile = NULL;
	*ttyfile = NULL;

	static struct option long_options[] = {
		{"help",	no_argument, 	0, 'h' },
		{"verify",	no_argument, 	0, 'v' },
		{"no-ctsrts",	no_argument, 	0, 'n' },
		{0,		0,		0, 0 },
	};

	while ((c = getopt_long(argc, argv, "+hvn", long_options, NULL)) != -1) {
		switch (c)
		{
		case 'h':
		case '?':
			print_usage();
			return -1;
		case 'n':
			*usectsrts = 0;
			break;
		case 'v':
			*verify = 1;
			break;
		}
	}

	// Options parsed, get mandatory arguments...
	if (optind >= argc) {
		fprintf(stderr, "non optional argument UART is missing\n");
		return -1;
	}

	*ttyfile = argv[optind];
	optind++;

	if (optind >= argc) {
		fprintf(stderr, "non optional argument CONFIG is missing\n");
		return -1;
	}

	*conffile = argv[optind];
	optind++;

	if (optind < argc) {
		// Parse optional job arguments...
		*cmd_head = parse_cmd_args(argc - optind, &argv[optind]);
	}

	return 0;
}

#define ARRAY_SIZE(w) sizeof(w)/sizeof(w[0])

int main(int argc, char * const argv[])
{
	struct sdp_dev *p_id;
	int err;
	ssize_t cnt;
	int config = 0;
	int verify = 0;
	int usectsrts = 1;
	int uart_fd;
	struct sdp_work *curr;
	char const *conf;
	char const *ttyfile;
	char const *conffilepath;
	char const *conffile;
	char const *basepath;

	err = parse_opts(argc, argv, &ttyfile, &conffilepath, &verify, &usectsrts, &curr);

	if (err < 0)
		return err;

	// Get machine specific configuration file..
	if ((conffile = strrchr(conffilepath, '/')) == NULL) {
		// Only a file was given as configuration
		basepath = get_base_path(argv[0]);
		conffile = conffilepath;
	} else {
		// A whole path is given as configuration
		basepath = get_base_path(conffilepath);
		conffile++; // Filename starts after slash
	}

	conf = conf_file_name(conffile, basepath, "/etc/imx-loader.d/");
	if (conf == NULL)
		return -1;

	p_id = parse_conf(conf);
	if (!p_id)
		return -1;

	// Open UART and start associating phase...
	err = connect_uart(&uart_fd, ttyfile, usectsrts);
	if (err < 0)
		goto out;

	p_id->transfer = &transfer_uart;

	// UART private pointer is TTY file descriptor...
	p_id->priv = &uart_fd;

	err = do_status(p_id);
	if (err) {
		printf("status failed\n");
		goto out;
	}

	// By default, use work from config file...
	if (curr == NULL)
		curr = p_id->work;

	while (curr) {
		if (curr->mem)
			perform_mem_work(p_id, curr->mem);
//		printf("jump_mode %x\n", curr->jump_mode);
		if (curr->filename[0]) {
			err = DoIRomDownload(p_id, curr, verify);
		}
		if (err) {
			err = do_status(p_id);
			break;
		}
		if (!curr->next && !curr->plug)
			break;
		err = do_status(p_id);
		printf("jump_mode %x plug=%i err=%i\n", curr->jump_mode, curr->plug, err);

		if (err)
			goto out;

		if (curr->plug) {
			curr->plug = 0;
			continue;
		}
		curr = curr->next;
	}

out:
	return 0;
}