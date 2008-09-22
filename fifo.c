#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <errno.h>
#include <atomic_ops.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "fifo.h"

#define SPIN_COUNT 0

__attribute__((aligned(64)))
int fifo_reader_exchange_count;
int fifo_reader_wake_count;

__attribute__((aligned(64)))
int fifo_writer_exchange_count;
int fifo_writer_wake_count;

int fifo_create(struct futex_fifo **ptr)
{
	int err = posix_memalign((void **)ptr, 4096, FIFO_TOTAL_SIZE);
	if (!err)
		memset(*ptr, 0, offsetof(struct futex_fifo, data));
	(*ptr)->head_wait = (*ptr)->tail_wait = 0xffffffff;
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
		futex_wait(&fifo->head, head);
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
		futex_wait(&fifo->tail, tail);
	} while (tail == fifo->tail);
}

static
void futex_fifo_notify_reader(struct futex_fifo *fifo, unsigned old_head)
{
	AO_nop_full();
	if (fifo->head_wait == old_head) {
		fifo_reader_wake_count++;
		futex_wake(&fifo->head);
	}
}

static
void futex_fifo_notify_writer(struct futex_fifo *fifo, unsigned old_tail)
{
	AO_nop_full();
	if (fifo->tail_wait == old_tail) {
		fifo_writer_wake_count++;
		futex_wake(&fifo->tail);
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
