#ifndef HOSTCOMPAT_WINSOCK2_H
#define HOSTCOMPAT_WINSOCK2_H

/** @file Enough of Winsock and the Win32 threading types to build the client's
 * network sources on a POSIX host.
 *
 * The packet dispatchers in Game/src/network/ parse untrusted bytes, which is
 * exactly the code that most wants a test — and they could not have one,
 * because network.h includes <winsock2.h> and the whole tree only ever built
 * under MSYS2.
 *
 * It has two callers now. Game/tests uses it to drive net_dispatch_*() on the
 * machine running CI, which is what it was written for. The Makefile's
 * non-Windows build also points at it, so `make strict` compiles the network
 * sources rather than stopping at the first one: without that, a third of the
 * client was outside the warnings-are-errors build and stayed there.
 *
 * Nothing here reaches the shipped client. That is the MSYS2 build, which
 * finds the real Winsock headers first and never looks in this directory —
 * $(HOST_SHIM) is empty on Windows.
 *
 * Deliberately minimal. It provides the handful of names the network sources
 * actually mention, and nothing else, so it cannot quietly diverge into a
 * second socket layer.
 */

/* PTHREAD_MUTEX_RECURSIVE needs _GNU_SOURCE, which must be defined before the
 * first libc header in the translation unit -- too early for this header to do
 * it. The build passes -D_GNU_SOURCE; this is the check that it did. */
#ifndef _GNU_SOURCE
#error "build the host network sources with -D_GNU_SOURCE"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int   SOCKET;
typedef int   BOOL;
typedef char  CHAR;
typedef unsigned long DWORD;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define TRUE  1
#define FALSE 0

#define closesocket(s)   close(s)

/* Winsock's startup handshake has no POSIX counterpart; it succeeds trivially. */
typedef struct { int unused; } WSADATA;
#define MAKEWORD(lo, hi) (((hi) << 8) | (lo))
static inline int  WSAStartup(int version, WSADATA* data) { (void)version; (void)data; return 0; }
static inline void WSACleanup(void) {}

/* Winsock spells the non-blocking toggle as an ioctl on the socket. */
typedef unsigned long u_long;
static inline int ioctlsocket(SOCKET s, long cmd, u_long* argp) {
    (void)cmd;
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    if (argp && *argp) flags |= O_NONBLOCK;
    else               flags &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, flags);
}
#define FIONBIO 0x8004667e

/* Win32's millisecond sleep. */
static inline void Sleep(unsigned long ms) { usleep(ms * 1000UL); }
#define WSAGetLastError() (errno)
#define WSAEWOULDBLOCK   EWOULDBLOCK

/** A recursive mutex, which is what a Win32 critical section is. */
typedef pthread_mutex_t CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(cs); }
static inline void EnterCriticalSection(CRITICAL_SECTION* cs)  { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs)  { pthread_mutex_unlock(cs); }

#endif // HOSTCOMPAT_WINSOCK2_H
