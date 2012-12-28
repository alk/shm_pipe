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

#include "fifo.h"

#ifdef JUST_MEMCPY
#define nrand48(dummy) 0
#endif

#define READER_BATCH 1024
#define WRITER_BATCH 1024
#define SEND_WORDS 2000000000U

static
struct shm_fifo *fifo;

static
int setaffinity;
static
int serialize;

#define SERIALIZE 0

static
sem_t reader_sem;
static
sem_t writer_sem;

int done_flag;

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


static
void *reader_thread(void *dummy)
{
	struct fifo_window window;
	unsigned long long count=0;
	int sum=0;
	unsigned short xsubi[3];
	memset(xsubi, 0, sizeof(xsubi));

	printf("reader's pid is %d\n", gettid());
	if (setaffinity)
		move_to_cpu(0);

	if (serialize)
		sem_wait(&reader_sem);

	fifo_window_init_reader(fifo, &window, 0, READER_BATCH*sizeof(int)*2);
	while (1) {
		int *ptr;
		unsigned len, i;

		if (serialize) {
			sem_post(&writer_sem);
			sem_wait(&reader_sem);
		}

		fifo_window_exchange_reader(&window);

		if (window.len == 0) {
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

		for (i = 0; i < len; i++)
			sum |= *ptr++ ^ nrand48(xsubi);
		count += i;
	}
	printf("sum = 0x%08x\ncount = %lld\n", sum, count);
	return (void *)(intptr_t)sum;
}


static
void *writer_thread(void *dummy)
{
	struct fifo_window window;
	unsigned count = 0;
	unsigned short xsubi[3];
	memset(xsubi, 0, sizeof(xsubi));

	printf("writer's pid is %d\n", gettid());
	if (setaffinity)
		move_to_cpu(1);

	if (serialize)
		sem_wait(&writer_sem);

	fifo_window_init_writer(fifo, &window, 4, WRITER_BATCH*sizeof(int)*2);
	while (count < SEND_WORDS) {
		int *ptr;
		unsigned len, i;
		fifo_window_exchange_writer(&window);

		if (serialize) {
			sem_post(&reader_sem);
			sem_wait(&writer_sem);
		}

		ptr = fifo_window_peek_span(&window, &len);
		len /= sizeof(int);
		if (len > WRITER_BATCH)
			len = WRITER_BATCH;
		count += len;
		if (count > SEND_WORDS) {
			count -= len;
			len = SEND_WORDS - count;
			count += len;
		}
		fifo_window_eat_span(&window, len*sizeof(int));
		for (i=0;i<len;i++)
			*ptr++ = nrand48(xsubi);
	}
	fprintf(stderr, "writer count %u\n", count);
	done_flag = 1;
	fifo_window_exchange_writer(&window);
	if (serialize)
		sem_post(&reader_sem);
	return 0;
}

static
char *usage_text =
	"Usage: %s [options]\n"
	"Benchmark shared memory fifo implementation.\n"
	"This binary has %s fifo implementation.\n"
	"  -a\tset affinity for dual- core or CPU machine\n"
	"  -s\tserialize processing for debugging\n"
	"\n";

static
void usage(char **argv)
{
	fprintf(stderr, usage_text, argv[0], fifo_implementation_type);
}

int main(int argc, char **argv)
{
	int rv;
	pthread_t reader, writer;
	int optchar;

	while ((optchar = getopt(argc, argv, "as")) >= 0) {
		switch (optchar) {
		case 'a':
			setaffinity = 1;
			break;
		case 's':
			serialize = 1;
			break;
		default:
			usage(argv);
			exit(1);
		}
	}

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

	rv = pthread_create(&reader, 0, reader_thread, 0);
	if (rv)
		fatal_perror("phread_create(&reader)");

	rv = pthread_create(&writer, 0, writer_thread, 0);
	if (rv)
		fatal_perror("pthread_create(&writer)");

	if (serialize)
		sem_post(&writer_sem);

	pthread_join(reader, 0);
	pthread_join(writer, 0);

	printf("fifo_writer_exchange_count = %d\n", fifo_writer_exchange_count);
	printf("fifo_writer_wake_count = %d\n", fifo_writer_wake_count);
	printf("fifo_reader_exchange_count = %d\n", fifo_reader_exchange_count);
	printf("fifo_reader_wake_count = %d\n", fifo_reader_wake_count);

	return 0;
}
