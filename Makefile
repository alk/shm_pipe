
OBJS=fifo.c
#CFLAGS=-O0 -Wall -pedantic -ggdb3 -std=gnu99
CFLAGS=-O2 -ggdb3 -DAO_USE_PENTIUM4_INSTRS -std=gnu99

%.o : %.c
	gcc $(CFLAGS) -c -o $@ $<

%.s : %.c
	gcc $(CFLAGS) -fverbose-asm -S -o $@ $<

all : main

clean:
	rm main.o fifo.o main

main.o fifo.o: fifo.h

main : main.o fifo.o
	gcc -o $@ $^ -lpthread
