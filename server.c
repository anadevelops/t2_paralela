#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "common.h"

static int ttl_ms = 300, latency_ms = 0, success_rate = 100;

static struct { char code[16]; float price; } quotes[] = {
    {"ETH/USDT", 1300.00f}, {"USD/BRL", 5.00f},
    {"PETR4", 25.50f}, {"VALE5", 75.30f}, {"ITUB4", 8.45f}
};
#define NUM_QUOTES ((int)(sizeof quotes / sizeof *quotes))

static void *handle_client(void *arg) {
    int fd = (int)(intptr_t)arg;

    QuoteRequest req;
    if (recv(fd, &req, sizeof(req), 0) <= 0 || is_message_expired(&req.header)) {
        close(fd); return NULL;
    }

    float price = -1.0f;
    for (int i = 0; i < NUM_QUOTES; i++)
        if (!strcmp(quotes[i].code, req.asset_code)) { price = quotes[i].price; break; }

    if (price < 0) {
        fprintf(stderr, "[SERVER] ativo nao encontrado: %s\n", req.asset_code);
        close(fd); return NULL;
    }

    if (rand() % 100 >= success_rate) {
        printf("[SERVER] falha simulada para %s\n", req.asset_code);
        close(fd); return NULL;
    }

    if (latency_ms > 0) {
        struct timespec d = {latency_ms / 1000, (long)(latency_ms % 1000) * 1000000L};
        nanosleep(&d, NULL);
    }

    QuoteResponse resp = {0};
    create_message_header(&resp.header, req.header.message_id,
                          MSG_QUOTE_RESPONSE, MESSAGE_TIMEOUT_SECONDS);
    strncpy(resp.asset_code, req.asset_code, sizeof(resp.asset_code) - 1);
    resp.price       = price;
    resp.quantity    = req.quantity;
    resp.valid_until = current_time_ms() + ttl_ms;

    send(fd, &resp, sizeof(resp), 0);
    printf("[SERVER] %s @ R$%.2f TTL=%dms\n", resp.asset_code, resp.price, ttl_ms);
    close(fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = QUOTATION_SERVER_PORT;
    if (argc > 1) port         = atoi(argv[1]);
    if (argc > 2) ttl_ms       = atoi(argv[2]);
    if (argc > 3) latency_ms   = atoi(argv[3]);
    if (argc > 4) success_rate = atoi(argv[4]);

    srand((unsigned int)time(NULL));
    printf("[SERVER] porta=%d TTL=%dms latencia=%dms sucesso=%d%%\n",
           port, ttl_ms, latency_ms, success_rate);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(port)
    };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 10);
    printf("[SERVER] porta=%d TTL=%dms latencia=%dms\n", port, ttl_ms, latency_ms);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, handle_client, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
}
