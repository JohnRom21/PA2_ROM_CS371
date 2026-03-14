/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>

#define MAX_EVENTS 64
#define PAYLOAD_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define DEFAULT_WINDOW_SIZE 8
#define MAX_CLIENTS 1024
#define TYPE_DATA 1
#define TYPE_ACK  2
#define TIMEOUT_MS 100

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 100000;
int window_size = DEFAULT_WINDOW_SIZE;
int mode = 1; /* 1 = Task1 pipelined, 2 = Task2 Go-Back-N */

typedef struct {
    int type;
    int seq_num;
    int ack_num;
    int length;
    char data[PAYLOAD_SIZE];
} packet_t;

typedef struct {
    int active;
    int acked;
    long long send_ts_us;
    packet_t pkt;
} slot_t;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;
    long rx_cnt;
    long retransmit_cnt;
    int window_size;
} client_thread_data_t;

typedef struct {
    int active;
    struct sockaddr_in addr;
    int expected_seq;
    int last_acked;
} client_state_t;

static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static int same_client(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static client_state_t *get_client_state(client_state_t *clients, const struct sockaddr_in *addr) {
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && same_client(&clients[i].addr, addr)) {
            return &clients[i];
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].active = 1;
            clients[i].addr = *addr;
            clients[i].expected_seq = 0;
            clients[i].last_acked = -1;
            return &clients[i];
        }
    }

    return NULL;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    slot_t *slots;
    int base = 0;
    int nextseq = 0;
    int done = 0;

    slots = (slot_t *)calloc(num_requests, sizeof(slot_t));
    if (slots == NULL) {
        perror("calloc");
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    data->request_rate = 0.0f;
    data->total_rtt = 0;
    data->total_messages = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->retransmit_cnt = 0;

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl add client socket");
        free(slots);
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    while (!done) {
        while (nextseq < num_requests && nextseq < base + data->window_size) {
            packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = TYPE_DATA;
            pkt.seq_num = nextseq;
            pkt.ack_num = -1;
            pkt.length = PAYLOAD_SIZE;
            memcpy(pkt.data, "ABCDEFGHIJKLMNO", PAYLOAD_SIZE);

            if (send(data->socket_fd, &pkt, sizeof(pkt), 0) < 0) {
                perror("send");
                done = 1;
                break;
            }

            slots[nextseq].active = 1;
            slots[nextseq].acked = 0;
            slots[nextseq].send_ts_us = now_us();
            slots[nextseq].pkt = pkt;

            data->tx_cnt++;
            nextseq++;
        }

        if (done) {
            break;
        }

        if (base >= num_requests) {
            break;
        }

        int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 10);
        if (n_events < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd) {
                packet_t resp;
                ssize_t n = recv(data->socket_fd, &resp, sizeof(resp), 0);
                if (n < 0) {
                    perror("recv");
                    done = 1;
                    break;
                }

                if (mode == 1) {
                    int seq = resp.seq_num;
                    if (seq >= 0 && seq < num_requests &&
                        slots[seq].active && !slots[seq].acked) {
                        long long rtt = now_us() - slots[seq].send_ts_us;
                        slots[seq].acked = 1;
                        data->rx_cnt++;
                        data->total_messages++;
                        data->total_rtt += rtt;

                        while (base < num_requests &&
                               slots[base].active &&
                               slots[base].acked) {
                            base++;
                        }
                    }
                } else {
                    if (resp.type == TYPE_ACK) {
                        int ack = resp.ack_num;
                        if (ack >= base && ack < num_requests) {
                            for (int s = base; s <= ack; s++) {
                                if (slots[s].active && !slots[s].acked) {
                                    long long rtt = now_us() - slots[s].send_ts_us;
                                    slots[s].acked = 1;
                                    data->rx_cnt++;
                                    data->total_messages++;
                                    data->total_rtt += rtt;
                                }
                            }
                            base = ack + 1;
                        }
                    }
                }
            }
        }

        long long now = now_us();

        if (mode == 1) {
            while (base < num_requests && slots[base].active) {
                if (slots[base].acked) {
                    base++;
                    continue;
                }

                if (now - slots[base].send_ts_us >= (long long)TIMEOUT_MS * 1000LL) {
                    slots[base].acked = 1; /* declare lost and move on */
                    base++;
                } else {
                    break;
                }
            }
        } else {
            if (base < nextseq &&
                slots[base].active &&
                !slots[base].acked &&
                now - slots[base].send_ts_us >= (long long)TIMEOUT_MS * 1000LL) {
                for (int s = base; s < nextseq; s++) {
                    if (slots[s].active && !slots[s].acked) {
                        if (send(data->socket_fd, &slots[s].pkt, sizeof(packet_t), 0) < 0) {
                            perror("retransmit send");
                            done = 1;
                            break;
                        }
                        slots[s].send_ts_us = now_us();
                        data->retransmit_cnt++;
                    }
                }
            }
        }
    }

    if (data->total_rtt > 0) {
        data->request_rate = (float)data->total_messages * 1000000.0f / (float)data->total_rtt;
    }

    free(slots);
    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        return;
    }

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0) {
            perror("epoll_create1");
            return;
        }

        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) {
            perror("socket");
            close(thread_data[i].epoll_fd);
            return;
        }

        if (connect(thread_data[i].socket_fd, (struct sockaddr *)&server_addr,
                    sizeof(server_addr)) < 0) {
            perror("connect");
            close(thread_data[i].socket_fd);
            close(thread_data[i].epoll_fd);
            return;
        }

        thread_data[i].window_size = window_size;
    }

    for (int i = 0; i < num_client_threads; i++) {
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            perror("pthread_create");
            close(thread_data[i].socket_fd);
            close(thread_data[i].epoll_fd);
            return;
        }
    }

    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;
    long total_tx = 0;
    long total_rx = 0;
    long total_retx = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_retx += thread_data[i].retransmit_cnt;
    }

    if (total_messages > 0) {
        printf("Average RTT: %lld us\n", total_rtt / total_messages);
    } else {
        printf("Average RTT: N/A\n");
    }

    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("tx_cnt=%ld rx_cnt=%ld lost_pkt_cnt=%ld retransmissions=%ld mode=%d\n",
           total_tx, total_rx, total_tx - total_rx, total_retx, mode);
}

void run_server() {
    int server_fd;
    int epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event event, events[MAX_EVENTS];
    client_state_t clients[MAX_CLIENTS];

    memset(clients, 0, sizeof(clients));

    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return;
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return;
    }

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl add server fd");
        close(server_fd);
        close(epoll_fd);
        return;
    }

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_events < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd) {
                packet_t pkt;
                struct sockaddr_in client_addr;
                socklen_t addrlen = sizeof(client_addr);

                ssize_t n = recvfrom(server_fd, &pkt, sizeof(pkt), 0,
                                     (struct sockaddr *)&client_addr, &addrlen);
                if (n < 0) {
                    perror("recvfrom");
                    continue;
                }

                if (mode == 1) {
                    if (sendto(server_fd, &pkt, sizeof(pkt), 0,
                               (struct sockaddr *)&client_addr, addrlen) < 0) {
                        perror("sendto");
                    }
                } else {
                    client_state_t *st = get_client_state(clients, &client_addr);
                    packet_t ack_pkt;

                    if (st == NULL) {
                        fprintf(stderr, "client table full\n");
                        continue;
                    }

                    memset(&ack_pkt, 0, sizeof(ack_pkt));
                    ack_pkt.type = TYPE_ACK;
                    ack_pkt.seq_num = -1;
                    ack_pkt.length = 0;

                    if (pkt.type == TYPE_DATA) {
                        if (pkt.seq_num == st->expected_seq) {
                            ack_pkt.ack_num = pkt.seq_num;
                            st->last_acked = pkt.seq_num;
                            st->expected_seq++;
                        } else {
                            ack_pkt.ack_num = st->last_acked;
                        }

                        if (sendto(server_fd, &ack_pkt, sizeof(ack_pkt), 0,
                                   (struct sockaddr *)&client_addr, addrlen) < 0) {
                            perror("sendto ack");
                        }
                    }
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        if (argc > 4) {
            num_client_threads = atoi(argv[4]);
        }
        if (argc > 5) {
            num_requests = atoi(argv[5]);
        }
        if (argc > 6) {
            window_size = atoi(argv[6]);
        }
        run_client();
    } else {
        printf("Usage:\n");
        printf("  %s server [server_ip server_port mode]\n", argv[0]);
        printf("  %s client [server_ip server_port num_client_threads num_requests window_size mode]\n", argv[0]);
    }

    return 0;
}
