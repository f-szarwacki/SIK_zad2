.PHONY: all clean

CC = g++
CFLAGS = -std=c++17 -Wall -Wextra

all: screen-worms-server screen-worms-client

screen-worms-server: server.cpp utils.h crc_table.h
	$(CC) $(CFLAGS) -o $@ server.cpp

screen-worms-client: client.cpp utils.h crc_table.h
	$(CC) $(CFLAGS) -o $@ client.cpp

clean:
	rm screen-worms-server screen-worms-client