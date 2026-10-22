// Check the ping watch stamps an echo when it arrives, not when it is read.

#include "network/net_ping.h"
#include "network/net_ping_watch.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The game's clock, which the watch reads through net_now().
double net_now(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap_ms(long ms) {
	struct timespec nap = { .tv_sec = 0, .tv_nsec = ms * 1000000L };

	nanosleep(&nap, NULL);
}

typedef struct {
	int fd;
	long delay_ms; // how long the far end holds the packet before echoing
} echo_job;

// The far end: reads one ping and sends it straight back.
static void* echo_main(void* arg) {
	echo_job* job = arg;
	uint8_t packet[PING_PACKET_MAX];
	ssize_t got = 0;

	while(got < 13) {
		ssize_t n = recv(job->fd, packet + got, sizeof(packet) - (size_t)got, 0);

		assert(n > 0);
		got += n;
	}
	nap_ms(job->delay_ms);
	assert(send(job->fd, packet, (size_t)got, 0) == got);

	return NULL;
}

static void link_pair(int* near_end, int* far_end) {
	int fds[2];
	int on = 1;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	(void)on;
	*near_end = fds[0];
	*far_end = fds[1];
	assert(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == 0);
}

// One ping; returns the figure, in milliseconds, the watch gave, or -1.
// The frame reads the echo frame_ms after sending, as the game does.
static double one_ping(uint32_t number, long delay_ms, long frame_ms) {
	int near_end;
	int far_end;
	uint8_t packet[PING_PACKET_MAX];
	uint8_t echoed[PING_PACKET_MAX];
	size_t size = ping_build(packet, 1, NET_PING_UNKNOWN, number);
	echo_job job;
	pthread_t far_thread;
	double sent;
	double arrived;
	double figure = -1.0;

	link_pair(&near_end, &far_end);
	job.fd = far_end;
	job.delay_ms = delay_ms;
	pthread_create(&far_thread, NULL, echo_main, &job);

	net_ping_watch_arm(near_end, packet, size);
	sent = net_now();
	assert(send(near_end, packet, size, 0) == (ssize_t)size);

	nap_ms(frame_ms);
	// the frame finally reads
	assert(recv(near_end, echoed, sizeof(echoed), 0) == (ssize_t)size);
	assert(memcmp(echoed, packet, size) == 0);

	if(net_ping_watch_take(&arrived))
		figure = ping_one_way_ms(sent, arrived);

	pthread_join(far_thread, NULL);
	close(near_end);
	close(far_end);

	return figure;
}

int main(void) {
	// An instant link read a frame later is still near zero, not a frame
	for(uint32_t i = 1; i <= 20; i++) {
		double figure = one_ping(i, 0, 17);

		assert(figure >= 0.0);
		assert(figure < 2.0);
	}

	// A 20 ms trip (10 ms each way) read a frame later is still about 10 ms
	for(uint32_t i = 21; i <= 25; i++) {
		double figure = one_ping(i, 20, 40);

		assert(figure >= 9.5);
		assert(figure < 13.0);
	}

	// Nothing seen is nothing taken
	net_ping_watch_disarm();
	double ignored;
	assert(!net_ping_watch_take(&ignored));

	puts("net_ping_watch_test: all passed");
	return 0;
}
