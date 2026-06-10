#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "common.h"

/* ============================================================================
 * CLIENTE DE TRADING (SISTEMA DE OPERAÇÃO)
 * 
 * Implementa os padrões:
 * - SAGA: Fluxo de compra com múltiplas etapas e possibilidade de rollback
 * - PROCESS MANAGER: Gerenciam estado das operações em andamento
 * - MESSAGE EXPIRATION: TTL nas mensagens, descarta mensagens expiradas
 * 
 * Máximo desacoplamento através de:
 * - Separação entre lógica de negócio (Saga) e comunicação
 * - Mensagens bem definidas (contrato)
 * - Estado centralizado no Process Manager
 * ============================================================================ */

/* ============================================================================
 * PROCESS MANAGER - Gerenciador de Processos/Sagas em Andamento
 * ============================================================================ */

#define MAX_SAGAS 10

typedef struct {
    SagaContext sagas[MAX_SAGAS];
    int num_sagas;
} ProcessManager;

static ProcessManager g_process_manager = {0};

/**
 * Cria uma nova saga no Process Manager
 */
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

/**
 * Obtém uma saga pelo ID
 */
static SagaContext *process_manager_get_saga(uint32_t saga_id) {
    for (int i = 0; i < g_process_manager.num_sagas; i++) {
        if (g_process_manager.sagas[i].saga_id == saga_id) {
            return &g_process_manager.sagas[i];
        }
    }
    return NULL;
}

/**
 * Atualiza o estado de uma saga
 */
static void process_manager_update_saga_state(uint32_t saga_id,
                                               SagaState new_state) {
    SagaContext *saga = process_manager_get_saga(saga_id);
    if (saga) {
        saga->current_state = new_state;
        printf("[PROCESS_MANAGER] Saga %u: %d -> %d\n",
               saga_id, saga->current_state - 1, new_state);
    }
}

/**
 * Remove uma saga completada
 */
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

/* ============================================================================
 * SAGA - Orquestrador de Fluxo de Compra
 * ============================================================================ */

/**
 * Etapa 1: Requisita cotação do primeiro ativo
 */
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

/**
 * Etapa 2: Processa resposta de cotação do ativo 1
 */
static int saga_receive_quote_1(int socket, SagaContext *saga) {
    printf("\n[SAGA %u] ◄ Etapa 2: Aguardando cotação de %s...\n",
           saga->saga_id, saga->asset_codes[0]);
    
    QuoteResponse resp;
    ssize_t received = recv(socket, (void *)&resp, sizeof(resp), 0);
    
    if (received <= 0) {
        fprintf(stderr, "❌ Erro ao receber cotação 1\n");
        return 0;
    }
    
    /* Verifica expiração */
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

/**
 * Etapa 3: Requisita cotação do segundo ativo
 */
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

/**
 * Etapa 4: Processa resposta de cotação do ativo 2
 */
static int saga_receive_quote_2(int socket, SagaContext *saga) {
    printf("\n[SAGA %u] ◄ Etapa 4: Aguardando cotação de %s...\n",
           saga->saga_id, saga->asset_codes[1]);
    
    QuoteResponse resp;
    ssize_t received = recv(socket, (void *)&resp, sizeof(resp), 0);
    
    if (received <= 0) {
        fprintf(stderr, "❌ Erro ao receber cotação 2\n");
        return 0;
    }
    
    /* Verifica expiração */
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

/**
 * Etapa 5: Análise de risco
 */
static int saga_analyze_risk(SagaContext *saga) {
    printf("\n[SAGA %u] 🔍 Etapa 5: Analisando risco da operação...\n",
           saga->saga_id);
    
    /* Calcula valor total */
    saga->total_value = (saga->prices[0] * saga->quantities[0]) +
                        (saga->prices[1] * saga->quantities[1]);
    
    /* Simula análise de risco (simplificada) */
    saga->risk_score = 0.3f;  /* 30% de risco (baixo) */
    
    printf("[SAGA %u] 📊 Valor total da operação: R$%.2f\n",
           saga->saga_id, saga->total_value);
    printf("[SAGA %u] 📈 Score de risco: %.1f%% (ACEITO)\n",
           saga->saga_id, saga->risk_score * 100);
    
    if (saga->risk_score > 0.7f) {
        printf("[SAGA %u] ❌ Risco muito alto! Operação CANCELADA\n",
               saga->saga_id);
        process_manager_update_saga_state(saga->saga_id, SAGA_FAILED);
        return 0;
    }
    
    process_manager_update_saga_state(saga->saga_id, SAGA_RISK_ANALYSIS);
    return 1;
}

/**
 * Etapa 6: Execução da compra
 */
static int saga_execute_trade(SagaContext *saga) {
    printf("\n[SAGA %u] ✈ Etapa 6: Executando compra...\n", saga->saga_id);
    
    /* Simula execução */
    sleep(1);
    
    printf("[SAGA %u] 🎯 Compra de %s × %.2f @ R$%.2f = R$%.2f\n",
           saga->saga_id,
           saga->asset_codes[0], saga->quantities[0],
           saga->prices[0], saga->prices[0] * saga->quantities[0]);
    
    printf("[SAGA %u] 🎯 Compra de %s × %.2f @ R$%.2f = R$%.2f\n",
           saga->saga_id,
           saga->asset_codes[1], saga->quantities[1],
           saga->prices[1], saga->prices[1] * saga->quantities[1]);
    
    printf("[SAGA %u] ✅ Operação executada com sucesso!\n", saga->saga_id);
    printf("[SAGA %u] 💰 Valor total investido: R$%.2f\n",
           saga->saga_id, saga->total_value);
    
    process_manager_update_saga_state(saga->saga_id, SAGA_COMPLETED);
    return 1;
}

/**
 * Orquestra o fluxo completo de uma Saga
 */
static void saga_orchestrate(int socket, SagaContext *saga) {
    printf("\n========================================\n");
    printf("🚀 INICIANDO SAGA #%u\n", saga->saga_id);
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
    printf("✅ SAGA #%u COMPLETADA COM SUCESSO!\n", saga->saga_id);
    printf("========================================\n\n");
    
    return;
    
saga_failed:
    printf("\n========================================\n");
    printf("❌ SAGA #%u FALHOU!\n", saga->saga_id);
    printf("========================================\n\n");
    process_manager_update_saga_state(saga->saga_id, SAGA_FAILED);
}

/* ============================================================================
 * CLIENTE PRINCIPAL
 * ============================================================================ */

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

/**
 * Função principal do cliente
 */
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

/* ============================================================================
 * PONTO DE ENTRADA
 * ============================================================================ */

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
