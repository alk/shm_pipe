#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

#if !USE_PIPE

#include "fifo.h"

static
struct shm_fifo *fifo;

static
sem_t reader_sem;
static
sem_t writer_sem;

int done_flag;

#else

#define BUFFERSIZE 32768

static
int reader_buffer[BUFFERSIZE/sizeof(int)];
static
int writer_buffer[BUFFERSIZE/sizeof(int)];

static
int read_fd, write_fd;

static
char *fifo_implementation_type = "read/write over pipe";

#endif

static
int setaffinity;
static
int serialize;

static
void fatal_perror(char *arg)
{
	perror(arg);
	exit(1);
}

static
pid_t gettid(void)
{
	return syscall(__NR_gettid);
}

static
void move_to_cpu(int number)
{
	pid_t tid = gettid();
	cpu_set_t set;
	int rv;

	CPU_ZERO(&set);
	CPU_SET(number, &set);
	rv = sched_setaffinity(tid, sizeof(set), &set);
	if (rv < 0)
		fatal_perror("sched_setaffinity");
}

#define READER_BATCH 256

static
void reader_normal_inner_loop(int *ptr, int len, uint64_t *count_ptr, int *sum_ptr,
			      unsigned short *xsubi)
{
	int sum = 0;
	for (int i = 0; i < len; i++)
		sum |= ptr[i] ^ nrand48(xsubi);
	*sum_ptr |= sum;
	*count_ptr += len;
}

static
void reader_fast_inner_loop(int *ptr, int len, uint64_t *count_ptr, int *sum_ptr,
			    unsigned short *xsubi)
{
	int sum = 0;
	for (int i = 0; i < len; i++)
		sum |= ptr[i];
	*sum_ptr |= sum;
	*count_ptr += len;
}

void (*reader_inner_loop)(int *ptr, int len, uint64_t *count_ptr, int *sum_ptr,
			  unsigned short *xsubi) = reader_normal_inner_loop;

static
void *reader_thread(void *dummy)
{
	unsigned long long count=0;
	int sum=0;
	unsigned short xsubi[3];
	memset(xsubi, 0, sizeof(xsubi));

	printf("reader's pid is %d\n", gettid());
	if (setaffinity)
		move_to_cpu(0);

#if !USE_PIPE
	struct fifo_window window;

	if (serialize)
		sem_wait(&reader_sem);

	fifo_window_init_reader(fifo, &window, 0, READER_BATCH*sizeof(int)*2);
#endif
	while (1) {
		int *ptr;
		unsigned len;
#if !USE_PIPE

		if (serialize) {
			sem_post(&writer_sem);
			sem_wait(&reader_sem);
		}

		fifo_window_exchange_reader(&window);

		if (window.len < 4) {
			if (done_flag)
				break;
			fifo_window_reader_wait(&window);
			continue;
		}

		ptr = fifo_window_peek_span(&window, &len);
		len /= sizeof(int);
		if (len > READER_BATCH)
			len = READER_BATCH;
		fifo_window_eat_span(&window, len*sizeof(int));
#else
		len = read(read_fd, reader_buffer, BUFFERSIZE);
		if (len == 0)
			break;

		len /= sizeof(int);
		ptr = reader_buffer;
#endif

		reader_inner_loop(ptr, len, &count,
				  &sum, xsubi);
	}
	printf("sum = 0x%08x\ncount = %lld\n", sum, count);
	return (void *)(intptr_t)sum;
}

#define WRITER_BATCH 256

static
void writer_normal_inner_loop(int *ptr, int len, uint64_t *count_ptr, unsigned short *xsubi)
{
	for (int i = 0; i < len; i++)
		ptr[i] = nrand48(xsubi);
	*count_ptr += len;
}

static
void writer_fast_inner_loop(int *ptr, int len, uint64_t *count_ptr, unsigned short *xsubi)
{
	for (int i = 0; i < len; i++)
		ptr[i] = 0;
	*count_ptr += len;
}

static
void (*writer_inner_loop)(int *ptr, int len, uint64_t *count_ptr,
			  unsigned short *xsubi) = writer_normal_inner_loop;

static
void *writer_thread(void *dummy)
{
	uint64_t count = 0;
	unsigned short xsubi[3];
	memset(xsubi, 0, sizeof(xsubi));

	printf("writer's pid is %d\n", gettid());
	if (setaffinity)
		move_to_cpu(1);

#if !USE_PIPE
	struct fifo_window window;
	if (serialize)
		sem_wait(&writer_sem);

	fifo_window_init_writer(fifo, &window, 4, WRITER_BATCH*sizeof(int)*2);
#endif
	while (count < 300000000ULL) {
		int *ptr;
		unsigned len;
#if !USE_PIPE
		fifo_window_exchange_writer(&window);

		if (serialize) {
			sem_post(&reader_sem);
			sem_wait(&writer_sem);
		}

		ptr = fifo_window_peek_span(&window, &len);
		len /= sizeof(int);
		if (len > WRITER_BATCH)
			len = WRITER_BATCH;
		fifo_window_eat_span(&window, len*sizeof(int));
#else
		len = BUFFERSIZE/sizeof(int);
		ptr = writer_buffer;
#endif

		writer_inner_loop(ptr, len, &count, xsubi);

#if USE_PIPE
		while (len > 0) {
			int rv = write(write_fd, writer_buffer, len);
			if (rv < 0) {
				if (errno == EINTR)
					continue;
				fatal_perror("write");
			}
			len -= rv;
		}
#endif
	}
#if !USE_PIPE
	done_flag = 1;
	fifo_window_exchange_writer(&window);
	if (serialize)
		sem_post(&reader_sem);
#else
	close(write_fd);
#endif
	fprintf(stderr, "writer count = %lld\n", count); 
	return 0;
}

static
char *usage_text =
	"Usage: %s [options]\n"
	"Benchmark shared memory fifo implementation.\n"
	"This binary has %s fifo implementation.\n"
	"  -a\tset affinity for dual- core or CPU machine\n"
	"  -s\tserialize processing for debugging\n"
	"  -f\tuse faster producer/consumer\n"
	"\n";

static
void usage(char **argv)
{
	fprintf(stderr, usage_text, argv[0], fifo_implementation_type);
}

int main(int argc, char **argv)
{
	int rv;
	int optchar;

	while ((optchar = getopt(argc, argv, "asf")) >= 0) {
		switch (optchar) {
		case 'a':
			setaffinity = 1;
			break;
		case 's':
			serialize = 1;
			break;
		case 'f':
			reader_inner_loop = reader_fast_inner_loop;
			writer_inner_loop = writer_fast_inner_loop;
			break;
		default:
			usage(argv);
			exit(1);
		}
	}

#if !USE_PIPE
	printf("FIFO_SIZE = %d\n", FIFO_SIZE);
	printf("sizeof(struct shm_fifo) = %d\n", sizeof(struct shm_fifo));

	if (serialize) {
		rv = sem_init(&reader_sem, 0, 0);
		if (rv)
			fatal_perror("sem_init(&reader_sem,...)");
		rv = sem_init(&writer_sem, 0, 0);
		if (rv)
			fatal_perror("sem_init(&writer_sem,...)");
	}

	rv = fifo_create(&fifo);
	if (rv)
		fatal_perror("fifo_create");

	if (serialize)
		sem_post(&writer_sem);
#else
	int pipes[2];
	rv = pipe(pipes);
	if (rv)
		fatal_perror("pipe");
	read_fd = pipes[0];
	write_fd = pipes[1];
#endif

	pthread_t reader, writer;
	rv = pthread_create(&reader, 0, reader_thread, 0);
	if (rv)
		fatal_perror("phread_create(&reader)");

	rv = pthread_create(&writer, 0, writer_thread, 0);
	if (rv)
		fatal_perror("pthread_create(&writer)");

	pthread_join(reader, 0);
	pthread_join(writer, 0);

#if !USE_PIPE
	printf("fifo_writer_exchange_count = %d\n", fifo_writer_exchange_count);
	printf("fifo_writer_wake_count = %d\n", fifo_writer_wake_count);
	printf("fifo_reader_exchange_count = %d\n", fifo_reader_exchange_count);
	printf("fifo_reader_wake_count = %d\n", fifo_reader_wake_count);
#endif

	return 0;
}
