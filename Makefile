
#CFLAGS=-O0 -Wall -pedantic -ggdb3 -std=gnu99
CFLAGS=-m32 -O3 -march=native -fomit-frame-pointer -DAO_USE_PENTIUM4_INSTRS -std=gnu99 -DFIFO_OVERRIDE -DJUST_MEMCPY
LINK=gcc -m32 -static

%.o : %.c
	gcc $(CFLAGS) -c -o $@ $<

%.s : %.c
	gcc $(CFLAGS) -fverbose-asm -S -o $@ $<

all : main_futex main_eventfd main_efd_nonblock main_emulation main_pipe

fifo_eventfd.o : fifo.c fifo.h
	gcc -DUSE_EVENTFD=1 $(CFLAGS) -c -o $@ $<

fifo_efd_nonblock.o : fifo.c fifo.h
	gcc -DUSE_EVENTFD=1 -DEVENTFD_NONBLOCKING=1 $(CFLAGS) -c -o $@ $<

fifo_eventfd_emulation.o : fifo.c fifo.h
	gcc -DUSE_EVENTFD=1 -DUSE_EVENTFD_EMULATION=1 $(CFLAGS) -c -o $@ $<

fifo_eventfd.s : fifo.c fifo.h
	gcc -DUSE_EVENTFD=1 $(CFLAGS) -fverbose-asm -S -o $@ $<

fifo_efd_nonblock.s : fifo.c fifo.h
	gcc -DUSE_EVENTFD=1 -DEVENTFD_NONBLOCKING=1 $(CFLAGS) -fverbose-asm -S -o $@ $<

fifo_eventfd_emulation.s : fifo.c fifo.h
	gcc -DUSE_EVENTFD=1 -DUSE_EVENTFD_EMULATION=1 $(CFLAGS) -fverbose-asm -S -o $@ $<

clean:
	rm -f *.o main_futex main_eventfd main_emulation main_pipe main_efd_nonblock

main.o fifo.o: fifo.h

main_futex : main.o fifo.o
	$(LINK) -o $@ $^ -lpthread

main_eventfd: main.o fifo_eventfd.o
	$(LINK) -o $@ $^ -lpthread

main_efd_nonblock: main.o fifo_efd_nonblock.o
	$(LINK) -o $@ $^ -lpthread

main_emulation: main.o fifo_eventfd_emulation.o
	$(LINK) -o $@ $^ -lpthread

main_pipe: main_pipe.o
	$(LINK) -o $@ $^ -lpthread
