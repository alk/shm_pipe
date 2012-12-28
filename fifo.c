#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <errno.h>
#include <atomic_ops.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "fifo.h"

#define likely(cond) __builtin_expect((cond), 1)
#define unlikely(cond) __builtin_expect((cond), 0)

#ifndef FIFO_OVERRIDE

#define USE_EVENTFD 1
#define EVENTFD_NONBLOCKING 0
#define USE_EVENTFD_EMULATION 0

#endif

#define NO_LIBC_EVENTFD

#if USE_EVENTFD_EMULATION && USE_EVENTFD
char *fifo_implementation_type = "eventfd emulation via pipe(2)";
#elif USE_EVENTFD
#if EVENTFD_NONBLOCKING
char *fifo_implementation_type = "non-blocking eventfd(2)";
#else
char *fifo_implementation_type = "eventfd(2)";
#endif
#else
char *fifo_implementation_type = "futex(2)";
#endif


#if USE_EVENTFD
#include <poll.h>

#if USE_EVENTFD_EMULATION
typedef uint32_t eventfd_t;

#else /* USE_EVENTFD_EMULATION */

#ifdef NO_LIBC_EVENTFD

typedef uint64_t eventfd_t;

static
int eventfd(unsigned initval, int flags)
{
	return syscall(__NR_eventfd, initval, flags);
}
#else
#include <sys/eventfd.h>

#endif /* ! NONLIBC_EVENTFD */
#endif /* ! USE_EVENTFD_EMULATION */
#else /* ! USE_EVENTFD */
#include <linux/futex.h>
#endif

#define SPIN_COUNT 0

__attribute__((aligned(64)))
int fifo_reader_exchange_count;
int fifo_writer_wake_count;

__attribute__((aligned(64)))
int fifo_writer_exchange_count;
int fifo_reader_wake_count;

#if USE_EVENTFD
static __attribute__((unused))
int file_flags_change(int fd, int and_mask, int or_mask)
{
	int flags;
	int err = fcntl(fd, F_GETFL, (long)&flags);
	if (err < 0)
		return err;
	flags = (flags & and_mask) | or_mask;
	return fcntl(fd, F_SETFL, (long) &flags);
}

#if USE_EVENTFD_EMULATION
static
int eventfd_create(struct shm_fifo_eventfd_storage *this)
{
	int fds[2];
	int rv;
	rv = pipe(fds);
	if (rv < 0)
		return rv;
	this->fd = fds[0];
	this->write_side_fd = fds[1];
	file_flags_change(this->write_side_fd, -1, O_NONBLOCK);
	fprintf(stderr, "eventfd_emulation:eventfd_create:success\n");
	return 0;
}

static
void eventfd_release(struct shm_fifo_eventfd_storage *this)
{
	close(this->fd);
	close(this->write_side_fd);
}

#else /* !USE_EVENTFD_EMULATION */

static
int eventfd_create(struct shm_fifo_eventfd_storage *this)
{
	int fd = eventfd(0, 0);
	if (fd < 0) {
		perror("eventfd");
		return fd;
	}
#if EVENTFD_NONBLOCKING
	file_flags_change(fd, -1, O_NONBLOCK);
#endif
	this->fd = fd;
	fprintf(stderr, "eventfd:eventfd_create:success\n");
	return 0;
}

static
void eventfd_release(struct shm_fifo_eventfd_storage *this)
{
	close(this->fd);
}
#endif /* !USE_EVENTFD_EMULATION */
#endif /* USE_EVENTFD */

int fifo_create(struct shm_fifo **ptr)
{
	int err = posix_memalign((void **)ptr, 4096, FIFO_TOTAL_SIZE);
	struct shm_fifo *fifo = *ptr;
	if (!err) {
		memset(fifo, 0, offsetof(struct shm_fifo, data));
#if USE_EVENTFD
		int rv;
		rv = eventfd_create(&fifo->head_eventfd);
		if (rv)
			goto out_free;
		rv = eventfd_create(&fifo->tail_eventfd);
		if (rv) {
			eventfd_release(&fifo->head_eventfd);
		out_free:
			free(fifo);
			return 0;
		}
#else
		fprintf(stderr, "fifo_create: using futex implementation\n");
#endif // USE_EVENTFD
	}
	fifo->head_wait = fifo->tail_wait = 0xffffffff;

	return err;
}

static
int common_fifo_window_init(struct shm_fifo *fifo, struct fifo_window *window,
			    unsigned min_length, unsigned pull_length, int reader)
{
	window->fifo = fifo;
	window->reader = reader;
	window->len = 0;
	if (min_length > pull_length)
		pull_length = min_length;
	window->min_length = min_length;
	window->pull_length = pull_length;
	return 0;
}

int fifo_window_init_reader(struct shm_fifo *fifo, struct fifo_window *window,
			    unsigned min_length, unsigned pull_length)
{
	window->start = fifo->tail % FIFO_SIZE;
	return common_fifo_window_init(fifo, window, min_length, pull_length, 1);
}

int fifo_window_init_writer(struct shm_fifo *fifo, struct fifo_window *window,
			    unsigned min_length, unsigned pull_length)
{
	window->start = fifo->head % FIFO_SIZE;
	return common_fifo_window_init(fifo, window, min_length, pull_length, 0);
}

#if USE_EVENTFD && USE_EVENTFD_EMULATION
static
void eventfd_wait(struct shm_fifo_eventfd_storage *eventfd, unsigned *addr, unsigned wait_value)
{
	AO_nop_full();
	if (*addr != wait_value)
		return;
	eventfd_t buf[8];
	int rv;
again:
	rv = read(eventfd->fd, buf, sizeof(buf));
	if (rv < 0) {
		if (errno == EINTR)
			goto again;
		perror("eventfd_wait:read");
		abort();
	}
}

static
void eventfd_wake(struct shm_fifo_eventfd_storage *eventfd)
{
	eventfd_t value = 1;
	write(eventfd->write_side_fd, &value, sizeof(value));
}

#elif USE_EVENTFD
static
void eventfd_wait(struct shm_fifo_eventfd_storage *eventfd, unsigned *addr, unsigned wait_value)
{
	AO_nop_full();
	if (*addr != wait_value)
		return;
	int fd = eventfd->fd;
#if EVENTFD_NONBLOCKING
	struct pollfd wait;
	wait.fd = fd;
	wait.events = POLLIN;
	poll(&wait, 1, -1);
#endif
	eventfd_t tmp;
	read(fd, &tmp, sizeof(eventfd_t));
}

static
void eventfd_wake(struct shm_fifo_eventfd_storage *eventfd)
{
	eventfd_t value = 1;
	write(eventfd->fd, &value, sizeof(value));
}

#else /* !USE_EVENTFD */
static
int futex(int *uaddr, int op, int val, const struct timespec *timeout,
	  int *uaddr2, int val3)
{
	return syscall(__NR_futex, uaddr, op, val, timeout, uaddr2, val3);
}

static
void futex_wait(void *addr, unsigned value)
{
	int rv;
	do {
		rv = futex(addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, value, 0, 0, 0);
	} while (rv && errno == EINTR);
	if (rv) {
		if (errno == EWOULDBLOCK)
			return;
		perror("futex_wait");
		exit(1);
	}
}

static
void futex_wake(void *addr)
{
	int rv;
	rv = futex(addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
	if (rv < 0) {
		perror("futex_wake");
		exit(1);
	}
}
#endif

void fifo_window_reader_wait(struct fifo_window *window)
{
	struct shm_fifo *fifo = window->fifo;
	unsigned count;
	unsigned tail;
	unsigned head;

	if (!window->reader)
		abort();

	tail = fifo->tail;
	head = fifo->head;
	if (head - tail != window->len)
		return;

	for (count = SPIN_COUNT; count > 0; count--) {
		AO_nop_full();
		if (fifo->head != head)
			return;
	}
	
	fifo->head_wait = head;
	do {
#if USE_EVENTFD
		eventfd_wait(&fifo->head_eventfd, &fifo->head, head);
#else
		futex_wait(&fifo->head, head);
#endif
	} while (head == fifo->head);
}

void fifo_window_writer_wait(struct fifo_window *window)
{
	struct shm_fifo *fifo = window->fifo;
	unsigned count;
	unsigned tail;
	unsigned head;

	if (window->reader)
		abort();

	tail = fifo->tail;
	head = fifo->head;
	if (tail + FIFO_SIZE - head != window->len)
		return;

	for (count = SPIN_COUNT; count > 0; count--) {
		AO_nop_full();
		if (fifo->tail != tail)
			return;
	}
	
	fifo->tail_wait = tail;
	do {
#if USE_EVENTFD
		eventfd_wait(&fifo->tail_eventfd, &fifo->tail, tail);
#else
		futex_wait(&fifo->tail, tail);
#endif
	} while (tail == fifo->tail);
}

static
void shm_fifo_notify_reader(struct shm_fifo *fifo, unsigned old_head)
{
	AO_nop_full();
	if (fifo->head_wait == old_head) {
		fifo_reader_wake_count++;
#if USE_EVENTFD
		eventfd_wake(&fifo->head_eventfd);
#else
		futex_wake(&fifo->head);
#endif
	}
}

static
void shm_fifo_notify_writer(struct shm_fifo *fifo, unsigned old_tail)
{
	AO_nop_full();
	if (fifo->tail_wait == old_tail) {
		fifo_writer_wake_count++;
#if USE_EVENTFD
		eventfd_wake(&fifo->tail_eventfd);
#else
		futex_wake(&fifo->tail);
#endif
	}
}

static
void fifo_notify_invalid_window(struct fifo_window *window, int reader)
{
	fprintf(stderr, "window %p (reader = %d) (arg-reader = %d) is invalid. len = %u\n", (void *)window, window->reader, reader, window->len);
	__builtin_trap();
}

static inline
unsigned check_window_free_count(struct fifo_window *window, unsigned old_start, int reader)
{
	unsigned start = window->start;
	unsigned free_count = start - old_start;
	if (unlikely(free_count > FIFO_SIZE)) {
		fifo_notify_invalid_window(window, reader);
		abort();
	}
	return free_count;
}

void fifo_window_exchange_reader(struct fifo_window *window)
{
	unsigned len;
again:
	len = window->len;
	struct shm_fifo *fifo = window->fifo;
	unsigned tail = fifo->tail;
	unsigned free_count = check_window_free_count(window, tail, 1);
	unsigned old_tail = tail;

	tail += free_count;
	fifo->tail = tail;
	window->start = tail;

	if (len < window->pull_length)
		len = window->len = fifo->head - tail;

	if (len > FIFO_SIZE) {
		fifo_notify_invalid_window(window, 1);
		fifo->tail = fifo->head;
		window->len = 0;
		window->start = tail % FIFO_SIZE;
	}

	shm_fifo_notify_writer(fifo, old_tail);

	if (unlikely(len < window->min_length)) {
		fifo_window_reader_wait(window);
		goto again;
	}

	fifo_reader_exchange_count++;
}

void fifo_window_exchange_writer(struct fifo_window *window)
{
	unsigned len;
again:
	len = window->len;
	struct shm_fifo *fifo = window->fifo;
	unsigned head = fifo->head;
	unsigned free_count = check_window_free_count(window, head, 0);
	unsigned old_head = head;

	head += free_count;
	fifo->head = head;
	window->start = head;

	if (len < window->pull_length)
		len = window->len = fifo->tail + FIFO_SIZE - head;

	if (len > FIFO_SIZE) {
		fifo_notify_invalid_window(window, 0);
		fifo->head = fifo->tail;
		window->len = FIFO_SIZE;
		window->start = head % FIFO_SIZE;
	}

	shm_fifo_notify_reader(fifo, old_head);

	if (unlikely(len < window->min_length)) {
		fifo_window_writer_wait(window);
		goto again;
	}

	fifo_writer_exchange_count++;
}
