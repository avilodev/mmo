// Notes the instant a ping's echo reaches this machine; see net_ping_watch.h.

#include "net_internal.h"
#include "network/net_ping.h"
#include "network/net_ping_watch.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

// More than a frame's worth of stream; an echo behind more than this is missed
#define PEEK_MAX 16384

// Looks this long, in seconds, before deciding the echo is not coming
#define GIVE_UP_SECONDS 3.0

// A fast link is followed closely; a slow one does not need a core to itself
#define CLOSE_WATCH_SECONDS 0.005

typedef struct {
	CRITICAL_SECTION lock;
	int started;
	int armed;
	unsigned generation; // which arming the thread is working on
	SOCKET sock;
	uint8_t needle[PING_PACKET_MAX];
	size_t needle_length;
	int seen;
	double arrived_seconds;
} watch_state;

static watch_state g_watch;

// --- The thread, and the signal that wakes it -------------------------------

#ifdef _WIN32

static HANDLE g_wake;

static void wake_create(void) {
	g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
}

static void wake_signal(void) {
	SetEvent(g_wake);
}

static void wake_wait(void) {
	WaitForSingleObject(g_wake, INFINITE);
}

static void pause_close(void) {
	SwitchToThread();
}

static void pause_far(void) {
	Sleep(1);
}

static void watch_loop(void);

static DWORD WINAPI watch_main(LPVOID unused) {
	(void)unused;

	// Without this a one millisecond sleep lasts up to fifteen
	timeBeginPeriod(1);
	watch_loop();

	return 0;
}

static int thread_start(void) {
	HANDLE thread = CreateThread(NULL, 0, watch_main, NULL, 0, NULL);

	if(!thread)
		return 0;
	CloseHandle(thread);

	return 1;
}

#else

static pthread_mutex_t g_wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake_cond = PTHREAD_COND_INITIALIZER;
static int g_wake_pending;

static void wake_create(void) {
}

static void wake_signal(void) {
	pthread_mutex_lock(&g_wake_lock);
	g_wake_pending = 1;
	pthread_cond_signal(&g_wake_cond);
	pthread_mutex_unlock(&g_wake_lock);
}

static void wake_wait(void) {
	pthread_mutex_lock(&g_wake_lock);
	while(!g_wake_pending)
		pthread_cond_wait(&g_wake_cond, &g_wake_lock);
	g_wake_pending = 0;
	pthread_mutex_unlock(&g_wake_lock);
}

static void pause_close(void) {
	sched_yield();
}

static void pause_far(void) {
	Sleep(1);
}

static void watch_loop(void);

static void* watch_main(void* unused) {
	(void)unused;
	watch_loop();

	return NULL;
}

static int thread_start(void) {
	pthread_t thread;

	if(pthread_create(&thread, NULL, watch_main, NULL) != 0)
		return 0;
	pthread_detach(thread);

	return 1;
}

#endif

// Whether this arming is still the one wanted.
static int still_wanted(unsigned generation) {
	int wanted;

	EnterCriticalSection(&g_watch.lock);
	wanted = g_watch.armed && g_watch.generation == generation;
	LeaveCriticalSection(&g_watch.lock);

	return wanted;
}

// Records the arrival, unless the watch moved on while the bytes were read.
static void note_arrival(unsigned generation, double arrived_seconds) {
	EnterCriticalSection(&g_watch.lock);
	if(g_watch.armed && g_watch.generation == generation) {
		g_watch.seen = 1;
		g_watch.arrived_seconds = arrived_seconds;
	}
	LeaveCriticalSection(&g_watch.lock);
}

// Peeks at the stream until the echo is whole in it, or there is no point.
static void watch_once(SOCKET sock, const uint8_t* needle, size_t needle_length,
		unsigned generation) {
	static uint8_t peeked[PEEK_MAX];
	double started = net_now();

	while(still_wanted(generation)) {
		int got = (int)recv(sock, (char*)peeked, (int)sizeof(peeked), MSG_PEEK);
		double now;

		if(got > 0 && ping_bytes_contain(peeked, (size_t)got, needle,
				needle_length)) {
			note_arrival(generation, net_now());
			return;
		}
		if(got == 0 || (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
			return;

		now = net_now();
		if(now - started > GIVE_UP_SECONDS)
			return;

		if(now - started < CLOSE_WATCH_SECONDS)
			pause_close();
		else
			pause_far();
	}
}

static void watch_loop(void) {
	for(;;) {
		SOCKET sock;
		uint8_t needle[PING_PACKET_MAX];
		size_t needle_length;
		unsigned generation;
		int armed;

		wake_wait();

		EnterCriticalSection(&g_watch.lock);
		armed = g_watch.armed;
		sock = g_watch.sock;
		generation = g_watch.generation;
		needle_length = g_watch.needle_length;
		memcpy(needle, g_watch.needle, sizeof(needle));
		LeaveCriticalSection(&g_watch.lock);

		if(armed)
			watch_once(sock, needle, needle_length, generation);
	}
}

// Starts the thread the first time it is wanted.
static int ensure_started(void) {
	if(g_watch.started)
		return 1;

	InitializeCriticalSection(&g_watch.lock);
	wake_create();
	if(!thread_start())
		return 0;
	g_watch.started = 1;

	return 1;
}

void net_ping_watch_arm(SOCKET sock, const uint8_t* packet, size_t length) {
	if(length == 0 || length > PING_PACKET_MAX || !ensure_started())
		return;

	EnterCriticalSection(&g_watch.lock);
	g_watch.armed = 1;
	g_watch.generation++;
	g_watch.sock = sock;
	g_watch.needle_length = length;
	memcpy(g_watch.needle, packet, length);
	g_watch.seen = 0;
	LeaveCriticalSection(&g_watch.lock);

	wake_signal();
}

void net_ping_watch_disarm(void) {
	if(!g_watch.started)
		return;

	EnterCriticalSection(&g_watch.lock);
	g_watch.armed = 0;
	g_watch.seen = 0;
	LeaveCriticalSection(&g_watch.lock);
}

int net_ping_watch_take(double* arrived_seconds) {
	int seen = 0;

	if(!g_watch.started)
		return 0;

	EnterCriticalSection(&g_watch.lock);
	if(g_watch.armed && g_watch.seen) {
		*arrived_seconds = g_watch.arrived_seconds;
		seen = 1;
	}
	g_watch.armed = 0;
	g_watch.seen = 0;
	LeaveCriticalSection(&g_watch.lock);

	return seen;
}
