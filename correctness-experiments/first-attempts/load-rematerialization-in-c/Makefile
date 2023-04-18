CC=gcc
FLAGS =-g -O3 -Wall -Wextra -pthread -pedantic # -Werror -std=c++17 -fsanitize=address -fsanitize=leak -fsanitize=undefined

all:
	$(CC) ReaderWriterTest.c $(FLAGS) -o run

clean:
	rm -f run