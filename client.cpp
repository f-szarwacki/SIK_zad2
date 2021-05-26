#include <unistd.h>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#include <memory>
#include <cstdlib>
#include <netdb.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <csignal>

#include "utils.h"

#define POLL_ARR_LEN_CLIENT 3
#define SERVER_MESSAGING_INTERVAL_MILLISECONDS 30

#define LEFT_KEY_DOWN_LEN 14
#define LEFT_KEY_UP_LEN 12
#define RIGHT_KEY_DOWN_LEN 15
#define RIGHT_KEY_UP_LEN 13

uint send_to_socket(int socket, char *buffer, uint buffer_size, int flags) {
    uint bytes_sent = 0;
    int rc;
    while ((rc = send(socket, buffer + bytes_sent, buffer_size - bytes_sent, flags)) > 0) {
        bytes_sent += rc;
    }
    if (rc == -1) {
        error("Sending to socket.", NONCRITICAL);
    }
    buffer[buffer_size] = 0;
    return bytes_sent;
}

class Client {
    std::string game_server;
    std::string player_name;
    std::string server_port;
    std::string gui_server;
    std::string gui_server_port;

    uint64_t session_id;
    uint8_t turn_direction;
    uint32_t next_expected_event_no;
    uint32_t current_game_id;
    uint32_t maxx, maxy;

    bool left_key_down, right_key_down;

    int server_socket;
    int gui_server_socket;
    uint server_socket_poll_ind;
    uint gui_server_socket_poll_ind;
    uint timer_poll_ind;

    char buffer[BUFFER_SIZE];
    char gui_buffer[BUFFER_SIZE];

    struct pollfd poll_arr[POLL_ARR_LEN_CLIENT];
    std::vector<Player> players;
    std::vector<std::string> player_names;

    int add_timer_to_poll(uint milliseconds);
    int add_to_poll(int fd, int events = POLLIN);
    uint prepare_message_to_server(char *buffer);
    void interpret_message_from_gui_server(uint len);
    void interpret_message_from_server(uint message_len);
    std::string get_player_name(uint player_no);

public:
    Client(std::string game_server, std::string player_name, std::string port_number, std::string gui_server, std::string gui_port_number);

    [[noreturn]] void run();
};

Client::Client(std::string game_server, std::string player_name, std::string port_number, std::string gui_server,
               std::string gui_port_number) : game_server{std::move(game_server)}, player_name{std::move(player_name)}, server_port{std::move(port_number)},
                                              gui_server{std::move(gui_server)}, gui_server_port{std::move(gui_port_number)} {

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    session_id = tv.tv_sec*(uint64_t) NANOSECONDS_IN_MILLISECOND + tv.tv_usec;

    for (int i = 0; i < POLL_ARR_LEN_CLIENT; ++i) {
        poll_arr[i].fd = -1;
        poll_arr[i].revents = 0;
    }

    turn_direction = STRAIGHT;
    next_expected_event_no = 0;
    left_key_down = false;
    right_key_down = false;

    timer_poll_ind = add_timer_to_poll(SERVER_MESSAGING_INTERVAL_MILLISECONDS);
}

[[noreturn]] void Client::run() {
    int rc;
    struct addrinfo addr_hints, *addr_result;

    // UDP communication with game server
    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_flags = 0;
    addr_hints.ai_family = AF_UNSPEC;
    addr_hints.ai_socktype = SOCK_DGRAM;
    addr_hints.ai_protocol = 0;

    rc = getaddrinfo(game_server.c_str(), server_port.c_str(), &addr_hints, &addr_result);
    if (rc != 0) {
        error(gai_strerror(rc), CRITICAL);
    }

    // Resolves address and tries possible addresses until one connects or all fail.
    struct addrinfo *it = addr_result;
    do {
        server_socket = socket(it->ai_family, SOCK_DGRAM, 0);
        if (server_socket < 0) {
            error("Making socket.", CRITICAL);
        }
        rc = connect(server_socket, addr_result->ai_addr, addr_result->ai_addrlen);
        if (rc == 0) {
            break;
        }

        close(server_socket);
        it = it->ai_next;
    } while (it != nullptr);

    if (rc < 0) {
        error("Connecting to game server.", CRITICAL);
    }

    if(fcntl(server_socket, F_SETFL, fcntl(server_socket, F_GETFL) | O_NONBLOCK) < 0) {
        error("Setting up socket.", CRITICAL);
    }

    freeaddrinfo(addr_result);
    server_socket_poll_ind = add_to_poll(server_socket);

    // TCP connection with GUI server
    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_flags = 0;
    addr_hints.ai_family = AF_UNSPEC;
    addr_hints.ai_socktype = SOCK_STREAM;
    addr_hints.ai_protocol = 0;

    // Resolves address and tries possible addresses until one connects or all fail.
    rc = getaddrinfo(gui_server.c_str(), gui_server_port.c_str(), &addr_hints, &addr_result);
    if (rc != 0) {
        error("Resolving address.", CRITICAL);
    }

    it = addr_result;
    do {
        gui_server_socket = socket(it->ai_family, SOCK_STREAM, 0);
        if (gui_server_socket < 0) {
            error("Making socket.", CRITICAL);
        }
        rc = connect(gui_server_socket, addr_result->ai_addr, addr_result->ai_addrlen);
        if (rc == 0) {
            break;
        }

        close(gui_server_socket);
        it = it->ai_next;
    } while (it != nullptr);

    if (rc < 0) {
        error("Connecting to GUI server.", CRITICAL);
    }

    // Disable Nagle's algorithm.
    int yes = 1;
    rc = setsockopt(gui_server_socket,IPPROTO_TCP, TCP_NODELAY, (char *) &yes, sizeof(int));
    if (rc < 0) {
        error("Setting socket options.", CRITICAL);
    }

    if(fcntl(gui_server_socket, F_SETFL, fcntl(gui_server_socket, F_GETFL) | O_NONBLOCK) < 0) {
        error("Setting up socket.", CRITICAL);
    }

    freeaddrinfo(addr_result);
    gui_server_socket_poll_ind = add_to_poll(gui_server_socket);

    // Main loop.
    while(true) {
        for (auto & i : poll_arr) {
            i.revents = 0;
        }

        rc = poll(poll_arr, POLL_ARR_LEN_CLIENT, POLL_TIMEOUT);
        if (rc < 0) {
            error("Poll error.", NONCRITICAL);
        }

        if (rc > 0) {
            for (uint i = 0; i < POLL_ARR_LEN_CLIENT; ++i) {
                if (poll_arr[i].revents != 0) {
                    if (i == server_socket_poll_ind) {
                        // Message from game server.
                        while ((rc = recv(server_socket, buffer, BUFFER_SIZE, 0)) > 0) {
                            interpret_message_from_server(rc);
                        }
                        if (rc < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
                            error("Receiving from server.", CRITICAL);
                        }
                    } else if (i == gui_server_socket_poll_ind) {
                        // Message from GUI server.
                        uint bytes_received = 0;
                        while ((rc = recv(gui_server_socket, gui_buffer + bytes_received, BUFFER_SIZE - bytes_received,
                                          0)) > 0) {
                            bytes_received += rc;
                        }
                        if (rc < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
                            error("Receiving from GUI server.", CRITICAL);
                        }
                        interpret_message_from_gui_server(bytes_received);
                    } else if (i == timer_poll_ind) {
                        // New message to server should be sent.
                        rc = read(poll_arr[timer_poll_ind].fd, buffer, BUFFER_SIZE);
                        uint len = prepare_message_to_server(buffer);
                        rc = send(server_socket, buffer, len, 0);

                        if (rc < 0) {
                            error("Sending to server.", NONCRITICAL);
                        }
                    }
                }
            }
        }
    }
}

// Makes a timer and adds its file descriptor to poll_arr.
int Client::add_timer_to_poll(uint milliseconds) {
    int ind = 1;
    while (poll_arr[++ind].fd >= 0);
    int round_timer_fd = timerfd_create(CLOCK_REALTIME, 0);
    struct timespec time_spec {milliseconds / MILLISECONDS_IN_SECOND, (milliseconds %
                                                                       MILLISECONDS_IN_SECOND) * NANOSECONDS_IN_MILLISECOND};
    struct itimerspec timer_spec {time_spec, time_spec};
    timerfd_settime(round_timer_fd, 0, &timer_spec, nullptr);
    poll_arr[ind].fd = round_timer_fd;
    poll_arr[ind].events = POLLIN;
    poll_arr[ind].revents = 0;
    return ind;
}

int Client::add_to_poll(int fd, int events) {
    int ind = -1;
    while (poll_arr[++ind].fd >= 0);
    poll_arr[ind].fd = fd;
    poll_arr[ind].events = events;
    poll_arr[ind].revents = 0;
    return ind;
}

// Put data about client to buffer.
uint Client::prepare_message_to_server(char *buffer) {
    *reinterpret_cast<uint64_t*> (buffer) = htobe64(session_id);
    buffer += sizeof (uint64_t);

    *reinterpret_cast<uint8_t*> (buffer) = turn_direction;
    buffer += sizeof (uint8_t);

    *reinterpret_cast<uint32_t*> (buffer) = htobe32(next_expected_event_no);
    buffer += sizeof (uint32_t);

    for (uint i = 0; i < player_name.size(); ++i){
        *reinterpret_cast<uint8_t*> (buffer) = *reinterpret_cast<uint8_t*>(&player_name[i]);
        buffer += sizeof (uint8_t);
    }

    return (sizeof (uint64_t) + sizeof (uint8_t) + sizeof (uint32_t) + player_name.size());
}

// Parses, interprets and reacts to messages from GUI server.
void Client::interpret_message_from_gui_server(uint len) {
    // Assumes message is in gui_buffer.
    uint bytes_read = 0;
    while (bytes_read < len) {
        if (strncmp(gui_buffer + bytes_read, "LEFT_KEY_DOWN\n", LEFT_KEY_DOWN_LEN) == 0) {
            left_key_down = true;
            turn_direction = LEFT;
            bytes_read += LEFT_KEY_DOWN_LEN;
        } else if (strncmp(gui_buffer + bytes_read, "LEFT_KEY_UP\n", LEFT_KEY_UP_LEN) == 0) {
            if (!left_key_down) {
                error("Incorrect message from GUI server.", NONCRITICAL);
            } else {
                left_key_down = false;
                if (right_key_down) {
                    turn_direction = RIGHT;
                } else {
                    turn_direction = STRAIGHT;
                }
            }
            bytes_read += LEFT_KEY_UP_LEN;
        } else if (strncmp(gui_buffer + bytes_read, "RIGHT_KEY_DOWN\n", RIGHT_KEY_DOWN_LEN) == 0) {
            right_key_down = true;
            turn_direction = RIGHT;
            bytes_read += RIGHT_KEY_DOWN_LEN;
        } else if (strncmp(gui_buffer + bytes_read, "RIGHT_KEY_UP\n", RIGHT_KEY_UP_LEN) == 0) {
            if (!right_key_down) {
                error("Incorrect message from GUI server.", NONCRITICAL);
            } else {
                right_key_down = false;
                if (left_key_down) {
                    turn_direction = LEFT;
                } else {
                    turn_direction = STRAIGHT;
                }
            }
            bytes_read += RIGHT_KEY_UP_LEN;
        } else {
            // Ignore message.
            bytes_read = len;
        }
    }
}

// Parses, interprets and reacts to messages from game server.
void Client::interpret_message_from_server(uint message_len) {
    // Assumes message is in buffer.
    char *buffer_ = buffer;
    uint32_t game_id = ntohl(*reinterpret_cast<uint32_t*>(buffer_));
    buffer_ += sizeof (uint32_t);
    while ((buffer_ - buffer) < message_len) {
        uint32_t len = ntohl(*reinterpret_cast<uint32_t*>(buffer_));
        buffer_ += sizeof (uint32_t);

        uint32_t crc32_from_message = ntohl(*reinterpret_cast<uint32_t*>(buffer_ + len));
        uint32_t crc32_counted = crc32(reinterpret_cast<uint8_t *>(buffer_ - sizeof(uint32_t)), len + sizeof(uint32_t));

        if (crc32_from_message != crc32_counted) {
            error("Message with incorrect CRC-32 code.", NONCRITICAL);
            break;
        } else {
            uint32_t event_no = ntohl(*reinterpret_cast<uint32_t *>(buffer_));
            buffer_ += sizeof(uint32_t);

            // Check if we got event we wanted, if yes we want the next one, else we ignore this message and wait
            // for correct one.
            if (event_no == next_expected_event_no) {
                next_expected_event_no++;
            } else {
                break;
            }

            uint8_t event_type = *reinterpret_cast<uint8_t *>(buffer_);
            buffer_ += sizeof(uint8_t);

            if (game_id != current_game_id && event_type != NEW_GAME_TYPE) {
                // Ignore messages with different game_id than current unless it is a NEW_GAME event.
                break;
            }

            uint bytes_written_to_gui_buffer = 0;
            if (event_type == NEW_GAME_TYPE) {
                player_names.clear();
                current_game_id = game_id;

                maxx = ntohl(*reinterpret_cast<uint32_t *>(buffer_));
                buffer_ += sizeof(uint32_t);

                maxy = ntohl(*reinterpret_cast<uint32_t *>(buffer_));
                buffer_ += sizeof(uint32_t);

                uint player_names_bytes_read = 0;
                do {
                    uint player_name_len = strlen(buffer_) + 1;
                    for (int i = 0; i < MAX_PLAYER_NAME_LEN; ++i) {
                        if (buffer_[i] == 0) {
                            break;
                        }
                        if (buffer_[i] < MIN_PLAYER_NAME_CHAR || buffer_[i] > MAX_PLAYER_NAME_CHAR) {
                            // Incorrect name - error.
                            error("Incorrect player name received from game server.", CRITICAL);
                        }
                    }
                    player_names_bytes_read += player_name_len;
                    player_names.emplace_back(buffer_);
                    buffer_ += player_name_len;
                } while (player_names_bytes_read < (len - NEW_GAME_BASE_LEN));

                bytes_written_to_gui_buffer += sprintf(gui_buffer, "NEW_GAME %u %u", maxx, maxy);
                for (uint i = 0; i < player_names.size(); ++i) {
                    bytes_written_to_gui_buffer += sprintf(gui_buffer + bytes_written_to_gui_buffer, " %s",
                                                           player_names[i].c_str());
                }
                bytes_written_to_gui_buffer += sprintf(gui_buffer + bytes_written_to_gui_buffer, "\n");
                send_to_socket(gui_server_socket, gui_buffer, bytes_written_to_gui_buffer, 0);

            } else if (event_type == PIXEL_TYPE) {
                uint8_t player_no = *reinterpret_cast<uint8_t *>(buffer_);
                buffer_ += sizeof(uint8_t);

                if (player_no >= player_names.size()) {
                    error("Incorrect player number received from game server.", CRITICAL);
                }

                uint32_t x = ntohl(*reinterpret_cast<uint32_t *>(buffer_));
                buffer_ += sizeof(uint32_t);

                if (x >= maxx) {
                    error("Incorrect x-coordinate received from game server.", CRITICAL);
                }

                uint32_t y = ntohl(*reinterpret_cast<uint32_t *>(buffer_));
                buffer_ += sizeof(uint32_t);

                if (y >= maxy) {
                    error("Incorrect y-coordinate received from game server.", CRITICAL);
                }

                bytes_written_to_gui_buffer += sprintf(gui_buffer, "PIXEL %u %u %s\n", x, y, get_player_name(player_no).c_str());
                send_to_socket(gui_server_socket, gui_buffer, bytes_written_to_gui_buffer, 0);

            } else if (event_type == PLAYER_ELIMINATED_TYPE) {
                uint8_t player_no = *reinterpret_cast<uint8_t *>(buffer_);
                buffer_ += sizeof(uint8_t);

                if (player_no >= player_names.size()) {
                    error("Incorrect player number.", CRITICAL);
                }

                bytes_written_to_gui_buffer += sprintf(gui_buffer, "PLAYER_ELIMINATED %s\n", get_player_name(player_no).c_str());
                send_to_socket(gui_server_socket, gui_buffer, bytes_written_to_gui_buffer, 0);
            } else if (event_type == GAME_OVER_TYPE) {
                next_expected_event_no = 0;
            } else {
                // Ignore message.
                buffer_ += len - sizeof (uint32_t) - sizeof (uint8_t);
            }

            // crc32
            buffer_ += sizeof(uint32_t);
        }
    }
}

std::string Client::get_player_name(uint player_no) {
    return player_names[player_no];
}

void sig_pipe_handler([[maybe_unused]] int signum) {
    error("Connection with GUI server broken.", CRITICAL);
}

int main(int argc, char *argv[]){
    // Handling SIGPIPE signal when TCP connection is broken.
    struct sigaction pipe_action;
    pipe_action.sa_handler = sig_pipe_handler;
    sigemptyset(&pipe_action.sa_mask);
    sigaddset(&pipe_action.sa_mask, SIGPIPE);
    pipe_action.sa_flags = 0;
    sigaction(SIGINT,&pipe_action, nullptr);

    // Default arguments.
    std::string game_server;
    std::string player_name;
    std::string port_number = "2021";
    std::string gui_server = "localhost";
    std::string gui_port_number = "20211";

    if (argc < 2) {
        error("Too few arguments.", CRITICAL);
    }

    game_server = std::string(argv[1]);
    int c;
    opterr = 0;
    try {
        while ((c = getopt(argc, argv, "n:p:i:r:")) != -1)
            switch (c) {
                case 'n':
                    // Option -n without argument is treated as empty player_name.
                    if (optarg != nullptr) {
                        player_name = std::string(optarg);
                    }
                    if (player_name.size() > MAX_PLAYER_NAME_LEN) {
                        error("Player name too long.", CRITICAL);
                    }
                    for (uint i = 0; i < player_name.size(); ++i) {
                        if (player_name[i] < MIN_PLAYER_NAME_CHAR || player_name[i] > MAX_PLAYER_NAME_CHAR) {
                            error("Incorrect character in player name.", CRITICAL);
                        }
                    }
                    break;
                case 'p':
                    port_number = std::string(optarg);
                    if (stoul(port_number) < 1 || stoul(port_number) > 65535) {
                        error("Incorrect port number.", CRITICAL);
                    }
                    break;
                case 'i':
                    gui_server = std::string(optarg);
                    break;
                case 'r':
                    gui_port_number = std::string(optarg);
                    if (stoul(gui_port_number) < 1 || stoul(gui_port_number) > 65535) {
                        error("Incorrect GUI port number.", CRITICAL);
                    }
                    break;
                case '?':
                    // Option -n without argument is treated as empty player_name.
                    if (optopt != 'n') {
                        error("Expected option argument.", CRITICAL);
                    }
                    break;
                default:
                    error("Unexpected argument.", CRITICAL);
                    break;
            }
    } catch (...) {
        error("Incorrect arguments.", CRITICAL);
    }

    if (argc - optind < 1) {
        error("Too few arguments.", CRITICAL);
    }

    Client client(game_server, player_name, port_number, gui_server, gui_port_number);
    client.run();
}