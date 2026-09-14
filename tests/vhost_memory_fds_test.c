/* SPDX-License-Identifier: BSD-2-Clause */
#include <scorpi/vhost_user.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
check_regions(size_t count)
{
	struct scorpi_vhost_msg sent, received;
	int sockets[2], fds[SCORPI_VHOST_MAX_MEMORY_REGIONS];
	int received_fds[SCORPI_VHOST_MAX_MEMORY_REGIONS];
	FILE *files[SCORPI_VHOST_MAX_MEMORY_REGIONS];
	size_t received_count = 0;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	vhost_user_msg_init(&sent, SCORPI_VHOST_MSG_SET_MEM_TABLE,
	    sizeof(sent.payload.memory));
	sent.payload.memory.nregions = (uint32_t)count;
	for (size_t i = 0; i < count; i++) {
		uint64_t marker = UINT64_C(0x53434f5250490000) + i;
		files[i] = tmpfile();
		assert(files[i] != NULL);
		fds[i] = fileno(files[i]);
		assert(pwrite(fds[i], &marker, sizeof(marker), 0) == sizeof(marker));
		sent.payload.memory.regions[i].guest_phys_addr = i == count - 1 ?
		    UINT64_C(0x40000000) : UINT64_C(0x100000000) + (i << 30);
		sent.payload.memory.regions[i].memory_size = sizeof(marker);
	}
	/* Exercise both directions and verify every received descriptor's data. */
	for (unsigned int direction = 0; direction < 2; direction++) {
		assert(vhost_user_send_msg(sockets[direction], &sent, fds, count) > 0);
		memset(&received, 0, sizeof(received));
		assert(vhost_user_recv_msg(sockets[1 - direction], &received,
		    received_fds, SCORPI_VHOST_MAX_MEMORY_REGIONS,
		    &received_count) > 0);
		assert(received_count == count);
		assert(memcmp(&sent, &received,
		    SCORPI_VHOST_HEADER_SIZE + sent.header.size) == 0);
		for (size_t i = 0; i < count; i++) {
			uint64_t marker;
			assert(pread(received_fds[i], &marker, sizeof(marker), 0) == sizeof(marker));
			assert(marker == UINT64_C(0x53434f5250490000) + i);
			assert(close(received_fds[i]) == 0);
		}
	}
	/* Reject excess descriptors without sending or dereferencing the array. */
	errno = 0;
	assert(vhost_user_send_msg(sockets[0], &sent, fds,
	    SCORPI_VHOST_MAX_MEMORY_REGIONS + 1) == -1);
	assert(errno == EINVAL);
	for (size_t i = 0; i < count; i++)
		fclose(files[i]);
	close(sockets[0]);
	close(sockets[1]);
}

int
main(void)
{
	check_regions(8);
	check_regions(9); /* Eight high-RAM chunks plus the new low-RAM bank. */
	check_regions(SCORPI_VHOST_MAX_MEMORY_REGIONS);
	puts("Vhost memory-table descriptor transfer passed (8, 9 and maximum regions)");
	return (0);
}
