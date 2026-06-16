#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "common.h"

#define MAX_SAGAS 10

typedef struct {
    SagaContext sagas[MAX_SAGAS];
    int num_sagas;
} ProcessManager;

static ProcessManager g_process_manager = {0};

// Cria uma nova saga no Process Manager
static SagaContext *process_manager_create_saga(uint32_t saga_id,
                                                 const char *asset1,
                                                 const char *asset2,
                                                 float qty1, float qty2) {
    if (g_process_manager.num_sagas >= MAX_SAGAS) {
        fprintf(stderr, "❌ Limite de Sagas atingido!\n");
        return NULL;
    }
    
    SagaContext *saga = &g_process_manager.sagas[g_process_manager.num_sagas++];
    init_saga_context(saga, saga_id, asset1, asset2, qty1, qty2);
    
    printf("[PROCESS_MANAGER] ✓ Nova Saga criada: ID=%u (assets: %s, %s)\n",
           saga_id, asset1, asset2);
    
    return saga;
}

// Obtém uma saga pelo ID
static SagaContext *process_manager_get_saga(uint32_t saga_id) {
    for (int i = 0; i < g_process_manager.num_sagas; i++) {
        if (g_process_manager.sagas[i].saga_id == saga_id) {
            return &g_process_manager.sagas[i];
        }
    }
    return NULL;
}

// Atualiza o estado de uma saga
static void process_manager_update_saga_state(uint32_t saga_id,
                                               SagaState new_state) {
    SagaContext *saga = process_manager_get_saga(saga_id);
    if (saga) {
        saga->current_state = new_state;
        printf("[PROCESS_MANAGER] Saga %u: %d -> %d\n",
               saga_id, saga->current_state - 1, new_state);
    }
}


// Remove uma saga completada
static void process_manager_remove_saga(uint32_t saga_id) {
    for (int i = 0; i < g_process_manager.num_sagas; i++) {
        if (g_process_manager.sagas[i].saga_id == saga_id) {
            g_process_manager.sagas[i] = g_process_manager.sagas[g_process_manager.num_sagas - 1];
            g_process_manager.num_sagas--;
            printf("[PROCESS_MANAGER] Saga %u removida\n", saga_id);
            return;
        }
    }
}

static int saga_request_quote_1(int socket, SagaContext *saga) {
    printf("\n[SAGA %u] ▶ Etapa 1: Solicitando cotação de %s (%.2f unidades)\n",
           saga->saga_id, saga->asset_codes[0], saga->quantities[0]);
    
    QuoteRequest req;
    create_message_header(&req.header, saga->saga_id, MSG_QUOTE_REQUEST,
                          MESSAGE_TIMEOUT_SECONDS);
    strncpy(req.asset_code, saga->asset_codes[0], 31);
    req.quantity = saga->quantities[0];
    
    if (send(socket, (void *)&req, sizeof(req), 0) < 0) {
        perror("send");
        return 0;
    }
    
    process_manager_update_saga_state(saga->saga_id, SAGA_WAITING_QUOTE_1);
    return 1;
}

static int saga_receive_quote_1(int socket, SagaContext *saga) {
    printf("\n[SAGA %u] ◄ Etapa 2: Aguardando cotação de %s...\n",
           saga->saga_id, saga->asset_codes[0]);
    
    QuoteResponse resp;
    ssize_t received = recv(socket, (void *)&resp, sizeof(resp), 0);
    
    if (received <= 0) {
        fprintf(stderr, "❌ Erro ao receber cotação 1\n");
        return 0;
    }

    if (is_message_expired(&resp.header)) {
        printf("[SAGA %u] ⚠ Cotação expirada (ignorando)\n", saga->saga_id);
        return 0;
    }
    
    saga->prices[0] = resp.price;
    saga->quotes_received++;
    
    printf("[SAGA %u] ✓ Cotação recebida: %s @ R$%.2f (válida até: %ld)\n",
           saga->saga_id, resp.asset_code, resp.price, resp.valid_until);
    
    process_manager_update_saga_state(saga->saga_id, SAGA_QUOTE_1_RECEIVED);
    return 1;
}

static int saga_request_quote_2(int socket, SagaContext *saga) {
    printf("\n[SAGA %u] ▶ Etapa 3: Solicitando cotação de %s (%.2f unidades)\n",
           saga->saga_id, saga->asset_codes[1], saga->quantities[1]);
    
    QuoteRequest req;
    create_message_header(&req.header, saga->saga_id + 1, MSG_QUOTE_REQUEST,
                          MESSAGE_TIMEOUT_SECONDS);
    strncpy(req.asset_code, saga->asset_codes[1], 31);
    req.quantity = saga->quantities[1];
    
    if (send(socket, (void *)&req, sizeof(req), 0) < 0) {
        perror("send");
        return 0;
    }
    
    process_manager_update_saga_state(saga->saga_id, SAGA_WAITING_QUOTE_2);
    return 1;
}

static int saga_receive_quote_2(int socket, SagaContext *saga) {
    printf("\n[SAGA %u] ◄ Etapa 4: Aguardando cotação de %s...\n",
           saga->saga_id, saga->asset_codes[1]);
    
    QuoteResponse resp;
    ssize_t received = recv(socket, (void *)&resp, sizeof(resp), 0);
    
    if (received <= 0) {
        fprintf(stderr, "❌ Erro ao receber cotação 2\n");
        return 0;
    }

    if (is_message_expired(&resp.header)) {
        printf("[SAGA %u] ⚠ Cotação expirada (ignorando)\n", saga->saga_id);
        return 0;
    }
    
    saga->prices[1] = resp.price;
    saga->quotes_received++;
    
    printf("[SAGA %u] ✓ Cotação recebida: %s @ R$%.2f (válida até: %ld)\n",
           saga->saga_id, resp.asset_code, resp.price, resp.valid_until);
    
    process_manager_update_saga_state(saga->saga_id, SAGA_QUOTE_2_RECEIVED);
    return 1;
}

static int saga_analyze_risk(SagaContext *saga) {
    printf("\n[SAGA %u] 🔍 Etapa 5: Enviando dados ao Risk Server...\n",
           saga->saga_id);

    saga->total_value = (saga->prices[0] * saga->quantities[0]) +
                        (saga->prices[1] * saga->quantities[1]);

    // Monta TradeOrder com informações para o Risk Server
    TradeOrder order;
    create_message_header(&order.header, saga->saga_id, MSG_TRADE_ORDER, MESSAGE_TIMEOUT_SECONDS);
    strncpy(order.asset_codes[0], saga->asset_codes[0], 31);
    strncpy(order.asset_codes[1], saga->asset_codes[1], 31);
    order.quantities[0] = saga->quantities[0];
    order.quantities[1] = saga->quantities[1];
    order.prices[0] = saga->prices[0];
    order.prices[1] = saga->prices[1];
    order.total_value = saga->total_value;

    // Conecta ao Risk Server (localhost:9002)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 0; }
    struct sockaddr_in raddr;
    memset(&raddr,0,sizeof(raddr));
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(9002);
    inet_pton(AF_INET, "127.0.0.1", &raddr.sin_addr);

    if (connect(sock, (struct sockaddr *)&raddr, sizeof(raddr)) < 0) {
        perror("connect to risk"); close(sock); return 0;
    }

    if (send(sock, &order, sizeof(order), 0) < 0) { perror("send"); close(sock); return 0; }

    TradeResponse resp;
    ssize_t r = recv(sock, &resp, sizeof(resp), 0);
    close(sock);
    if (r <= 0) { fprintf(stderr, "Erro recebendo resposta do Risk Server\n"); return 0; }

    if (is_message_expired(&resp.header)) {
        printf("[SAGA %u] ⚠ Resposta do Risk Server expirada\n", saga->saga_id);
        return 0;
    }

    if (!resp.success) {
        printf("[SAGA %u] Risco proibiu operação: %s\n", saga->saga_id, resp.status_message);
        process_manager_update_saga_state(saga->saga_id, SAGA_FAILED);
        return 0;
    }

    printf("[SAGA %u] Risk Server aprovou: %s\n", saga->saga_id, resp.status_message);
    process_manager_update_saga_state(saga->saga_id, SAGA_RISK_ANALYSIS);
    return 1;
}

static int saga_execute_trade(SagaContext *saga) {
    printf("\n[SAGA %u] ✈ Etapa 6: Enviando ordens de compra ao Purchase Server...\n", saga->saga_id);

    // Envia ordem de compra para o primeiro ativo (purchase server localhost:9100)
    TradeOrder order1;
    create_message_header(&order1.header, saga->saga_id, MSG_TRADE_ORDER, MESSAGE_TIMEOUT_SECONDS);
    strncpy(order1.asset_codes[0], saga->asset_codes[0], 31);
    order1.quantities[0] = saga->quantities[0];
    order1.prices[0] = saga->prices[0];

    int sock1 = socket(AF_INET, SOCK_STREAM, 0);
    if (sock1 < 0) { perror("socket"); return 0; }
    struct sockaddr_in baddr;
    memset(&baddr,0,sizeof(baddr)); baddr.sin_family = AF_INET; baddr.sin_port = htons(9100);
    inet_pton(AF_INET, "127.0.0.1", &baddr.sin_addr);
    if (connect(sock1, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) { perror("connect buy1"); close(sock1); return 0; }
    if (send(sock1, &order1, sizeof(order1), 0) < 0) { perror("send buy1"); close(sock1); return 0; }
    TradeResponse resp1; if (recv(sock1, &resp1, sizeof(resp1), 0) <= 0) { fprintf(stderr,"❌ No response buy1\n"); close(sock1); return 0; }
    close(sock1);

    if (is_message_expired(&resp1.header)) { printf("[SAGA %u] ⚠ Resposta compra 1 expirada\n", saga->saga_id); return 0; }
    if (!resp1.success) {
        printf("[SAGA %u] Falha na compra do primeiro ativo: %s\n", saga->saga_id, resp1.status_message);
        process_manager_update_saga_state(saga->saga_id, SAGA_FAILED);
        return 0;
    }
    printf("[SAGA %u] ✓ Compra 1 executada: %s\n", saga->saga_id, resp1.status_message);

    // Envia ordem de compra para o segundo ativo
    TradeOrder order2;
    create_message_header(&order2.header, saga->saga_id + 1, MSG_TRADE_ORDER, MESSAGE_TIMEOUT_SECONDS);
    strncpy(order2.asset_codes[0], saga->asset_codes[1], 31);
    order2.quantities[0] = saga->quantities[1];
    order2.prices[0] = saga->prices[1];

    int sock2 = socket(AF_INET, SOCK_STREAM, 0);
    if (sock2 < 0) { perror("socket"); return 0; }
    if (connect(sock2, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) { perror("connect buy2"); close(sock2); return 0; }
    if (send(sock2, &order2, sizeof(order2), 0) < 0) { perror("send buy2"); close(sock2); return 0; }
    TradeResponse resp2; if (recv(sock2, &resp2, sizeof(resp2), 0) <= 0) { fprintf(stderr,"❌ No response buy2\n"); close(sock2); return 0; }
    close(sock2);

    if (is_message_expired(&resp2.header)) { printf("[SAGA %u] ⚠ Resposta compra 2 expirada\n", saga->saga_id); return 0; }
    if (!resp2.success) {
        printf("[SAGA %u] Falha na compra do segundo ativo: %s\n", saga->saga_id, resp2.status_message);
        // Compensação: vender o primeiro ativo (envia ordem de sell com quantidade negativa)
        printf("[SAGA %u] ⚠ Iniciando compensação: vendendo ativo 1\n", saga->saga_id);
        TradeOrder cancel1;
        create_message_header(&cancel1.header, saga->saga_id + 2, MSG_TRADE_ORDER, MESSAGE_TIMEOUT_SECONDS);
        strncpy(cancel1.asset_codes[0], saga->asset_codes[0], 31);
        cancel1.quantities[0] = -saga->quantities[0];
        cancel1.prices[0] = saga->prices[0];

        int sockc = socket(AF_INET, SOCK_STREAM, 0);
        if (sockc >= 0) {
            if (connect(sockc, (struct sockaddr *)&baddr, sizeof(baddr)) == 0) {
                TradeResponse cresp; send(sockc, &cancel1, sizeof(cancel1), 0);
                if (recv(sockc, &cresp, sizeof(cresp), 0) > 0) {
                    printf("[SAGA %u] Compensação: %s\n", saga->saga_id, cresp.status_message);
                }
            }
            close(sockc);
        }

        process_manager_update_saga_state(saga->saga_id, SAGA_FAILED);
        return 0;
    }

    printf("[SAGA %u] ✓ Compra 2 executada: %s\n", saga->saga_id, resp2.status_message);
    printf("[SAGA %u] Operação executada com sucesso!\n", saga->saga_id);
    printf("[SAGA %u] Valor total investido: R$%.2f\n", saga->saga_id, saga->total_value);

    process_manager_update_saga_state(saga->saga_id, SAGA_COMPLETED);
    return 1;
}


static void saga_orchestrate(int socket, SagaContext *saga) {
    printf("\n========================================\n");
    printf("INICIANDO SAGA #%u\n", saga->saga_id);
    printf("========================================\n");
    
    /* Etapa 1-2: Cotação ativo 1 */
    if (!saga_request_quote_1(socket, saga)) {
        goto saga_failed;
    }
    if (!saga_receive_quote_1(socket, saga)) {
        goto saga_failed;
    }
    
    /* Etapa 3-4: Cotação ativo 2 */
    if (!saga_request_quote_2(socket, saga)) {
        goto saga_failed;
    }
    if (!saga_receive_quote_2(socket, saga)) {
        goto saga_failed;
    }
    
    /* Etapa 5: Análise de risco */
    if (!saga_analyze_risk(saga)) {
        goto saga_failed;
    }
    
    /* Etapa 6: Execução */
    if (!saga_execute_trade(saga)) {
        goto saga_failed;
    }
    
    printf("\n========================================\n");
    printf("SAGA #%u COMPLETADA COM SUCESSO!\n", saga->saga_id);
    printf("========================================\n\n");
    
    return;
    
saga_failed:
    printf("\n========================================\n");
    printf("SAGA #%u FALHOU!\n", saga->saga_id);
    printf("========================================\n\n");
    process_manager_update_saga_state(saga->saga_id, SAGA_FAILED);
}

static int connect_to_server(const char *host, int port) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(socket_fd);
        return -1;
    }
    
    if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(socket_fd);
        return -1;
    }
    
    printf("[CLIENT] ✓ Conectado ao servidor em %s:%d\n", host, port);
    return socket_fd;
}


int trading_client_main(const char *server_host, int server_port) {
    int socket_fd = connect_to_server(server_host, server_port);
    if (socket_fd < 0) {
        return 1;
    }
    
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║  SISTEMA DE TRADING AUTOMATIZADO      ║\n");
    printf("║  (Saga + Process Manager + TTL)       ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    
    /* ========== Saga 1: Compra PETR4 + VALE5 ========== */
    SagaContext *saga1 = process_manager_create_saga(
        1, "PETR4", "VALE5", 100.0f, 50.0f
    );
    saga_orchestrate(socket_fd, saga1);
    
    sleep(2);
    
    /* ========== Saga 2: Compra ITUB4 + PETR4 ========== */
    SagaContext *saga2 = process_manager_create_saga(
        2, "ITUB4", "PETR4", 200.0f, 75.0f
    );
    saga_orchestrate(socket_fd, saga2);
    
    /* Limpeza */
    process_manager_remove_saga(1);
    process_manager_remove_saga(2);
    
    close(socket_fd);
    printf("\n[CLIENT] ✓ Desconectado\n");
    
    return 0;
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = QUOTATION_SERVER_PORT;
    
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    
    return trading_client_main(host, port);
}
