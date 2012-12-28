#ifndef SHM_FIFO_H
#define SHM_FIFO_H
#include <stdint.h>

struct shm_fifo_eventfd_storage {
	int fd;
	int write_side_fd;
};

struct shm_fifo {
	unsigned head;
	unsigned head_wait;
	struct shm_fifo_eventfd_storage head_eventfd;

	__attribute__((aligned(128)))
	unsigned tail;
	unsigned tail_wait;
	struct shm_fifo_eventfd_storage tail_eventfd;

	__attribute__((aligned(128)))
	char data[0];
};

struct fifo_window {
	struct shm_fifo *fifo;
	unsigned start, len;
	unsigned min_length, pull_length;
	int reader;
};

#define FIFO_TOTAL_SIZE (65536+sizeof(struct shm_fifo))

#define FIFO_SIZE (FIFO_TOTAL_SIZE-sizeof(struct shm_fifo))

static inline
void *fifo_window_peek_span(struct fifo_window *window, unsigned *span_len)
{
	unsigned start = window->start % FIFO_SIZE;
	char *p = &(window->fifo->data[start]);
	if (span_len) {
		unsigned len = window->len;
		unsigned end = start + len;
		end = (end > FIFO_SIZE) ? FIFO_SIZE : end;
		len = end - start;
		*span_len = len;
	}
	return p;
}

static inline
void fifo_window_eat_span(struct fifo_window *window, unsigned span_len)
{
	unsigned start = window->start + span_len;
	window->start = start;
	window->len -= span_len;
}

static inline
void *fifo_window_get_span(struct fifo_window *window, unsigned *span_len)
{
	unsigned len;
	void *rv;
	rv = fifo_window_peek_span(window, &len);
	fifo_window_eat_span(window, len);
	if (span_len)
		*span_len = len;
	return rv;
}

extern int64_t fifo_reader_exchange_count;
extern int64_t fifo_writer_exchange_count;
extern int64_t fifo_reader_wake_count;
extern int64_t fifo_writer_wake_count;
extern int64_t fifo_reader_wait_spins;
extern int64_t fifo_writer_wait_spins;
extern int64_t fifo_reader_wait_calls;
extern int64_t fifo_writer_wait_calls;

int fifo_create(struct shm_fifo **ptr);

int fifo_window_init_reader(struct shm_fifo *fifo,
			    struct fifo_window *window,
			    unsigned min_length,
			    unsigned pull_length);

int fifo_window_init_writer(struct shm_fifo *fifo,
			    struct fifo_window *window,
			    unsigned min_length,
			    unsigned pull_length);

void fifo_window_reader_wait(struct fifo_window *window);
void fifo_window_writer_wait(struct fifo_window *window);

void fifo_window_exchange_writer(struct fifo_window *window);
void fifo_window_exchange_reader(struct fifo_window *window);

extern char *fifo_implementation_type;

#endif
