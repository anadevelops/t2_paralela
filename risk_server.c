#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "common.h"

static int success_rate = 90; // %
static int min_latency_ms = 10;
static int max_latency_ms = 200;


static int random_between(int a, int b) {
    if (b <= a) return a;
    return a + rand() % (b - a + 1);
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    int port = 9002;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) success_rate = atoi(argv[2]);
    if (argc > 3) min_latency_ms = atoi(argv[3]);
    if (argc > 4) max_latency_ms = atoi(argv[4]);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_socket, 5) < 0) { perror("listen"); return 1; }

    printf("[RISK] Listening on port %d (success=%d%% latency=%d-%dms)\n",
           port, success_rate, min_latency_ms, max_latency_ms);

    while (1) {
        int client = accept(server_socket, NULL, NULL);
        if (client < 0) { perror("accept"); continue; }

        TradeOrder order;
        ssize_t r = recv(client, &order, sizeof(order), 0);
        if (r <= 0) { close(client); continue; }

        if (is_message_expired(&order.header)) {
            printf("[RISK] Received expired message id=%u, ignoring\n", order.header.message_id);
            close(client);
            continue;
        }

        int latency = random_between(min_latency_ms, max_latency_ms);
        usleep(latency * 1000);

        TradeResponse resp;
        create_message_header(&resp.header, order.header.message_id, MSG_TRADE_RESPONSE, MESSAGE_TIMEOUT_SECONDS);
        resp.order_id = order.header.message_id;

        int roll = random_between(0, 99);
        if (roll < success_rate) {
            resp.success = 1;
            snprintf(resp.status_message, sizeof(resp.status_message), "RISK_APPROVED (latency=%dms)", latency);
            printf("[RISK] Approved order %u (latency=%dms)\n", order.header.message_id, latency);
        } else {
            resp.success = 0;
            snprintf(resp.status_message, sizeof(resp.status_message), "RISK_REJECTED (latency=%dms)", latency);
            printf("[RISK] Rejected order %u (latency=%dms)\n", order.header.message_id, latency);
        }

        send(client, &resp, sizeof(resp), 0);
        close(client);
    }

    close(server_socket);
    return 0;
}
