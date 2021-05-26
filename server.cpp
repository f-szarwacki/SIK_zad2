#include <unistd.h>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <algorithm>

#include "utils.h"

// Compares two sockets - their addresses and ports.
bool check_if_sockets_equal(struct sockaddr *addr1, socklen_t addrlen1, struct sockaddr *addr2, socklen_t addrlen2) {
    if (addrlen1 == addrlen2 && addr1->sa_family == addr2->sa_family) {
        if (addr1->sa_family == AF_INET && memcmp(&(((struct sockaddr_in*)addr1)->sin_addr),
                &(((struct sockaddr_in*)addr2)->sin_addr),sizeof(struct in_addr)) == 0 &&
                (((struct sockaddr_in*)addr1)->sin_port == ((struct sockaddr_in*)addr2)->sin_port)) {
            //IPv4 sockets are equal
            return true;
        }

        if (addr1->sa_family == AF_INET6 && memcmp(&(((struct sockaddr_in6*)addr1)->sin6_addr),
                                                  &(((struct sockaddr_in6*)addr2)->sin6_addr),sizeof(struct in6_addr)) == 0 &&
            (((struct sockaddr_in6*)addr1)->sin6_port == ((struct sockaddr_in6*)addr2)->sin6_port)) {
            //IPv6 sockets are equal
            return true;
        }
    }
    return false;
}

void reset_timer(uint fd, uint milliseconds) {
    struct timespec time_spec {milliseconds / MILLISECONDS_IN_SECOND, (milliseconds %
                                                                       MILLISECONDS_IN_SECOND) * NANOSECONDS_IN_MILLISECOND};
    struct itimerspec timer_spec {time_spec, time_spec};
    timerfd_settime(fd, 0, &timer_spec, nullptr);
}

struct MessageFromClient {
    uint64_t session_id;
    uint8_t turn_direction;
    uint32_t next_expected_event_no;
    char player_name[MAX_PLAYER_NAME_LEN + 1];
};

// Parses message from buffer to structure MessageFromClient
MessageFromClient parse_message_from_client(char *buffer, uint len) {
    MessageFromClient return_value{};
    return_value.session_id = be64toh(*((uint64_t*) buffer));
    buffer += sizeof(uint64_t);
    return_value.turn_direction = *((uint8_t*) buffer);
    buffer += sizeof(uint8_t);
    return_value.next_expected_event_no = be32toh(*((uint32_t*)buffer));
    buffer += sizeof(uint32_t);
    strncpy(return_value.player_name, (char*) buffer, len - (sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint32_t)));
    return_value.player_name[len - (sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint32_t))] = 0;
    return return_value;
}

class Event {
public:
    uint32_t len; // This is len of event_* fields - as defined in communication protocol.
    uint32_t event_no;
    uint8_t event_type;
    Event(uint32_t len, uint32_t event_no, uint8_t event_type) : len{len}, event_no{event_no}, event_type{event_type} {}
    virtual ~Event() = default;
};

class NewGameEvent : public Event {
public:
    uint32_t max_x, max_y;
    char player_names[MAX_NUM_OF_PLAYERS][MAX_PLAYER_NAME_LEN + 1]; // accounting for '\0'
    NewGameEvent(uint32_t max_x, uint32_t max_y, std::vector<Player> players, uint32_t event_no)
    : Event(NEW_GAME_BASE_LEN, event_no, NEW_GAME_TYPE), max_x{max_x}, max_y{max_y} {
        for (int i = 0; i < MAX_NUM_OF_PLAYERS; ++i) {
            for (int j = 0; j < MAX_PLAYER_NAME_LEN + 1; ++j) {
                player_names[i][j] = 0;
            }
        }
        for (uint i = 0; i < players.size(); ++i) {
            auto player_name_len = players[i].name.size();
            if (players[i].status == WILLING_TO_PLAY) {
                strncpy(player_names[i], players[i].name.c_str(), player_name_len);
                player_names[i][player_name_len] = '\0';
                len += player_name_len + 1; // accounting for '\0'
            }
        }
    }
    ~NewGameEvent() = default;
};

class PixelEvent : public Event {
public:
    uint8_t player_number;
    uint32_t x, y;
    PixelEvent(uint8_t player_number, uint32_t x, uint32_t y, uint32_t event_no) : Event(PIXEL_LEN, event_no, PIXEL_TYPE),
    player_number{player_number}, x{x}, y{y} {}
};

class PlayerEliminatedEvent : public Event {
public:
    uint8_t player_number;
    PlayerEliminatedEvent(uint8_t player_number, uint32_t event_no) : Event(PLAYER_ELIMINATED_LEN, event_no,
                                                                            PLAYER_ELIMINATED_TYPE),
                                                                            player_number{player_number} {}
};

class GameOverEvent : public Event {
public:
    explicit GameOverEvent(uint32_t event_no) : Event(GAME_OVER_LEN, event_no, GAME_OVER_TYPE) {}
};

class Server {
    uint16_t port_number;
    uint32_t seed;
    uint32_t turning_speed;
    uint32_t rounds_per_sec;
    uint32_t width;
    uint32_t height;
    uint32_t maxx;
    uint32_t maxy;
    int sock;
    uint32_t next_rand;
    uint32_t game_id;
    pixel *game_board;
    std::vector<Player> players;
    uint32_t rand();
    char buffer[BUFFER_SIZE];
    struct pollfd poll_arr[POLL_ARR_LEN];
    uint round_timer_poll_ind, socket_poll_ind;
    std::vector<std::shared_ptr<Event>> events;
    uint next_event_no_to_be_sent;
    uint num_of_players_with_status[STATUS_COUNT];
    bool is_game_active;

    bool get_board(uint32_t x, uint32_t y);
    void set_board(uint32_t x, uint32_t y, bool value);
    void react_to_message_from_client(MessageFromClient message, struct sockaddr *client_address, socklen_t addrlen);
    int add_timer_to_poll(uint milliseconds);
    int add_to_poll(int fd, int events = POLLIN);
    void play_round();
    void send_events(uint event_no, uint to_whom = ALL);
    char *put_event_to_buffer(const std::shared_ptr<Event>& event_ptr, char *buffer_offset);
    void new_game();
    void eliminate_player(std::vector<Player>::iterator it);
    void eat_pixel(uint8_t player_no, uint x, uint y);
    void game_over();
    void delete_player(std::vector<Player>::iterator it);
    void change_status(std::vector<Player>::iterator it, uint new_status);
    void disconnect_player(std::vector<Player>::iterator it);
public:
    Server(uint16_t port_number, uint32_t seed, uint32_t turning_speed, uint32_t rounds_per_sec,
           uint32_t width, uint32_t height);

    [[noreturn]] void run();
};

// Makes a timer and adds its file descriptor to poll_arr.
int Server::add_timer_to_poll(uint milliseconds) {
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

Server::Server(uint16_t port_number, uint32_t seed, uint32_t turning_speed, uint32_t rounds_per_sec,
               uint32_t width, uint32_t height) : port_number{port_number},
               seed{seed},
               turning_speed{turning_speed},
               rounds_per_sec{rounds_per_sec},
               width{width},
               height{height},
               next_rand{seed} {
    try {
        game_board = new bool[height * width];
    } catch (std::bad_array_new_length& e) {
        error("Allocation", CRITICAL);
    }

    for (uint i = 0; i < height * width; ++i) {
        game_board[i] = NOT_EATEN;
    }

    for (auto & i : poll_arr) {
        i.revents = 0;
        i.fd = -1;
    }

    for (int i = 0; i < STATUS_COUNT; ++i) {
        num_of_players_with_status[i] = 0;
    }

    round_timer_poll_ind = add_timer_to_poll(1000 / rounds_per_sec);

    maxx = width;
    maxy = height;

    is_game_active = false;
}

[[noreturn]] void Server::run() {
    int rc;
    // making a socket
    sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock == -1) {
        error("Making socket.", CRITICAL);
    }

    struct sockaddr_in6 server;

    // binding the socket
    server.sin6_family = AF_INET6;
    server.sin6_addr = in6addr_any;
    server.sin6_port = htobe16(port_number);     // using port number given as parameter
    rc = bind(sock, (struct sockaddr *)&server, sizeof(server));
    if (rc == -1) {
        error("Error opening socket.", CRITICAL);
    }
    if(fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) < 0) {
        error("Setting up socket.", CRITICAL);
    }

    // Make sure that socket will handle IPv4 messages as well.
    int v = 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(int));

    socket_poll_ind = add_to_poll(sock);

    while(true) {
        for (int i = 0; i < POLL_ARR_LEN; ++i) {
            poll_arr[i].revents = 0;
        }

        rc = poll(poll_arr, POLL_ARR_LEN, POLL_TIMEOUT);
        if (rc < 0) {
            error("Poll error.", NONCRITICAL);
        }

        if (rc > 0) {
            for (uint i = 0; i < POLL_ARR_LEN; ++i) {
                if (poll_arr[i].revents != 0) {
                    if (i == socket_poll_ind) {
                        // new message at socket
                        struct sockaddr_in6 client;
                        socklen_t client_len = sizeof(struct sockaddr_in6);
                        while ((rc = recvfrom(poll_arr[i].fd, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client, &client_len)) > 0) {
                            // parse message from client
                            MessageFromClient mess = parse_message_from_client(buffer, rc);
                            // react to message from client
                            react_to_message_from_client(mess, (struct sockaddr*)&client, client_len);
                        }
                    } else if (i == round_timer_poll_ind) {
                        // new round to be played
                        rc = read(poll_arr[i].fd, buffer, BUFFER_SIZE);

                        if (is_game_active) {
                            play_round();
                            send_events(next_event_no_to_be_sent);
                            next_event_no_to_be_sent = events.size();
                        }
                    } else {
                        // player timeout
                        rc = read(poll_arr[i].fd, buffer, BUFFER_SIZE);
                        for (auto it = players.begin(); it != players.end(); ++it) {
                            if (it->poll_arr_index == i) {
                                disconnect_player(it);
                                break; // at most one player has this poll_arr_index
                            }
                        }
                    }
                }
            }
        }
    }
}

uint32_t Server::rand() {
    uint32_t temp = next_rand;
    next_rand = (uint32_t) (((uint64_t) next_rand * RAND_MULTIPLIER) % RAND_MOD);
    return temp;
}

bool Server::get_board(uint32_t x, uint32_t y) {
    if (x < width && y < height) {
        return game_board[x + width * y];
    } else return false;
}

void Server::set_board(uint32_t x, uint32_t y, bool value) {
    if (x < width && y < height) {
        game_board[x + width * y] = value;
    }
}

// Checks whether player is a new or existing player, correctness of their message and if it is correct it either adds
// a player or alters existing player turn_direction. At the end it sends a suitable response.
void Server::react_to_message_from_client(MessageFromClient message, struct sockaddr *client_address, socklen_t addrlen) {
    for (int i = 0; i < MAX_PLAYER_NAME_LEN; ++i) {
        if (message.player_name[i] == 0) {
            break;
        }
        if (message.player_name[i] < MIN_PLAYER_NAME_CHAR || message.player_name[i] > MAX_PLAYER_NAME_CHAR) {
            // Incorrect name - ignore.
            return;
        }
    }
    bool is_new_player = true;
    uint player_no = 0;
    for (auto it = players.begin(); it != players.end(); ++it) {
        if (it->status != DISCONNECTED && it->status != ZOMBIE) {
            if (check_if_sockets_equal(client_address, addrlen, (struct sockaddr *) (&it->address), it->addrlen)) {
                if (message.session_id == it->session_id) {
                    // Same player.
                    if (strncmp(it->name.c_str(), message.player_name, it->name.size()) == 0) {
                        // Same name.
                        is_new_player = false;
                        if (message.turn_direction > LEFT) { // Incorrect turn_direction - ignore.
                            return;
                        }
                        it->turn_direction = message.turn_direction;
                        reset_timer(poll_arr[it->poll_arr_index].fd, PLAYER_TIMEOUT_MILLISECONDS);

                        if (it->status == WAITING && message.turn_direction != STRAIGHT) {
                            change_status(it, WILLING_TO_PLAY);
                            if (num_of_players_with_status[WILLING_TO_PLAY] >= 2 && num_of_players_with_status[WAITING] == 0) {
                                // new game can start
                                new_game();
                            }
                        }
                        break; // At most one player can match.
                    } else {
                        // Different name - ignore.
                        return;
                    }
                } else if (message.session_id > it->session_id) {
                    // New player.
                    disconnect_player(it);
                    is_new_player = true;
                    break;
                } else {
                    // Ignore
                    return;
                }
            }
        }

        player_no++;
    }

    if (is_new_player) {
        if (players.size() - num_of_players_with_status[DISCONNECTED] - num_of_players_with_status[ZOMBIE] >= MAX_NUM_OF_PLAYERS) {
            // Too many players - ignore
            return;
        }
        for (auto it = players.begin(); it != players.end(); it++) {
            if (!it->name.empty() && strncmp(message.player_name, it->name.c_str(), it->name.size()) == 0) {
                // Same name as other player - ignore.
                return;
            }
        }
        Player player;
        player.name = message.player_name;
        player.session_id = message.session_id;
        if (is_game_active || player.name.empty()) {
            player.status = OBSERVER;
            num_of_players_with_status[OBSERVER]++;
        } else if (message.turn_direction == STRAIGHT){
            player.status = WAITING;
            num_of_players_with_status[WAITING]++;
        } else {
            player.status = WILLING_TO_PLAY;
            num_of_players_with_status[WILLING_TO_PLAY]++;
        }

        player.address = *(sockaddr_in6*)client_address;
        player.addrlen = addrlen;
        if (message.turn_direction > LEFT) { // Incorrect turn_direction - ignore.
            return;
        }
        player.turn_direction = message.turn_direction;

        player.poll_arr_index = add_timer_to_poll(PLAYER_TIMEOUT_MILLISECONDS);

        players.push_back(player);
        player_no = players.size() - 1;
    }

    send_events(message.next_expected_event_no, player_no);
}

int Server::add_to_poll(int fd, int events) {
    int ind = -1;
    while (poll_arr[++ind].fd >= 0);
    poll_arr[ind].fd = fd;
    poll_arr[ind].events = events;
    poll_arr[ind].revents = 0;
    return ind;
}

// Plays a single round.
void Server::play_round() {
    for (auto it = players.begin(); it != players.end(); ++it) {
        if (it->status == PLAYING || it->status == ZOMBIE) {
            // turn
            if (it->turn_direction == RIGHT) {
                it->direction += turning_speed;
                if (it->direction >= 360) it->direction -= 360;
            } else if (it->turn_direction == LEFT) {
                it->direction -= turning_speed;
                if (it->direction < 360) it->direction += 360;
            }

            // move
            int old_pixel_x = ceil(it->x);
            int old_pixel_y = ceil(it->y);

            it->x += cos((double) it->direction * DEGREES_TO_RADIANS);
            it->y += sin((double) it->direction * DEGREES_TO_RADIANS);

            int pixel_x = ceil(it->x);
            int pixel_y = ceil(it->y);

            if (old_pixel_x == pixel_x && old_pixel_y == pixel_y) {
                // worm didn't change its pixel
                continue;
            }

            if ((pixel_x < 0 || pixel_x >= (int64_t)width) || (pixel_y < 0 || pixel_y >= (int64_t)height) || get_board(pixel_x, pixel_y) == EATEN) {
                // player failed and got eliminated :(
                eliminate_player(it);
                if (num_of_players_with_status[PLAYING] + num_of_players_with_status[ZOMBIE] < 2) {
                    game_over();
                    break; // Game over ends round!
                }
            } else {
                // player ate another pixel
                eat_pixel(std::distance(players.begin(), it), pixel_x, pixel_y);
            }
        }
    }
}

// Gets events from given number to the end from events table, puts them into buffer and sends to player given or ALL
// players.
void Server::send_events(uint event_no, uint to_whom) {
    int rc = 0;
    while (event_no < events.size()) { // if event_no is equal to events.size() nothing is sent
        uint total_events_size_in_batch = 4;
        char *buffer_ = buffer;
        *reinterpret_cast<uint32_t*> (buffer_) = htobe32(game_id);
        buffer_ += sizeof(uint32_t);
        do {
            total_events_size_in_batch += events[event_no]->len + 8;
            buffer_ = put_event_to_buffer(events[event_no++], buffer_);
        } while ((event_no < events.size()) && (total_events_size_in_batch + events[event_no]->len + 8) < BUFFER_SIZE);
        if (to_whom == ALL) {
            for (auto player : players) {
                if (!(player.status == DISCONNECTED || player.status == ZOMBIE)) {
                    rc = sendto(sock, buffer, total_events_size_in_batch, 0, (struct sockaddr*) &player.address, player.addrlen);
                    if (rc < 0) {
                        error(strerror(errno), NONCRITICAL);
                    }
                }
            }
        } else {
            auto player = players[to_whom];
            if (!(player.status == DISCONNECTED || player.status == ZOMBIE)) {
                rc = sendto(sock, buffer, total_events_size_in_batch, 0, (struct sockaddr*) &player.address, player.addrlen);
                if (rc < 0) {
                    error(strerror(errno), NONCRITICAL);
                }
            }
        }

        if (events[event_no - 1]->event_type == GAME_OVER_TYPE) {
            events.clear();
            next_event_no_to_be_sent = 0;
        }
    }
}

// Copies information from Event object to buffer.
char *Server::put_event_to_buffer(const std::shared_ptr<Event>& event_ptr, char *buffer_offset) {
    uint8_t *buffer_start = reinterpret_cast<uint8_t*>(buffer_offset);
    *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(event_ptr->len);
    buffer_offset += sizeof (uint32_t);

    *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(event_ptr->event_no);
    buffer_offset += sizeof (uint32_t);

    *reinterpret_cast<uint8_t*> (buffer_offset) = event_ptr->event_type;
    buffer_offset += sizeof (uint8_t);

    NewGameEvent *new_game_event_ptr;
    PixelEvent *pixel_event_ptr;
    PlayerEliminatedEvent *player_eliminated_event_ptr;

    switch (event_ptr->event_type) {
        case NEW_GAME_TYPE:
            new_game_event_ptr = dynamic_cast<NewGameEvent*>(event_ptr.get());
            *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(new_game_event_ptr->max_x);
            buffer_offset += sizeof (uint32_t);

            *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(new_game_event_ptr->max_y);
            buffer_offset += sizeof (uint32_t);

            for (int i = 0; i < MAX_NUM_OF_PLAYERS; ++i) {
                if (strlen(new_game_event_ptr->player_names[i]) > 0) {
                    strcpy(reinterpret_cast<char *>(buffer_offset), new_game_event_ptr->player_names[i]);
                    buffer_offset += strlen(reinterpret_cast<char *>(buffer_offset)) + 1; // '\0'
                } else {
                    break;
                }
            }
            break;
        case PIXEL_TYPE:
            pixel_event_ptr = dynamic_cast<PixelEvent*>(event_ptr.get());
            *reinterpret_cast<uint8_t*> (buffer_offset) = pixel_event_ptr->player_number;
            buffer_offset += sizeof (uint8_t);

            *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(pixel_event_ptr->x);
            buffer_offset += sizeof (uint32_t);

            *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(pixel_event_ptr->y);
            buffer_offset += sizeof (uint32_t);

            break;
        case PLAYER_ELIMINATED_TYPE:
            player_eliminated_event_ptr = dynamic_cast<PlayerEliminatedEvent*>(event_ptr.get());
            *reinterpret_cast<uint8_t*> (buffer_offset) = player_eliminated_event_ptr->player_number;
            buffer_offset += sizeof (uint8_t);
            break;
        case GAME_OVER_TYPE:
            // event_data is empty
            break;
    }

    *reinterpret_cast<uint32_t*> (buffer_offset) = htobe32(crc32(buffer_start, reinterpret_cast<uint8_t*>(buffer_offset) - buffer_start));
    buffer_offset += sizeof (uint32_t);
    return buffer_offset;
}

// Eliminates player - changes its state and generates PlayerEliminatedEvent.
void Server::eliminate_player(std::vector<Player>::iterator it) {
    if (it->status == PLAYING) {
        change_status(it, ELIMINATED);
    } else if (it->status == ZOMBIE){
        change_status(it, DISCONNECTED);
    }

    events.push_back(std::make_shared<PlayerEliminatedEvent>(std::distance(players.begin(), it), events.size()));
}

// Changes state of pixel and generates PixelEvent.
void Server::eat_pixel(uint8_t player_no, uint x, uint y) {
    set_board(x, y, EATEN);
    events.push_back(std::make_shared<PixelEvent>(player_no, x, y, events.size()));
}

// Finishes the game, removes disconnected players from memory, updates status of another clients
// and generates GameOverEvent.
void Server::game_over() {
    is_game_active = false;
    bool player_disconnected = false;
    do {
        player_disconnected = false;
        for (auto it = players.begin(); it != players.end(); ++it) {
            if (it->status == DISCONNECTED || it->status == ZOMBIE) {
                player_disconnected = true;
                delete_player(it);
                break;
            }
        }
    } while (player_disconnected);

    for (auto it = players.begin(); it != players.end(); ++it) {
        if (it->status == PLAYING || it->status == ELIMINATED || (it->status == OBSERVER && !it->name.empty())) {
            change_status(it, WAITING);
        }
    }
    events.push_back(std::make_shared<GameOverEvent>(events.size()));
}

// Clears the board, sorts players in alphabetical order, initialises players locations and directions and generates
// Events (at least NewGameEvent).
void Server::new_game() {
    is_game_active = true;
    for (uint i = 0; i < width * height; ++i) {
        game_board[i] = NOT_EATEN;
    }

    // Sort players by name.
    std::sort(players.begin(), players.end(), [](const Player& a, const Player& b) {
        if (a.name.empty()) {
            if (b.name.empty()) {
                return (a.poll_arr_index <= b.poll_arr_index);
            } else {
                return false;
            }
        } else {
            if (b.name.empty()) {
                return true;
            } else {
                return (a.name <= b.name);
            }
        }
    });

    game_id = rand();

    events.push_back(std::make_shared<NewGameEvent>(maxx, maxy, players, events.size()));

    for (auto it = players.begin(); it != players.end(); ++it) {
        if (it->status == WILLING_TO_PLAY) {
            change_status(it, PLAYING);
        }
        if (it->status == PLAYING || it->status == ZOMBIE) {
            it->x = (rand() % maxx) + 0.5;
            it->y = (rand() % maxy) + 0.5;
            it->direction = rand() % 360;

            uint pixel_x = ceil(it->x);
            uint pixel_y = ceil(it->y);

            if (get_board(pixel_x, pixel_y) == EATEN) {
                // got eliminated at start :((
                eliminate_player(it);
                if (num_of_players_with_status[PLAYING] + num_of_players_with_status[ZOMBIE] < 2) {
                    game_over();
                }
            } else {
                // player ate pixel
                eat_pixel(std::distance(players.begin(), it), pixel_x, pixel_y);
            }
        }
    }
}

// Deletes player from memory.
void Server::delete_player(std::vector<Player>::iterator it) {
    num_of_players_with_status[it->status]--;
    close(poll_arr[it->poll_arr_index].fd); // Timer fd closed.
    poll_arr[it->poll_arr_index].fd = -1; // Remove timer.
    players.erase(it);
}

void Server::change_status(std::vector<Player>::iterator it, uint new_status) {
    num_of_players_with_status[it->status]--;
    it->status = new_status;
    num_of_players_with_status[it->status]++;
}

// Changes status to either ZOMBIE or DISCONNECTED and, if possible, deletes player.
void Server::disconnect_player(std::vector<Player>::iterator it) {
    if (it->status == PLAYING) {
        // change to ZOMBIE
        change_status(it, ZOMBIE);
    } else {
        change_status(it, DISCONNECTED);
        if (!is_game_active) {
            delete_player(it);
        }
    }
}

int main(int argc, char *argv[]) {
    // Default values.
    uint32_t port_number = 2021, seed = time(nullptr), turning_speed = 6, rounds_per_sec = 50;
    uint32_t width = 640, height = 480;

    int64_t seed_as_signed;

    int c;
    try {
        while ((c = getopt(argc, argv, "p:s:t:v:w:h:")) != -1) {
            if (std::string(optarg).find_first_not_of( "0123456789" ) != std::string::npos) {
                error("Incorrect character in argument.", CRITICAL);
            }
            switch (c) {
                case 'p':
                    port_number = std::stoul(optarg);
                    if (port_number < 1 || port_number > 65535) {
                        error("Incorrect port number.", CRITICAL);
                    }
                    break;
                case 's':
                    seed_as_signed = std::stoll(optarg);
                    if (seed_as_signed < 0 || seed_as_signed > UINT32_MAX) {
                        error("Seed not in accepted interval.", CRITICAL);
                    }
                    seed = (uint32_t) seed_as_signed;
                    break;
                case 't':
                    turning_speed = std::stoul(optarg);
                    if (turning_speed < 1 || turning_speed > 90) {
                        error("Incorrect turning speed.", CRITICAL);
                    }
                    break;
                case 'v':
                    rounds_per_sec = std::stoul(optarg);
                    if (rounds_per_sec < 1 || rounds_per_sec > 250) {
                        error("Incorrect rounds per sec.", CRITICAL);
                    }
                    break;
                case 'w':
                    width = std::stoul(optarg);
                    if (width < 10 || width > 4096) {
                        error("Incorrect width.", CRITICAL);
                    }
                    break;
                case 'h':
                    height = std::stoul(optarg);
                    if (height < 10 || height > 4096) {
                        error("Incorrect height.", CRITICAL);
                    }
                    break;
                default:
                    error("Unexpected argument.", CRITICAL);
                    break;
            }
        }
    } catch (...) {
        error("Invalid argument.", CRITICAL);
    }

    if (argc - optind != 0) {
        error("Unexpected argument.", CRITICAL);
    }

    Server server(port_number, seed, turning_speed, rounds_per_sec, width, height);

    server.run();
}
