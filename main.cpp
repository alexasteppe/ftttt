#pragma clang diagnostic push
#pragma ide diagnostic ignored "llvmlibc-callee-namespace"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctime>

using namespace std;

struct Message {
    int seq_num;
    int type; // 0 for MOVE, 1 for ACK, -1 for error
    int error_code;
    int row;
    int col;
    char player;
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

void connect_to_server(struct sockaddr_in &server_addr, const char *server_ip, int port) {
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(port);
}

ssize_t send_message(int sockfd, const Message &msg, const sockaddr_in *server_addr) {
    return sendto(sockfd, &msg, sizeof(msg), 0, (const sockaddr *)server_addr, sizeof(*server_addr));
}

ssize_t recv_message(int sockfd, Message &msg, sockaddr_in *server_addr, int timeout) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    int ret = select(sockfd + 1, &read_fds, nullptr, nullptr, &tv);
    if (ret < 0) {
        perror("select() failed");
        return -1;
    } else if (ret == 0) {
        return 0; // timeout
    }

    socklen_t addr_len = sizeof(*server_addr);
    return recvfrom(sockfd, &msg, sizeof(msg), 0, (sockaddr *)server_addr, &addr_len);
}

void switch_to_next_server(int &sockfd, sockaddr_in *&current_server_addr, sockaddr_in &main_server_addr, sockaddr_in &backup_server_addr) {
    close(sockfd);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket() failed");
        exit(1);
    }

    if (current_server_addr == &main_server_addr) {
        current_server_addr = &backup_server_addr;
        cout << "Switching to backup server" << endl;
    } else {
        current_server_addr = &main_server_addr;
        cout << "Switching to main server" << endl;
    }
}

int main(int argc, char** argv) {
    if (argc != 4) {
        cerr << "Usage: " << argv[0] << " <main-server-ip> <backup-server-ip> <port>" << endl;
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket() failed");
        return 1;
    }

    sockaddr_in main_server_addr, backup_server_addr;
    connect_to_server(main_server_addr, argv[1], atoi(argv[3]));
    connect_to_server(backup_server_addr, argv[2], atoi(argv[3]));

    struct sockaddr_in *current_server_addr = &main_server_addr;
    socklen_t server_addr_len = sizeof(*current_server_addr);

    char board[3][3];
    memset(board, ' ', sizeof(board)); // initialize board to empty

    srand(time(NULL)); // initialize random seed

    char player = rand() % 2 == 0 ? 'X' : 'O'; // choose random player

    int seq_num = 0;
    int timeout = 100; // initial timeout is 100ms
    int num_attempts = 0;
    while (true) {
        print_board(board);
        if (check_win(board, 'X')) {
            cout << "X wins!" << endl;
            break;
        } else if (check_win(board, 'O')) {
            cout << "O wins!" << endl;
            break;
        } else if (num_attempts >= 10) {
            cerr << "Error: maximum number of attempts reached" << endl;
            return 1;
        }
        if (player == 'X') {
            cout << "X's turn" << endl;
        } else {
            cout << "O's turn" << endl;
        }
        string input;
        cout << "Enter row (A-C) and column (1-3) for " << player << " (e.g. A1, B2): ";
        cin >> input;
        if (input.size() != 2 || input[0] < 'A' || input[0] > 'C' || input[1] < '1' || input[1] > '3' || board[input[0] - 'A'][input[1] - '1'] != ' ') {
            cerr << "Invalid move, please try again" << endl;
            continue;
        }
        int row = input[0] - 'A';
        int col = input[1] - '1';

        Message msg;
        msg.seq_num = seq_num++;
        msg.type = 0; // MOVE message
        msg.row = row;
        msg.col = col;
        msg.player = player;
        /* send msg to server */
        ssize_t bytes_sent = send_message(sockfd, msg, current_server_addr);
        if (bytes_sent < 0) {
            perror("sendto() failed");
            return 1;
        }
        bool ack_received = false;
        while (!ack_received) {
            Message ack_msg;
            ssize_t bytes_received = recv_message(sockfd, ack_msg, current_server_addr, timeout);
            if (bytes_received < 0) {
                perror("recvfrom() failed");
                return 1;
            } else if (bytes_received == 0) { // timeout occurred
                /* resend msg */
                bytes_sent = send_message(sockfd, msg, current_server_addr);
                if (bytes_sent < 0) {
                    perror("sendto() failed");
                    return 1;
                }
                timeout *= 2; // double the timeout
                num_attempts++;

                if (num_attempts >= 5) { // switch to backup server after 5 failed attempts
                    switch_to_next_server(sockfd, current_server_addr, main_server_addr, backup_server_addr);
                    timeout = 100; // reset timeout on server switch
                    num_attempts = 0; // reset number of attempts
                }
            } else { // ack_msg received
                if (ack_msg.type == 1 && ack_msg.seq_num == msg.seq_num) { // ACK message
                    if (ack_msg.error_code == -1) { // error message
                        cerr << "Error: invalid move, please try again" << endl;
                        continue;
                    }
                    ack_received = true;
                    timeout = 100; // reset timeout on successful ack
                    num_attempts = 0; // reset number of attempts
                    board[ack_msg.row][ack_msg.col] = ack_msg.player;
                    player = player == 'X' ? 'O' : 'X';
                    print_board(board);
                }
            }
        }
    }
    close(sockfd);
    return 0;
}

#pragma clang diagnostic pop
