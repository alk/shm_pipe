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

#define USE_EVENTFD 1
#define NO_LIBC_EVENTFD

#if USE_EVENTFD
#include <poll.h>

#ifdef NO_LIBC_EVENTFD

typedef uint64_t eventfd_t;

static
int eventfd(unsigned initval, int flags)
{
	return syscall(__NR_eventfd, initval, flags);
}

#else
#include <sys/eventfd.h>

#endif /* NONLIBC_EVENTFD */
#else /* ! USE_EVENTFD */
#include <linux/futex.h>
#endif

#define SPIN_COUNT 0

__attribute__((aligned(64)))
int fifo_reader_exchange_count;
int fifo_reader_wake_count;

__attribute__((aligned(64)))
int fifo_writer_exchange_count;
int fifo_writer_wake_count;

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

static
int create_eventfd()
{
	int fd = eventfd(0, 0);
	if (fd < 0) {
		perror("eventfd");
		return fd;
	}
#if EVENTFD_NONBLOCKING
	file_flags_change(fd, -1, O_NONBLOCK);
#endif
	return fd;
}
#endif

int fifo_create(struct futex_fifo **ptr)
{
	int err = posix_memalign((void **)ptr, 4096, FIFO_TOTAL_SIZE);
	struct futex_fifo *fifo = *ptr;
	if (!err) {
		memset(fifo, 0, offsetof(struct futex_fifo, data));
#if USE_EVENTFD
		int fd;
		fifo->eventfd_head = fd = create_eventfd();
		if (fd < 0)
			goto out_free;
		fifo->eventfd_tail = fd = create_eventfd();
		if (fd < 0) {
			close(fifo->eventfd_head);
		out_free:
			free(fifo);
			return 0;
		}
#endif // USE_EVENTFD
	}
	fifo->head_wait = fifo->tail_wait = 0xffffffff;

	return err;
}

void fifo_window_init_reader(struct futex_fifo *fifo, struct fifo_window *window)
{
	window->fifo = fifo;
	window->reader = 1;
	window->start = fifo->tail % FIFO_SIZE;
	window->len = 0;
}

void fifo_window_init_writer(struct futex_fifo *fifo, struct fifo_window *window)
{
	window->fifo = fifo;
	window->reader = 0;
	window->start = fifo->head % FIFO_SIZE;
	window->len = 0;
}

#if USE_EVENTFD
static
void eventfd_wait(int fd, unsigned *addr, unsigned wait_value)
{
#if EVENTFD_NONBLOCKING
	if (sizeof(eventfd_t) > sizeof(struct pollfd))
		abort();
#endif
	AO_nop_full();
	if (*addr == wait_value) {
#if EVENTFD_NONBLOCKING
		struct pollfd wait;
		wait.fd = fd;
		wait.events = POLLIN;
		poll(&wait, 1, -1);
#else
		eventfd_t wait;
#endif
		read(fd, &wait, sizeof(eventfd_t));
	}
}

static
void eventfd_wake(int fd) {
	eventfd_t value = 1;
	AO_nop_full();
	write(fd, &value, sizeof(value));
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
	struct futex_fifo *fifo = window->fifo;
	unsigned count;
	unsigned tail;
	unsigned head;

	tail = fifo->tail;
	head = fifo->head;
	if (head - tail != window->len)
		return;

	for (count = SPIN_COUNT; count > 0; count--) {
		abort();
		AO_nop_full();
		if (fifo->head != head)
			return;
	}
	
	fifo->head_wait = head;
	do {
#if USE_EVENTFD
		eventfd_wait(fifo->eventfd_head, &fifo->head, head);
#else
		futex_wait(&fifo->head, head);
#endif
	} while (head == fifo->head);
}

void fifo_window_writer_wait(struct fifo_window *window)
{
	struct futex_fifo *fifo = window->fifo;
	unsigned count;
	unsigned tail;
	unsigned head;

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
		eventfd_wait(fifo->eventfd_tail, &fifo->tail, tail);
#else
		futex_wait(&fifo->tail, tail);
#endif
	} while (tail == fifo->tail);
}

static
void futex_fifo_notify_reader(struct futex_fifo *fifo, unsigned old_head)
{
	AO_nop_full();
	if (fifo->head_wait == old_head) {
		fifo_reader_wake_count++;
#if USE_EVENTFD
		eventfd_wake(fifo->eventfd_head);
#else
		futex_wake(&fifo->head);
#endif
	}
}

static
void futex_fifo_notify_writer(struct futex_fifo *fifo, unsigned old_tail)
{
	AO_nop_full();
	if (fifo->tail_wait == old_tail) {
		fifo_writer_wake_count++;
#if USE_EVENTFD
		eventfd_wake(fifo->eventfd_tail);
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
	if (free_count > FIFO_SIZE) {
		fifo_notify_invalid_window(window, reader);
		abort();
	}
	return free_count;
}

void fifo_window_exchange_reader(struct fifo_window *window)
{
	struct futex_fifo *fifo = window->fifo;
	unsigned tail = fifo->tail;
	unsigned free_count = check_window_free_count(window, tail, 1);
	unsigned head = fifo->head;
	unsigned old_tail = tail;

	tail += free_count;
	fifo->tail = tail;
	window->start = tail;
	window->len = head - tail;

	if (window->len > FIFO_SIZE) {
		fifo_notify_invalid_window(window, 1);
		fifo->tail = head;
		window->len = 0;
		window->start = tail % FIFO_SIZE;
	}

	fifo_reader_exchange_count++;
	futex_fifo_notify_writer(fifo, old_tail);
}

void fifo_window_exchange_writer(struct fifo_window *window)
{
	struct futex_fifo *fifo = window->fifo;
	unsigned head = fifo->head;
	unsigned free_count = check_window_free_count(window, head, 0);
	unsigned tail = fifo->tail;
	unsigned old_head = head;

	head += free_count;
	fifo->head = head;
	window->start = head;
	window->len = tail + FIFO_SIZE - head;

	if (window->len > FIFO_SIZE) {
		fifo_notify_invalid_window(window, 0);
		fifo->head = tail;
		window->len = FIFO_SIZE;
		window->start = head % FIFO_SIZE;
	}

	fifo_writer_exchange_count++;
	futex_fifo_notify_reader(fifo, old_head);
}
