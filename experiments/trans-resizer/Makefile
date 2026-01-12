# Trans-resizer experiment Makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

trans_resizer: trans_resizer.c bitwriter.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f trans_resizer

.PHONY: clean
