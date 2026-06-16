# Documentação de Padrões de Projeto Distribuído

## Resumo Executivo

Este sistema implementa **3 padrões de projeto de programação distribuída** em um protótipo de trading automatizado:

1. **Saga Distribuído** - Orquestração de transações com compensação
2. **Request-Reply Pattern** - Comunicação RPC síncrona entre processos
3. **TTL (Time-To-Live)** - Message Expiration para confiabilidade temporal

---

## 1. Saga Distribuído (Compensating Transactions)

### Definição
Padrão que coordena múltiplas operações distribuídas sem usar transações ACID. Se uma etapa falha, etapas anteriores são compensadas (revertidas).

### Implementação no Sistema

**Arquivo**: `client.c` - Função `saga_orchestrate()`

**Fluxo de Execução**:
```
Etapa 1: Request Quote 1 ────────────── Pode falhar
Etapa 2: Receive Quote 1 ─────────────→ TTL pode expirar
Etapa 3: Request Quote 2 ────────────── Pode falhar
Etapa 4: Receive Quote 2 ─────────────→ TTL pode expirar
Etapa 5: Risk Analysis (Risk Server) ──→ Pode ser rejeitado
Etapa 6: Execution (Purchase Server)
         ├─ Buy Asset 1 ──→ Pode falhar
         ├─ Buy Asset 2 ──→ Pode falhar ❌
         └─ Compensação: Sell Asset 1 (Rollback)
```

**Estruturas Usadas**:
- `SagaContext`: Mantém estado e dados da transação (arquivo: `common.h`)
- `ProcessManager` (global em `client.c`): Gerencia múltiplas sagas em paralelo

**Exemplo de Compensação**:
```c
if (!resp2.success) {
    printf("[SAGA %u] ❌ Falha na compra do segundo ativo\n", saga->saga_id);
    
    // COMPENSAÇÃO: vender o primeiro ativo
    TradeOrder cancel1;
    create_message_header(&cancel1.header, saga->saga_id + 2, ...);
    strncpy(cancel1.asset_codes[0], saga->asset_codes[0], 31);
    cancel1.quantities[0] = -saga->quantities[0];  // Quantidade NEGATIVA = venda
    
    // Enviar para Purchase Server
    int sockc = socket(...);
    connect(sockc, ...);
    send(sockc, &cancel1, sizeof(cancel1), 0);
    recv(sockc, &cresp, sizeof(cresp), 0);
    
    printf("[SAGA %u] Compensação: %s\n", saga->saga_id, cresp.status_message);
}
```

**Garantias**:
- ✅ Atomicidade: Tudo ou nada (com compensação)
- ✅ Consistência: Rollback automático de operações bem-sucedidas
- ✅ Rastreabilidade: Cada saga tem ID único

**Cenários Testados**:
1. ✅ Ambas compras bem-sucedidas → Saga completa
2. ✅ Segunda compra falha → Compensação vende primeira, Saga falha

---

## 2. Request-Reply Pattern (RPC Distribuído)

### Definição
Padrão de comunicação síncrona onde um cliente envia uma requisição estruturada e aguarda resposta do servidor.

### Implementação no Sistema

**Arquivos Envolvidos**:
- `client.c`: Envia `QuoteRequest` / `TradeOrder` e recebe respostas
- `server.c`: Quotation Server (recebe `QuoteRequest`)
- `risk_server.c`: Risk Server (recebe `TradeOrder`)
- `purchase_server.c`: Purchase Server (recebe `TradeOrder`)

**Fluxo de Comunicação**:

```
CLIENT                                    SERVER
  |                                         |
  |--- TCP Connect ---->              Listen
  |                                         |
  |--- Send QuoteRequest ------------->|
  |    (estrutura de 64+ bytes)        | Process
  |                                    | (simula latência)
  |<-- Recv QuoteResponse ------|------|
  |    (estrutura com preço)   recebe  |
  |                                    |
  Close Socket                         |
```

**Estruturas de Requisição**:

```c
// Requisição de Cotação
typedef struct {
    MessageHeader header;
    char asset_code[32];     // Ex: "ETH/USDT"
    float quantity;          // Quantidade
} QuoteRequest;

// Requisição de Operação (Risk ou Purchase)
typedef struct {
    MessageHeader header;
    char asset_codes[MAX_ASSETS][32];
    float quantities[MAX_ASSETS];
    float prices[MAX_ASSETS];
    float total_value;
    float risk_score;
} TradeOrder;
```

**Exemplo em Cliente**:
```c
// 1. Conectar ao Risk Server
int sock = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in raddr;
memset(&raddr, 0, sizeof(raddr));
raddr.sin_family = AF_INET;
raddr.sin_port = htons(9002);
inet_pton(AF_INET, "127.0.0.1", &raddr.sin_addr);
connect(sock, (struct sockaddr *)&raddr, sizeof(raddr));

// 2. Enviar TradeOrder
send(sock, &order, sizeof(order), 0);

// 3. Aguardar resposta
TradeResponse resp;
recv(sock, &resp, sizeof(resp), 0);

// 4. Processar resposta
if (resp.success) {
    printf("Operação aprovada\n");
} else {
    printf("Operação rejeitada: %s\n", resp.status_message);
}
```

**Vantagens**:
- ✅ Síncrono e previsível
- ✅ Fácil de debugar
- ✅ Sem necessidade de fila
- ✅ Respostas correlacionadas com requisições

**Portas Usadas**:
- 9001: Quotation Server
- 9002: Risk Server
- 9100: Purchase Server

---

## 3. TTL (Time-To-Live) / Message Expiration

### Definição
Padrão que adiciona "data de validade" às mensagens. Mensagens expiradas são descartadas, garantindo que dados obsoletos não sejam processados.

### Implementação no Sistema

**Arquivo Principal**: `common.h`

**MessageHeader com TTL**:
```c
typedef struct {
    uint32_t message_id;    // ID único
    MessageType type;       // Tipo de mensagem
    time_t timestamp;       // Hora de criação
    int32_t ttl;           // Time-To-Live em segundos (ex: 30)
} MessageHeader;

// Função de validação
static inline int is_message_expired(const MessageHeader *header) {
    time_t now = time(NULL);
    if (header->ttl > 0) {
        return (now - header->timestamp) > header->ttl;  // True se expirou
    }
    return 0;
}
```

**Pontos de Verificação**:

1. **Server.c** (Quotation Server):
```c
if (is_message_expired(&req->header)) {
    printf("[SERVER] ⚠ MENSAGEM EXPIRADA! ID: %u (ignorando)\n",
           req->header.message_id);
    return;  // Descarta requisição expirada
}
```

2. **client.c** (Trading Client):
```c
// Após receber resposta de cotação
if (is_message_expired(&resp.header)) {
    printf("[SAGA %u] ⚠ Cotação expirada (ignorando)\n", saga->saga_id);
    return 0;  // Aborta operação
}

// Após receber resposta de risco
if (is_message_expired(&resp.header)) {
    printf("[SAGA %u] ⚠ Resposta do Risk Server expirada\n", saga->saga_id);
    return 0;  // Aborta operação
}

// Após receber resposta de compra
if (is_message_expired(&resp1.header)) {
    printf("[SAGA %u] ⚠ Resposta compra 1 expirada\n", saga->saga_id);
    return 0;  // Aborta operação
}
```

3. **risk_server.c** e **purchase_server.c**:
```c
if (is_message_expired(&order.header)) {
    printf("[RISK] Received expired message id=%u, ignoring\n",
           order.header.message_id);
    close(client);
    continue;  // Ignora ordem expirada
}
```

**Ciclo de Vida de Uma Mensagem**:

```
Timestamp: T0
TTL: 30 segundos
Válida: T0 + 30s

T0      ──> Mensagem criada
T0 + 10 ──> Em trânsito para servidor (válida)
T0 + 20 ──> Processada (válida)
T0 + 29 ──> Ainda válida
T0 + 31 ──> EXPIRADA! Descartada
```

**Cenários Tratados**:
1. ✅ Cotação válida dentro do TTL → Aceita
2. ✅ Cotação expirada → Aborta operação
3. ✅ Resposta do Risk Server expirada → Aborta
4. ✅ Resposta de compra expirada → Aborta com compensação

**Configuração**:
```c
#define MESSAGE_TIMEOUT_SECONDS 30  // common.h

// Criar mensagem com TTL
create_message_header(&header, msg_id, MSG_TYPE, MESSAGE_TIMEOUT_SECONDS);
```

---

## Integração dos Padrões

Os 3 padrões trabalham juntos de forma integrada:

```
┌─────────────────────────────────────────────────────────────┐
│ SAGA DISTRIBUÍDO (Orquestração + Compensação)               │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ REQUEST-REPLY: Quote + Risk + Purchase (3 turnos)  │   │
│  │                                                      │   │
│  │  [Request 1] ──→ Server ──→ [Response 1]           │   │
│  │  [Request 2] ──→ Server ──→ [Response 2]           │   │
│  │  [Request 3] ──→ Server ──→ [Response 3] ❌ FALHA   │   │
│  │                                                      │   │
│  │  Compensação: [Request -1] ──→ Rollback            │   │
│  └─────────────────────────────────────────────────────┘   │
│                           ▲                                 │
│                           │                                 │
│                    TTL CHECK                                │
│                 (cada resposta)                             │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## Testes Fornecidos

### 1. `run_test.sh` - Cenário de Sucesso
- Risk: 90% sucesso
- Purchase: 95% sucesso
- Resultado esperado: Ambas as sagas completadas com sucesso

### 2. `test_failure.sh` - Cenário de Falha com Compensação
- Risk: 100% sucesso (sempre aprova)
- Purchase: 50% sucesso (alta taxa de falha)
- Resultado esperado: Segunda compra falha, compensação vende primeiro ativo

---

## Requisitos do Enunciado - Cobertura

| Requisito | Implementação | Arquivo |
|-----------|------------------|---------|
| 4 processos independentes | quotation_server, risk_server, purchase_server, trading_client | *.c |
| N ordens de compra | ProcessManager com MAX_SAGAS (parametrizável) | client.c, common.h |
| 4 integrações | Quote, Risk, Buy1, Buy2 | client.c |
| Confiável e atômico | Saga com compensação | client.c |
| TTL com aborto | is_message_expired() em todos os servidores | common.h, *.c |
| Falha cancela operação | saga_failed goto + compensação | client.c |
| 3 padrões distribuídos | Saga, Request-Reply, TTL | Todos arquivos |
| Taxa de sucesso configurável | Parâmetro no Risk/Purchase Server | risk_server.c, purchase_server.c |
| Latência configurável | usleep(random_between()) | risk_server.c, purchase_server.c |

---

## Extensões Possíveis

1. **Persistência**: Salvar estado de sagas em SQLite
2. **Timeout**: Aguardar máximo N segundos por resposta
3. **Retry Automático**: Reenviar com backoff exponencial
4. **Multi-threading**: Processar múltiplas sagas em paralelo
5. **Load Balancing**: Múltiplos servidores de risco/compra
6. **Serialização JSON**: Usar JSON ao invés de structs binários
7. **Autenticação**: TLS/mTLS entre processos
8. **Logging**: Arquivo de audit trail
9. **Metrics**: Prometheus/Grafana
10. **Observabilidade**: Distributed tracing (OpenTelemetry)

---

## Conclusão

Este protótipo demonstra como combinar 3 padrões de projeto de forma efetiva para construir sistemas distribuídos confiáveis, mesmo sem usar middleware pesado como message queues ou message brokers. A simplicidade do design (C puro, TCP sockets) facilita a compreensão dos conceitos fundamentais de arquitetura distribuída.
