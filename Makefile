.PHONY: all clean

CC = g++
CFLAGS = -std=c++17 -Wall -Wextra

all: screen-worms-server screen-worms-client

screen-worms-server: server.cpp
	$(CC) $(CFLAGS) -o $@ $^

screen-worms-client: client.cpp
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm screen-worms-server screen-worms-client