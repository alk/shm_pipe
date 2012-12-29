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

/* fifo_window reflects portion of fifo currently owned by reader or
 * writer. Start of window can be advanced by fifo_window_eat_span
 * (but note it won't be passed to reader/writer until next exchange
 * call). Actual pointer is retrieved by calling either
 * fifo_window_peek_span or fifo_window_get_span */
struct fifo_window {
	struct shm_fifo *fifo;
	unsigned start, len;
	unsigned min_length, pull_length;
	int reader;
};

#define FIFO_TOTAL_SIZE (65536+sizeof(struct shm_fifo))

#define FIFO_SIZE (FIFO_TOTAL_SIZE-sizeof(struct shm_fifo))

/* returns pointer and length of linear portion of window that's
 * available for consuming or producing */
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

/* advanced start of window by span_len. I.e. next call to peek will
 * point to byte after span_len in window. Note, that
 * produced/consumed portion released through this call _will not be_
 * passed back to fifo until next call to
 * fifo_window_exchange_{writer,reader} */
static inline
void fifo_window_eat_span(struct fifo_window *window, unsigned span_len)
{
	unsigned start = window->start + span_len;
	window->start = start;
	window->len -= span_len;
}

/* combines peek and eat in convenient single call. Note: next call to
 * fifo_window_exchange_{reader,writer} will pass data returned by
 * this call to fifo, essentially invalidating it */
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

/* inits window. min_length arg is size of window below which it'll
 * automatically wait for more in exchange call. pull_length arg is
 * size of window below which it'll attempt to grab all available
 * data/free-space in exchange call */
int fifo_window_init_reader(struct shm_fifo *fifo,
			    struct fifo_window *window,
			    unsigned min_length,
			    unsigned pull_length);

int fifo_window_init_writer(struct shm_fifo *fifo,
			    struct fifo_window *window,
			    unsigned min_length,
			    unsigned pull_length);

/* waits until more data or space is available for consuming or
 * producing. Note: it won't actually advance len of window, it has to
 * be done via call to exchange below */
void fifo_window_reader_wait(struct fifo_window *window);
void fifo_window_writer_wait(struct fifo_window *window);

/* releases "eaten" (i.e. consumed by consumer or produced by
 * producer) portion of window back to fifo and (depending on window
 * pull_length and min_length options) gets fresh data/free-space from
 * fifo */
void fifo_window_exchange_writer(struct fifo_window *window);
void fifo_window_exchange_reader(struct fifo_window *window);

extern char *fifo_implementation_type;

#endif
