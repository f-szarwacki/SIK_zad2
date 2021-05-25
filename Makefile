.PHONY: all clean

CC = g++
CFLAGS = -Wall -Wextra

all: screen-worms-server screen-worms-client

screen-worms-server: server.cpp utils.h crc_table.h
	$(CC) $(CFLAGS) -o $@ $^

screen-worms-client: client.cpp utils.h crc_table.h
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm screen-worms-server screen-worms-client