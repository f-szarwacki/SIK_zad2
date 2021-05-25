#ifndef SIKDUZE_UTILS_H
#define SIKDUZE_UTILS_H
#include <cstdint>
#include <string>

#define CRITICAL true
#define NONCRITICAL false

#define RAND_MULTIPLIER 279410273
#define RAND_MOD 4294967291
#define BUFFER_SIZE 548
#define MILLISECONDS_IN_SECOND 1000
#define NANOSECONDS_IN_MILLISECOND 1000000
#define DEGREES_TO_RADIANS (M_PI/180)

#define MAX_PLAYER_NAME_LEN 20
#define MAX_NUM_OF_PLAYERS 25

#define POLL_ARR_LEN (MAX_NUM_OF_PLAYERS + 2)
#define POLL_TIMEOUT 0
#define ALL 999999

#define NEW_GAME_TYPE 0
#define PIXEL_TYPE 1
#define PLAYER_ELIMINATED_TYPE 2
#define GAME_OVER_TYPE 3

#define NEW_GAME_BASE_LEN 13
#define PIXEL_LEN 14
#define PLAYER_ELIMINATED_LEN 6
#define GAME_OVER_LEN 5

#define EATEN true
#define NOT_EATEN false

#define PLAYER_TIMEOUT_MILLISECONDS (2*MILLISECONDS_IN_SECOND)

#define OBSERVER 0
#define WAITING 1
#define WILLING_TO_PLAY 2
#define PLAYING 3
#define ELIMINATED 4
#define DISCONNECTED 5
#define ZOMBIE 6

#define STATUS_COUNT 7

#define STRAIGHT 0
#define LEFT 1
#define RIGHT 2

#include "crc_table.h"

typedef unsigned int uint;
typedef bool pixel;

struct Player {
    double x, y;
    uint direction;
    int turn_direction;
    std::string name;
    struct sockaddr address;
    uint poll_arr_index;
    uint status;
};

void error(const std::string& log, bool critical) {
    std::cerr << log << "\n";
    if (critical) {
        exit(EXIT_FAILURE);
    }
}

uint32_t crc32(uint8_t *data, uint len_of_data) {
    uint32_t crc = 0xFFFFFFFF;
    uint32_t lookup_ind = 0;
    for (uint i = 0; i < len_of_data; ++i) {
        lookup_ind = (crc ^ data[i]) & 0xFF;
        crc = (crc >> 8) ^ crc_table[lookup_ind];
    }
    crc = crc ^ 0xFFFFFFFF;
    return crc;
}

#endif //SIKDUZE_UTILS_H
