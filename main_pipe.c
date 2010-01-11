#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

#ifdef JUST_MEMCPY
#define nrand48(dummy) 0
#endif

static
int setaffinity;

#define BUFFERSIZE 32768

int reader_buffer[BUFFERSIZE/sizeof(int)];
int writer_buffer[BUFFERSIZE/sizeof(int)];

int read_fd, write_fd;

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
	unsigned long long count=0;
	int sum=0;
	unsigned short xsubi[3];
	memset(xsubi, 0, sizeof(xsubi));

	printf("reader's pid is %d\n", gettid());
	if (setaffinity)
		move_to_cpu(0);

	while (1) {
		unsigned len, i;

		len = read(read_fd, reader_buffer, BUFFERSIZE);
		if (len == 0)
			break;

		len /= sizeof(int);

		for (i = 0; i < len; i++, count++)
			sum |= reader_buffer[i] ^  nrand48(xsubi);
	}
	printf("sum = 0x%08x\ncount = %lld\n", sum, count);
	return (void *)(intptr_t)sum;
}

static
void *writer_thread(void *dummy)
{
	unsigned count = 0;
	unsigned short xsubi[3];
	memset(xsubi, 0, sizeof(xsubi));

	printf("writer's pid is %d\n", gettid());
	if (setaffinity)
		move_to_cpu(1);

	while (count < 300000000U) {
		unsigned len, i;

		for (i=0;i<BUFFERSIZE/sizeof(int);i++,count++)
			writer_buffer[i] = nrand48(xsubi);
		i *= sizeof(int);
		while (i > 0) {
			len = write(write_fd, writer_buffer, i);
			if (len < 0) {
				if (errno == EINTR)
					continue;
				fatal_perror("write");
			}
			i -= len;
		}
		
	}
	close(write_fd);
	return 0;
}

static
char *usage_text =
	"Usage: %s [options]\n"
	"Benchmark in-kernel pipe fifo implementation.\n"
	"  -a\tset affinity for dual- core or CPU machine\n"
	"\n";

static
void usage(char **argv)
{
	fprintf(stderr, usage_text, argv[0]);
}

int main(int argc, char **argv)
{
	int rv;
	pthread_t reader, writer;
	int pipes[2];
	int optchar;

	while ((optchar = getopt(argc, argv, "a")) >= 0) {
		switch (optchar) {
		case 'a':
			setaffinity = 1;
			break;
		default:
			usage(argv);
			exit(1);
		}
	}

	rv = pipe(pipes);
	if (rv)
		fatal_perror("pipe");
	read_fd = pipes[0];
	write_fd = pipes[1];

	rv = pthread_create(&reader, 0, reader_thread, 0);
	if (rv)
		fatal_perror("phread_create(&reader)");

	rv = pthread_create(&writer, 0, writer_thread, 0);
	if (rv)
		fatal_perror("pthread_create(&writer)");

	pthread_join(reader, 0);
	pthread_join(writer, 0);

	return 0;
}
