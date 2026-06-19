#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "common.h"

static int success_rate = 95;
static int min_latency_ms = 20;
static int max_latency_ms = 300;
static char fail_asset_code[32] = "";

// sorteia um valor inteiro dentro do intervalo informado
static int random_between(int a, int b) {
    if (b <= a) return a;
    return a + rand() % (b - a + 1);
}

// garante que uma configuracao fique dentro de um limite valido
static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

// corrige argumentos invalidos recebidos pela linha de comando
static void normalize_config(void) {
    success_rate = clamp_int(success_rate, 0, 100);
    min_latency_ms = clamp_int(min_latency_ms, 0, 60000);
    max_latency_ms = clamp_int(max_latency_ms, 0, 60000);

    if (max_latency_ms < min_latency_ms) {
        int tmp = min_latency_ms;
        min_latency_ms = max_latency_ms;
        max_latency_ms = tmp;
    }
}

// pausa a execucao por um tempo em milissegundos para simular latencia
static void sleep_ms(int milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;

    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

// quantidade positiva = compra, negativa = venda
static const char *operation_name(const TradeOrder *order) {
    return order->quantities[0] < 0.0f ? "SELL" : "BUY";
}

// permite forcar falha de um ativo especifico para teste deterministico
static int should_force_failure(const TradeOrder *order) {
    return fail_asset_code[0] != '\0' &&
           strcmp(order->asset_codes[0], fail_asset_code) == 0;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));

    // configuracao padrao que pode ser sobrescrita pelos argumentos
    int port = 9100;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) success_rate = atoi(argv[2]);
    if (argc > 3) min_latency_ms = atoi(argv[3]);
    if (argc > 4) max_latency_ms = atoi(argv[4]);

    // arg opcional pra ativo que deve falhar sempre
    if (argc > 5) {
        strncpy(fail_asset_code, argv[5], sizeof(fail_asset_code) - 1);
        fail_asset_code[sizeof(fail_asset_code) - 1] = '\0';
    }
    normalize_config();

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) { perror("socket"); return 1; }

    // permite reutilizar a porta depois de reiniciar o servidor
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // endereco que o servidor de compra vai escutar conexoes TCP
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_socket, 5) < 0) { perror("listen"); return 1; }

    printf("[BUY] Listening on port %d (success=%d%% latency=%d-%dms",
           port, success_rate, min_latency_ms, max_latency_ms);
    if (fail_asset_code[0] != '\0') {
        printf(" forced_fail_asset=%s", fail_asset_code);
    }
    printf(")\n");

    while (1) {
        int client = accept(server_socket, NULL, NULL);
        if (client < 0) { perror("accept"); continue; }

        // recebe  ordem enviada pelo sistema de operacao
        TradeOrder order;
        ssize_t r = recv(client, &order, sizeof(order), 0);
        if (r <= 0) { close(client); continue; }

        if (is_message_expired(&order.header)) {
            printf("[BUY] Received expired order id=%u, ignoring\n", order.header.message_id);
            close(client);
            continue;
        }

        // prepara a resposta pro cliente
        TradeResponse resp;
        create_message_header(&resp.header, order.header.message_id, MSG_TRADE_RESPONSE, MESSAGE_TIMEOUT_SECONDS);
        resp.order_id = order.header.message_id;

        // simula o tempo de processamento da compra
        int latency = random_between(min_latency_ms, max_latency_ms);
        const char *operation = operation_name(&order);
        sleep_ms(latency);

        // verifica se a ordem expirou durante o processamento
        if (is_message_expired(&order.header)) {
            resp.success = 0;
            snprintf(resp.status_message, sizeof(resp.status_message),
                     "%s_TIMEOUT_AFTER_PROCESSING asset=%s (latency=%dms)",
                     operation, order.asset_codes[0], latency);
            printf("[BUY] %s order %u for %s expired after processing (latency=%dms)\n",
                   operation, order.header.message_id, order.asset_codes[0], latency);
            send(client, &resp, sizeof(resp), 0);
            close(client);
            continue;
        }

        // ordem pode falhar por configuracao forcada ou pela taxa de sucesso
        int forced_failure = should_force_failure(&order);
        int roll = random_between(0, 99);
        if (!forced_failure && roll < success_rate) {
            resp.success = 1;
            snprintf(resp.status_message, sizeof(resp.status_message),
                     "%s_EXECUTED asset=%s qty=%.2f price=%.2f (latency=%dms)",
                     operation, order.asset_codes[0], order.quantities[0],
                     order.prices[0], latency);
            printf("[BUY] %s executed order %u asset=%s qty=%.2f price=%.2f (latency=%dms)\n",
                   operation, order.header.message_id, order.asset_codes[0],
                   order.quantities[0], order.prices[0], latency);
        } else {
            resp.success = 0;
            snprintf(resp.status_message, sizeof(resp.status_message),
                     "%s_FAILED asset=%s%s (latency=%dms)",
                     operation, order.asset_codes[0],
                     forced_failure ? " forced" : "", latency);
            printf("[BUY] %s failed order %u asset=%s%s (latency=%dms)\n",
                   operation, order.header.message_id, order.asset_codes[0],
                   forced_failure ? " forced" : "", latency);
        }

        send(client, &resp, sizeof(resp), 0);
        close(client);
    }

    close(server_socket);
    return 0;
}
