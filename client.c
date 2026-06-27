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

#define MAX_SAGAS 10

// Conecta via TCP e retorna o fd, ou -1 em erro
static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

// Envia uma TradeOrder e recebe a TradeResponse; retorna 1 se ok e nao expirada
static int send_trade_order(struct sockaddr_in *addr, uint32_t id,
                             const char *asset, float qty, float price,
                             TradeResponse *out) {
    TradeOrder o = {0};
    create_message_header(&o.header, id, MSG_TRADE_ORDER, MESSAGE_TIMEOUT_SECONDS);
    strncpy(o.asset_codes[0], asset, sizeof(o.asset_codes[0]) - 1);
    o.quantities[0] = qty;
    o.prices[0]     = price;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        if (s >= 0) close(s);
        return 0;
    }
    send(s, &o, sizeof(o), 0);
    int ok = recv(s, out, sizeof(*out), 0) > 0;
    close(s);
    return ok && !is_message_expired(&out->header);
}

// --- Process Manager ---

typedef struct { SagaContext sagas[MAX_SAGAS]; int num_sagas; } ProcessManager;
static ProcessManager pm = {0};

static SagaContext *pm_create(uint32_t id, const char *a1, const char *a2,
                               float q1, float q2) {
    if (pm.num_sagas >= MAX_SAGAS) return NULL;
    SagaContext *s = &pm.sagas[pm.num_sagas++];
    init_saga_context(s, id, a1, a2, q1, q2);
    printf("[PM] Saga %u criada (%s | %s)\n", id, a1, a2);
    return s;
}

static void pm_set_state(SagaContext *saga, SagaState state) {
    saga->current_state = state;
    printf("[PM] Saga %u -> estado %d\n", saga->saga_id, state);
}

static void pm_remove(uint32_t id) {
    for (int i = 0; i < pm.num_sagas; i++)
        if (pm.sagas[i].saga_id == id) {
            pm.sagas[i] = pm.sagas[--pm.num_sagas];
            return;
        }
}

// --- Scatter/Gather ---

typedef struct {
    const char *host; int port; uint32_t saga_id;
    char   asset_code[32]; float quantity;
    float  price; long long valid_until; int success;
} QuoteWorkerArgs;

static void *quote_worker(void *arg) {
    QuoteWorkerArgs *w = (QuoteWorkerArgs *)arg;
    w->success = 0;

    int fd = tcp_connect(w->host, w->port);
    if (fd < 0) return NULL;

    QuoteRequest req = {0};
    create_message_header(&req.header, w->saga_id, MSG_QUOTE_REQUEST, MESSAGE_TIMEOUT_SECONDS);
    strncpy(req.asset_code, w->asset_code, sizeof(req.asset_code) - 1);
    req.quantity = w->quantity;
    send(fd, &req, sizeof(req), 0);

    QuoteResponse resp;
    int ok = recv(fd, &resp, sizeof(resp), 0) > 0;
    close(fd);

    if (!ok || is_message_expired(&resp.header)) return NULL;

    w->price       = resp.price;
    w->valid_until = resp.valid_until;
    w->success     = 1;
    printf("[SCATTER] %s @ R$%.2f\n", w->asset_code, w->price);
    return NULL;
}

static int scatter_gather_quotes(SagaContext *saga, const char *host, int port) {
    printf("\n[SAGA %u] SCATTER (%s | %s)\n",
           saga->saga_id, saga->asset_codes[0], saga->asset_codes[1]);

    QuoteWorkerArgs args[2];
    for (int i = 0; i < 2; i++) {
        args[i] = (QuoteWorkerArgs){
            .host    = host, .port = port,
            .saga_id = saga->saga_id + (uint32_t)i,
            .quantity = saga->quantities[i],
        };
        strncpy(args[i].asset_code, saga->asset_codes[i], sizeof(args[i].asset_code) - 1);
    }

    pthread_t threads[2];
    pthread_create(&threads[0], NULL, quote_worker, &args[0]);
    pthread_create(&threads[1], NULL, quote_worker, &args[1]);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    if (!args[0].success || !args[1].success) {
        printf("[SAGA %u] GATHER: cotacao incompleta\n", saga->saga_id);
        return 0;
    }

    saga->prices[0]       = args[0].price;
    saga->prices[1]       = args[1].price;
    saga->quotes_received = 2;
    saga->ttl_deadline_ms = args[0].valid_until < args[1].valid_until
                            ? args[0].valid_until : args[1].valid_until;

    printf("[SAGA %u] GATHER: %s=R$%.2f | %s=R$%.2f | TTL restante=%lldms\n",
           saga->saga_id,
           saga->asset_codes[0], saga->prices[0],
           saga->asset_codes[1], saga->prices[1],
           saga->ttl_deadline_ms - current_time_ms());
    return 1;
}

// --- Etapas da Saga ---

#define TTL_EXPIRED(s) ((s)->ttl_deadline_ms > 0 && current_time_ms() >= (s)->ttl_deadline_ms)

static int analyze_risk(SagaContext *saga, const char *host) {
    printf("\n[SAGA %u] Etapa 5: risco\n", saga->saga_id);

    saga->total_value = saga->prices[0] * saga->quantities[0]
                      + saga->prices[1] * saga->quantities[1];

    TradeOrder o = {0};
    create_message_header(&o.header, saga->saga_id, MSG_TRADE_ORDER, MESSAGE_TIMEOUT_SECONDS);
    for (int i = 0; i < MAX_ASSETS; i++) {
        strncpy(o.asset_codes[i], saga->asset_codes[i], sizeof(o.asset_codes[i]) - 1);
        o.quantities[i] = saga->quantities[i];
        o.prices[i]     = saga->prices[i];
    }
    o.total_value = saga->total_value;

    int fd = tcp_connect(host, 9002);
    if (fd < 0) return 0;
    send(fd, &o, sizeof(o), 0);
    TradeResponse resp;
    int ok = recv(fd, &resp, sizeof(resp), 0) > 0;
    close(fd);

    if (!ok || is_message_expired(&resp.header) || !resp.success) {
        printf("[SAGA %u] Risco rejeitou\n", saga->saga_id);
        return 0;
    }
    printf("[SAGA %u] Risco aprovou: %s\n", saga->saga_id, resp.status_message);
    pm_set_state(saga, SAGA_RISK_ANALYSIS);
    return 1;
}

static int execute_trade(SagaContext *saga, const char *host) {
    printf("\n[SAGA %u] Etapa 6: compras\n", saga->saga_id);

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(9100)};
    inet_pton(AF_INET, host, &addr.sin_addr);

    TradeResponse r1 = {0}, r2 = {0};

    // Compra ativo 1
    if (!send_trade_order(&addr, saga->saga_id,
                          saga->asset_codes[0], saga->quantities[0], saga->prices[0], &r1)
        || !r1.success) {
        printf("[SAGA %u] Falha compra 1: %s\n", saga->saga_id, r1.status_message);
        return 0;
    }
    printf("[SAGA %u] Compra 1 OK: %s\n", saga->saga_id, r1.status_message);
    saga->asset1_bought = 1;

    if (TTL_EXPIRED(saga)) {
        printf("[SAGA %u] TTL expirado antes de comprar ativo 2\n", saga->saga_id);
        goto compensate;
    }

    // Compra ativo 2
    if (!send_trade_order(&addr, saga->saga_id + 1,
                          saga->asset_codes[1], saga->quantities[1], saga->prices[1], &r2)
        || !r2.success) {
        printf("[SAGA %u] Falha compra 2: %s\n", saga->saga_id, r2.status_message);
        goto compensate;
    }
    printf("[SAGA %u] Compra 2 OK: %s | total=R$%.2f\n",
           saga->saga_id, r2.status_message, saga->total_value);
    pm_set_state(saga, SAGA_COMPLETED);
    return 1;

compensate:
    printf("[SAGA %u] Compensacao: vendendo %s\n", saga->saga_id, saga->asset_codes[0]);
    TradeResponse rc = {0};
    send_trade_order(&addr, saga->saga_id + 2,
                     saga->asset_codes[0], -saga->quantities[0], saga->prices[0], &rc);
    printf("[SAGA %u] Compensacao: %s\n", saga->saga_id, rc.status_message);
    return 0;
}

// --- Orquestrador ---

static void saga_orchestrate(const char *qhost, int qport,
                              const char *rhost, const char *phost,
                              SagaContext *saga) {
    printf("\n========================================\n");
    printf("SAGA #%u (%s | %s)\n", saga->saga_id,
           saga->asset_codes[0], saga->asset_codes[1]);
    printf("========================================\n");

    if (!scatter_gather_quotes(saga, qhost, qport))        goto fail;
    if (TTL_EXPIRED(saga)) { puts("[TTL] expirado antes do risco");       goto fail; }
    if (!analyze_risk(saga, rhost))                        goto fail;
    if (TTL_EXPIRED(saga)) { puts("[TTL] expirado antes das compras");    goto fail; }
    if (!execute_trade(saga, phost))                       goto fail;

    printf("SAGA #%u COMPLETA\n\n", saga->saga_id);
    return;
fail:
    printf("SAGA #%u FALHOU\n\n", saga->saga_id);
}

// --- Main ---

int main(int argc, char *argv[]) {
    int         n     = (argc > 1) ? atoi(argv[1]) : 1;
    const char *qhost = (argc > 2) ? argv[2] : "127.0.0.1";
    const char *rhost = (argc > 3) ? argv[3] : "127.0.0.1";
    const char *phost = (argc > 4) ? argv[4] : "127.0.0.1";

    printf("Trading | sagas=%d qhost=%s rhost=%s phost=%s\n\n",
           n, qhost, rhost, phost);

    const char *pairs[][2] = {
        {"ETH/USDT", "USD/BRL"}, {"PETR4", "VALE5"}, {"ITUB4", "PETR4"}
    };
    int ok = 0, fail = 0;

    for (int i = 0; i < n; i++) {
        const char **p = pairs[i % 3];
        SagaContext *s = pm_create((uint32_t)(i + 1), p[0], p[1], 1.0f, 1.0f);
        if (!s) { fail++; continue; }

        saga_orchestrate(qhost, QUOTATION_SERVER_PORT, rhost, phost, s);

        if (s->current_state == SAGA_COMPLETED) ok++; else fail++;
        pm_remove((uint32_t)(i + 1));
    }

    printf("=== SUMARIO: %d OK | %d FALHA | %d TOTAL ===\n", ok, fail, n);
    return 0;
}
