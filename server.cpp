#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>
#include <fstream>

using namespace std;

struct Message {
    int seq_num;
    int type; // 0 for MOVE, 1 for ACK, 2 for STATE, 3 for BACKUP, -1 for error
    int error_code;
    int row;
    int col;
    char player;
};

enum ServerRole {
    MAIN_SERVER,
    BACKUP_SERVER
};

void print_board(char board[3][3]) {
    cout << "   1 2 3" << endl;
    cout << "  -------" << endl;
    for (int i = 0; i < 3; i++) {
        cout << " " << char('A' + i) << "| ";
        for (int j = 0; j < 3; j++) {
            cout << board[i][j] << " ";
        }
        cout << endl;
    }
    cout << endl;
}

bool check_win(char board[3][3], char player) {
    for (int i = 0; i < 3; i++) {
        if (board[i][0] == player && board[i][1] == player && board[i][2] == player) {
            return true;
        }
        if (board[0][i] == player && board[1][i] == player && board[2][i] == player) {
            return true;
        }
    }
    if (board[0][0] == player && board[1][1] == player && board[2][2] == player) {
        return true;
    }
    if (board[0][2] == player && board[1][1] == player && board[2][0] == player) {
        return true;
    }
    return false;
}

void send_state(const char board[3][3], int sockfd, const sockaddr_in &client_addr, socklen_t client_addr_len) {
    Message state_msg;
    state_msg.type = 2; // STATE message
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            state_msg.row = i;
            state_msg.col = j;
            state_msg.player = board[i][j];
            ssize_t bytes_sent = sendto(sockfd, &state_msg, sizeof(state_msg), 0, (const sockaddr*)&client_addr, client_addr_len);
            if (bytes_sent < 0) {
                perror("sendto() failed");
            }
        }
    }
}

int setup_socket(int argc, char** argv, ServerRole& server_role, vector<sockaddr_in> backup_servers) {
    if (argc != 2 && argc != 4) {
        cerr << "Usage: " << argv[0] << " <port> [main_server_ip main_server_port]" << endl;
        exit(1);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket() failed");
        exit(1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(atoi(argv[1]));

    int bind_result = ::bind(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr));
    if (bind_result < 0) {
        perror("bind() failed");
        exit(1);
    }

    int server_counter;
    const char* counter_filename = "server_counter.txt";
    ifstream counter_in(counter_filename);
    if (counter_in) {
        counter_in >> server_counter;
        counter_in.close();
    } else {
        server_counter = 0;
    }

    ofstream counter_out(counter_filename);
    if (counter_out) {
        counter_out << server_counter + 1;
        counter_out.close();
    } else {
        cerr << "Error: could not open counter file for writing" << endl;
        exit(1);
    }

    if (server_counter == 0) {
        server_role = MAIN_SERVER;
        cout << "I am the main server." << endl;
    } else {
        server_role = BACKUP_SERVER;
        cout << "I am a backup server." << endl;
        for (int i = 2; i < argc; i += 2) {
            struct sockaddr_in backup_server;
            backup_server.sin_family = AF_INET;
            backup_server.sin_addr.s_addr = inet_addr(argv[i]);
            backup_server.sin_port = htons(atoi(argv[i + 1]));
            backup_servers.push_back(backup_server);
            cout << "Connected to backup server " << argv[i] << ":" << argv[i + 1] << endl;
        }
    }
    return sockfd;
}

void process_move_message(const Message &msg, int sockfd, const sockaddr_in &client_addr, const vector<sockaddr_in> &backup_servers, char board[3][3], char &player) {
    if (msg.row < 0 || msg.row > 2 || msg.col < 0 || msg.col > 2 || board[msg.row][msg.col] != ' ') {
        Message ack_msg;
        ack_msg.seq_num = msg.seq_num;
        ack_msg.type = 1; // ACK message
        ack_msg.error_code = -1; // invalid move error code
        ssize_t bytes_sent = sendto(sockfd, &ack_msg, sizeof(ack_msg), 0, (const sockaddr *) &client_addr, sizeof(client_addr));
        if (bytes_sent < 0) {
            perror("sendto() failed");
            exit(1);
        }
        return;
    }
    board[msg.row][msg.col] = msg.player;
    // Send board state to backup server
    for (const auto &backup_server : backup_servers) {
        send_state(board, sockfd, backup_server, sizeof(backup_server));
    }
    // Send ACK to client
    Message ack_msg;
    ack_msg.seq_num = msg.seq_num;
    ack_msg.type = 1; // ACK message
    ack_msg.error_code = 0; // no error code
    ack_msg.row = msg.row;
    ack_msg.col = msg.col;
    ack_msg.player = msg.player;
    ssize_t bytes_sent = sendto(sockfd, &ack_msg, sizeof(ack_msg), 0, (const sockaddr *) &client_addr, sizeof(client_addr));
    if (bytes_sent < 0) {
        perror("sendto() failed");
        exit(1);
    }
    player = player == 'X' ? 'O' : 'X';
}

void process_message_main_server(const Message &msg, int sockfd, const sockaddr_in &client_addr, vector<sockaddr_in> &backup_servers, char board[3][3], char &player) {
    if (msg.type == 0) { // MOVE message
        process_move_message(msg, sockfd, client_addr, backup_servers, board, player);
    } else if (msg.type == 3) { // BACKUP message
        backup_servers.push_back(client_addr);
        // send the new backup server a STATE message to sync with the main server
        send_state(board, sockfd, client_addr, sizeof(client_addr));
    } else if (msg.type == 5) { // CHECK message
        // reply with a CHECK message
        Message check_msg;
        check_msg.type = 5; // CHECK message
        ssize_t bytes_sent = sendto(sockfd, &check_msg, sizeof(check_msg), 0, (const sockaddr *) &client_addr, sizeof(client_addr));
        if (bytes_sent < 0) {
            perror("sendto() failed");
            exit(1);
        }
    }
    for (const auto &backup_server : backup_servers) {
        send_state(board, sockfd, backup_server, sizeof(backup_server));
    }
}

void process_message_backup_server(const Message &msg, int sockfd, const sockaddr_in &client_addr, sockaddr_in &main_server_addr, std::vector<sockaddr_in> &backup_server_addrs, char board[3][3], ServerRole &server_role) {
    if (msg.type == 2) { // STATE message
        board[msg.row][msg.col] = msg.player;
    } else if (msg.type == 4) { // PROMOTE message
        server_role = MAIN_SERVER;
        main_server_addr = client_addr;
    } else if (msg.type == 5) { // CHECK message
        // reply with a CHECK message
        Message check_msg;
        check_msg.type = 5; // CHECK message
        ssize_t bytes_sent = sendto(sockfd, &check_msg, sizeof(check_msg), 0, (const sockaddr *) &main_server_addr, sizeof(main_server_addr));
        if (bytes_sent < 0) {
            perror("sendto() failed");
            exit(1);
        }
        // wait for a response from the main server or a backup server
        char buffer[sizeof(Message)];
        ssize_t bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (bytes_received < 0) {
            server_role = MAIN_SERVER;
            cout << "Main server failed, backup server becomes the main server." << endl;
            main_server_addr = client_addr;
            // send a PROMOTE_BACKUP message to all backup servers
            Message promote_msg;
            promote_msg.type = 6; // PROMOTE_BACKUP message
            for (const auto &backup_server_addr : backup_server_addrs) {
                ssize_t bytes_sent = sendto(sockfd, &promote_msg, sizeof(promote_msg), 0, (const sockaddr *) &backup_server_addr, sizeof(backup_server_addr));
                if (bytes_sent < 0) {
                    perror("sendto() failed");
                    exit(1);
                }
            }
            backup_server_addrs.clear();
        } else if (bytes_received == sizeof(Message)) {
            Message *msgg = (Message *) buffer;
            if (msgg->type == 5) { // CHECK message
                // ignore the message, we only care that the main server or a backup server is still reachable
            } else if (msgg->type == 6) { // PROMOTE_BACKUP message
                // add backup server address to the list of backup servers
                backup_server_addrs.push_back(client_addr);
            }
        }
    }
}


void remove_disconnected_clients(vector<sockaddr_in> &clients, vector<socklen_t> &client_lengths) {
    for (int i = 0; i < clients.size(); i++) {
        if (client_lengths[i] == 0) {
            clients.erase(clients.begin() + i);
            client_lengths.erase(client_lengths.begin() + i);
            i--;
        }
    }
}

void check_main_server_connection(int sockfd, sockaddr_in &main_server_addr, ServerRole &server_role, const sockaddr_in &client_addr, vector<sockaddr_in> &backup_servers) {
    if (server_role == BACKUP_SERVER) {
        Message check_msg;
        check_msg.type = 5; // CHECK message
        ssize_t bytes_sent = sendto(sockfd, &check_msg, sizeof(check_msg), 0, (const sockaddr *) &main_server_addr,
                                    sizeof(main_server_addr));
        if (bytes_sent < 0) {
            perror("sendto() failed");
            exit(1);
        }
        ssize_t bytes_received = recvfrom(sockfd, &check_msg, sizeof(check_msg), 0, NULL, NULL);
        if (bytes_received < 0) {
            // Main server is disconnected, so we become the new main server
            server_role = MAIN_SERVER;
            main_server_addr = client_addr;
            cout << "Main server failed, backup server becomes the main server." << endl;
            // Send a PROMOTE message to all backup servers to let them know that we are the new main server
            Message promote_msg;
            promote_msg.type = 4; // PROMOTE message
            ssize_t bytes_sent = sendto(sockfd, &promote_msg, sizeof(promote_msg), 0, (const sockaddr *) &client_addr,
                                        sizeof(client_addr));
            if (bytes_sent < 0) {
                perror("sendto() failed");
                exit(1);
            }
            // Clear the list of backup servers, since they should all sync with us now
            backup_servers.clear();
        }
    }
}

int main(int argc, char** argv) {
    ServerRole server_role;
    sockaddr_in main_server_addr;
    vector<sockaddr_in> backup_servers;

    int sockfd = setup_socket(argc, argv, server_role, backup_servers);

    char board[3][3];
    memset(board, ' ', sizeof(board)); // initialize board to empty

    char player = 'X';

    vector<sockaddr_in> clients;
    vector<socklen_t> client_lengths;

    while (true) {
        print_board(board);
        if (check_win(board, 'X')) {
            cout << "X wins!" << endl;
            break;
        } else if (check_win(board, 'O')) {
            cout << "O wins!" << endl;
            break;
        }

        // Handle message from clients
        Message msg;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        ssize_t bytes_received = recvfrom(sockfd, &msg, sizeof(msg), 0, (struct sockaddr*) &client_addr, &client_addr_len);
        if (bytes_received < 0) {
            perror("recvfrom() failed");
            exit(1);
        }

        // Process message based on server role
        if (server_role == MAIN_SERVER) {
            process_message_main_server(msg, sockfd, client_addr, backup_servers, board, player);
        } else {
            process_message_backup_server(msg, sockfd, client_addr, main_server_addr, backup_servers, board, server_role);
        }

        // Remove disconnected clients
        remove_disconnected_clients(clients, client_lengths);

        // Check if main server is still connected
        check_main_server_connection(sockfd, main_server_addr, server_role, client_addr, backup_servers);
    }

    close(sockfd);

    return 0;
}

