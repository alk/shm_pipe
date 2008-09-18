#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "fifo.h"

static
struct futex_fifo *fifo;

#define SETAFFINITY 0
#define SERIALIZE 0

#if SERIALIZE
static
sem_t reader_sem;
static
sem_t writer_sem;
#endif

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

#define READER_BATCH 512

static
void *reader_thread(void *dummy)
{
	struct fifo_window window;
	int sum;

	printf("reader's pid is %d\n", gettid());
#if SETAFFINITY
	move_to_cpu(0);
#endif

#if SERIALIZE
	sem_wait(&reader_sem);
#endif

	fifo_window_init_reader(fifo, &window);
	while (!done_flag) {
		int *ptr;
		unsigned len, i;

#if SERIALIZE
		sem_post(&writer_sem);
		sem_wait(&reader_sem);
#endif

		fifo_window_exchange_reader(&window);

		if (window.len < 4) {
			fifo_window_reader_wait(&window);
			continue;
		}

		ptr = fifo_window_peek_span(&window, &len);
		len /= sizeof(int);
		if (len > READER_BATCH)
			len = READER_BATCH;
		fifo_window_eat_span(&window, len*sizeof(int));

		for (i = 0; i < len; i++)
			sum += ptr[i] % 0x3461d349;
	}
	return (void *)(intptr_t)sum;
}

#define WRITER_BATCH 256

static
void *writer_thread(void *dummy)
{
	struct fifo_window window;
	unsigned count = 0;

	printf("writer's pid is %d\n", gettid());
#if SETAFFINITY
	move_to_cpu(1);
#endif

#if SERIALIZE
	sem_wait(&writer_sem);
#endif

	fifo_window_init_writer(fifo, &window);
	while (count < 2000000000U) {
		int *ptr;
		unsigned len, i;
		fifo_window_exchange_writer(&window);

#if SERIALIZE
		sem_post(&reader_sem);
		sem_wait(&writer_sem);
#endif

		if (window.len < 4) {
			fifo_window_writer_wait(&window);
			continue;
		}

		ptr = fifo_window_peek_span(&window, &len);
		len /= sizeof(int);
		if (len > WRITER_BATCH)
			len = WRITER_BATCH;
		fifo_window_eat_span(&window, len*sizeof(int));
		for (i=0;i<len;i++,count++)
			ptr[i] = count % 0x5ffefefe;
	}
	done_flag = 1;
	fifo_window_exchange_writer(&window);
#if SERIALIZE
	sem_post(&reader_sem);
#endif
	return 0;
}

int main()
{
	int rv;
	pthread_t reader, writer;

#if SERIALIZE
	rv = sem_init(&reader_sem, 0, 0);
	if (rv)
		fatal_perror("sem_init(&reader_sem,...)");
	rv = sem_init(&writer_sem, 0, 0);
	if (rv)
		fatal_perror("sem_init(&writer_sem,...)");
#endif

	rv = fifo_create(&fifo);
	if (rv)
		fatal_perror("fifo_create");

	rv = pthread_create(&reader, 0, reader_thread, 0);
	if (rv)
		fatal_perror("phread_create(&reader)");

	rv = pthread_create(&writer, 0, writer_thread, 0);
	if (rv)
		fatal_perror("pthread_create(&writer)");

#if SERIALIZE
	sem_post(&writer_sem);
#endif

	pthread_join(reader, 0);
	pthread_join(writer, 0);

	printf("fifo_writer_exchange_count = %d\n", fifo_writer_exchange_count);
	printf("fifo_writer_wake_count = %d\n", fifo_writer_wake_count);
	printf("fifo_reader_exchange_count = %d\n", fifo_reader_exchange_count);
	printf("fifo_reader_wake_count = %d\n", fifo_reader_wake_count);

	return 0;
}
