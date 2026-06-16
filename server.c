#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#include "common.h"

typedef struct {
    int client_socket;
    int thread_id;
} ClientContext;

// Tabela de cotações fixas (simulação de dados reais)
typedef struct {
    char asset_code[32];
    float price;
} QuotationTable;

static QuotationTable quotes[] = {
    {"PETR4", 25.50f},
    {"VALE5", 75.30f},
    {"ITUB4", 8.45f}
};

static int num_quotes = sizeof(quotes) / sizeof(quotes[0]);

static float get_fixed_quote(const char *asset_code) {
    for (int i = 0; i < num_quotes; i++) {
        if (strcmp(quotes[i].asset_code, asset_code) == 0) {
            return quotes[i].price;
        }
    }
    return -1.0f; 
}

static void process_quote_request(int socket, const QuoteRequest *req) {
    printf("[SERVER] Recebido: requisição de cotação para %s (qty: %.2f)\n",
           req->asset_code, req->quantity);
    
    if (is_message_expired(&req->header)) {
        printf("[SERVER] ⚠ MENSAGEM EXPIRADA! ID: %u (ignorando)\n",
               req->header.message_id);
        return;
    }
    
    float price = get_fixed_quote(req->asset_code);
    
    if (price < 0.0f) {
        printf("[SERVER] ❌ Ativo não encontrado: %s\n", req->asset_code);
        return;
    }
    
    QuoteResponse resp;
    create_message_header(&resp.header, req->header.message_id,
                          MSG_QUOTE_RESPONSE, MESSAGE_TIMEOUT_SECONDS);
    
    strncpy(resp.asset_code, req->asset_code, 31);
    resp.price = price;
    resp.quantity = req->quantity;
    resp.valid_until = time(NULL) + 10;  /* Cotação válida por 10 segundos */
    
    ssize_t sent = send(socket, (void *)&resp, sizeof(resp), 0);
    if (sent < 0) {
        perror("send");
        return;
    }
    
    printf("[SERVER] ✓ Cotação enviada: %s @ R$%.2f\n",
           resp.asset_code, resp.price);
}

static void *handle_client(void *arg) {
    ClientContext *ctx = (ClientContext *)arg;
    int socket = ctx->client_socket;
    int tid = ctx->thread_id;
    
    printf("[SERVER] [Thread %d] Cliente conectado\n", tid);
    
    while (1) {
        QuoteRequest req;
        ssize_t received = recv(socket, (void *)&req, sizeof(req), 0);
        
        if (received < 0) {
            perror("recv");
            break;
        }
        
        if (received == 0) {
            printf("[SERVER] [Thread %d] Cliente desconectado\n", tid);
            break;
        }
        
        if (req.header.type == MSG_QUOTE_REQUEST) {
            process_quote_request(socket, &req);
        }
    }
    
    close(socket);
    free(ctx);
    printf("[SERVER] [Thread %d] Encerrando\n", tid);
    
    return NULL;
}

int quotation_server_main(int port) {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_socket);
        return 1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        return 1;
    }
    
    if (listen(server_socket, 5) < 0) {
        perror("listen");
        close(server_socket);
        return 1;
    }
    
    printf("[SERVER] ✓ Servidor de Cotações iniciado em porta %d\n", port);
    printf("[SERVER] Cotações disponíveis:\n");
    for (int i = 0; i < num_quotes; i++) {
        printf("  - %s: R$%.2f\n", quotes[i].asset_code, quotes[i].price);
    }
    printf("[SERVER] Aguardando conexões...\n\n");
    
    int thread_counter = 0;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket,
                                    (struct sockaddr *)&client_addr,
                                    &client_addr_len);
        
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        
        ClientContext *ctx = malloc(sizeof(ClientContext));
        ctx->client_socket = client_socket;
        ctx->thread_id = thread_counter++;
        
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ctx) != 0) {
            perror("pthread_create");
            free(ctx);
            close(client_socket);
            continue;
        }
        
        pthread_detach(tid);
    }
    
    close(server_socket);
    return 0;
}

int main(int argc, char *argv[]) {
    int port = QUOTATION_SERVER_PORT;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    return quotation_server_main(port);
}
