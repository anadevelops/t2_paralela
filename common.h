#ifndef COMMON_H
#define COMMON_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <time.h>
#include <stdint.h>

// Retorna timestamp monotônico em milissegundos
static inline long long current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

#define TRADING_SERVER_PORT 9000
#define QUOTATION_SERVER_PORT 9001
#define MAX_ASSETS 2
#define MESSAGE_TIMEOUT_SECONDS 30  
#define BUFFER_SIZE 1024


// Estados do Saga (fluxo de compra) 
typedef enum {
    SAGA_INITIAL,           
    SAGA_WAITING_QUOTE_1,   
    SAGA_QUOTE_1_RECEIVED,  
    SAGA_WAITING_QUOTE_2,   
    SAGA_QUOTE_2_RECEIVED,  
    SAGA_RISK_ANALYSIS,   
    SAGA_EXECUTION,         
    SAGA_COMPLETED,         
    SAGA_FAILED            
} SagaState;

// Tipos de mensagem para comunicação 
typedef enum {
    MSG_QUOTE_REQUEST,      
    MSG_QUOTE_RESPONSE,     
    MSG_TRADE_ORDER,        
    MSG_TRADE_RESPONSE,     
    MSG_ERROR               
} MessageType;

// Cabeçalho de mensagem com suporte a expiração
typedef struct {
    uint32_t message_id;           
    MessageType type;              
    time_t timestamp;              
    int32_t ttl;                  
} MessageHeader;

// Requisição de cotação
typedef struct {
    MessageHeader header;
    char asset_code[32];         
    float quantity;            
} QuoteRequest;

// Resposta de cotação
typedef struct {
    MessageHeader header;
    char asset_code[32];
    float price;                  
    float     quantity;
    long long valid_until;   // ms absoluto: current_time_ms() + ttl_ms
} QuoteResponse;

// Ordem de compra
typedef struct {
    MessageHeader header;
    char asset_codes[MAX_ASSETS][32];
    float quantities[MAX_ASSETS];
    float prices[MAX_ASSETS];
    float total_value;
    float risk_score;            
} TradeOrder;

// Resposta de operação de compra
typedef struct {
    MessageHeader header;
    uint32_t order_id;
    int success;                   
    char status_message[256];
} TradeResponse;

// Contexto do processo Saga (Process Manager)
typedef struct {
    uint32_t saga_id;
    SagaState current_state;
    time_t created_at;
    
    char asset_codes[MAX_ASSETS][32];
    float quantities[MAX_ASSETS];
    float prices[MAX_ASSETS];
    int quotes_received;
    
    float total_value;
    float risk_score;

    time_t    expiration_time;
    long long ttl_deadline_ms;  // deadline absoluto da cotação em ms
    int       asset1_bought;    // 1 se ativo 1 já foi comprado (controla compensação)
} SagaContext;

static inline int is_message_expired(const MessageHeader *header) {
    time_t now = time(NULL);
    if (header->ttl > 0) {
        return (now - header->timestamp) > header->ttl;
    }
    return 0;
}

static inline void create_message_header(MessageHeader *header,
                                         uint32_t msg_id,
                                         MessageType type,
                                         int32_t ttl) {
    header->message_id = msg_id;
    header->type = type;
    header->timestamp = time(NULL);
    header->ttl = ttl;
}

static inline void init_saga_context(SagaContext *saga,
                                     uint32_t saga_id,
                                     const char *asset1,
                                     const char *asset2,
                                     float qty1, float qty2) {
    saga->saga_id = saga_id;
    saga->current_state = SAGA_INITIAL;
    saga->created_at = time(NULL);
    saga->expiration_time = saga->created_at + MESSAGE_TIMEOUT_SECONDS;
    
    strncpy(saga->asset_codes[0], asset1, 31);
    strncpy(saga->asset_codes[1], asset2, 31);
    saga->quantities[0]   = qty1;
    saga->quantities[1]   = qty2;
    saga->quotes_received = 0;
    saga->ttl_deadline_ms = 0;
    saga->asset1_bought   = 0;
}

#endif 
