
#CFLAGS=-O0 -Wall -pedantic -ggdb3 -std=gnu99
CFLAGS=-m32 -O3 -march=native -fomit-frame-pointer -DAO_USE_PENTIUM4_INSTRS -std=gnu99

%.o : %.c
	gcc $(CFLAGS) -c -o $@ $<

%.s : %.c
	gcc $(CFLAGS) -fverbose-asm -S -o $@ $<

all : main main_pipe

clean:
	rm -f main.o fifo.o main main_pipe main_pipe.o

main.o fifo.o: fifo.h

main : main.o fifo.o
	gcc -m32 -o $@ $^ -lpthread -static

main_pipe: main_pipe.o
	gcc -m32 -o $@ $^ -lpthread -static
