CC = gcc

CFLAGS = -g -std=c17 -D_POSIX_C_SOURCE=200809L \
		 -Wall -Werror -Wextra \
		 -Wcast-align -Wconversion -Wfloat-equal -Wformat=2 -Wnull-dereference -Wshadow -Wsign-conversion -Wswitch-enum -Wundef -Wunreachable-code -Wunused \
 		 -fsanitize=undefined

ifneq ($(shell uname -s),Darwin) # if not MacOS
	CFLAGS += -fmax-errors=5
endif

all: cloudIST

cloudIST: main.c constants.h parser.o datacenter.o datacenter_utils.o resources.o filesystem.o
	$(CC) $(CFLAGS) $(SLEEP) -o cloudIST main.c parser.o datacenter.o datacenter_utils.o resources.o filesystem.o -lm

%.o: %.c %.h
	$(CC) $(CFLAGS) -c ${@:.o=.c}

run: cloudIST
	@./cloudIST

clean:
	rm -f *.o cloudIST

format:
	@which clang-format >/dev/null 2>&1 || echo "Please install clang-format to run this command"
	clang-format -i *.c *.h