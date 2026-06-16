# Sistema de Trading Automatizado Distribuído

Um protótipo de sistema de operação em mercado (trading) automatizado que simula a compra de dois ativos (ETH/USDT, USD/BRL) a partir de dados de cotação, análise de risco e execução de compra em processos distribuídos independentes.

## Arquitetura

O sistema é composto por **4 processos independentes**:

1. **Quotation Server** (porta 9001): Sistema de cotação que retorna preços de ativos com TTL
2. **Risk Server** (porta 9002): Sistema de análise de risco com latência configurável
3. **Purchase Server** (porta 9100): Sistema de execução de compra/venda de ativos
4. **Trading Client** (cliente): Orquestra o fluxo completo usando Saga Distribuído

## Padrões de Projeto Distribuído Implementados

### 1. **Saga Distribuído** (Compensação)
- **Implementação**: Fluxo de compra de 2 ativos com compensação automática
- **Local**: `client.c` (orquestração)
- **Descrição**: 
  - Se a primeira compra sucede e a segunda falha, executa uma venda (compensação) do primeiro ativo
  - Garante atomicidade das operações através da compensação
  - Estados: INITIAL → QUOTE_1 → QUOTE_2 → RISK_ANALYSIS → EXECUTION → COMPLETED/FAILED

### 2. **Request-Reply Pattern** (RPC Distribuído)
- **Implementação**: Comunicação TCP síncrona entre cliente e servidores
- **Locais**: 
  - `client.c`: envia `TradeOrder`/`QuoteRequest` e aguarda resposta
  - `risk_server.c` e `purchase_server.c`: recebem requisição, processam e respondem
- **Descrição**:
  - Cliente conecta a cada servidor, envia requisição estruturada
  - Servidor processa com latência simulada e TTL checking
  - Retorna `TradeResponse` com status de sucesso/falha

### 3. **TTL (Time-To-Live) / Message Expiration**
- **Implementação**: Verificação de expiração em cada etapa
- **Estrutura**: `MessageHeader` com `timestamp` e `ttl`
- **Locais**:
  - `common.h`: `is_message_expired()` valida se mensagem expirou
  - `server.c`: verifica TTL de requisições de cotação
  - `risk_server.c` e `purchase_server.c`: rejeitam mensagens expiradas
  - `client.c`: valida TTL de respostas
- **Comportamento**: Operação é abortada se TTL for excedido antes/depois de qualquer integração

## Fluxo de Execução (Exemplo 1: Sucesso)

```
1. Client cria Saga: "Compra ETH/USDT + USD/BRL"
2. Client → Quotation Server: "Qual o preço de ETH/USDT e USD/BRL?"
   Quotation Server responde: ETH/USDT: R$1,300 | USD/BRL: R$5.00 (TTL: 300ms)
3. Client → Risk Server: "Análise estes preços"
   Risk Server aprova após latência (10-200ms)
4. Client → Purchase Server: "Compra ETH/USDT"
   Purchase Server executa com sucesso (20-300ms)
5. Client → Purchase Server: "Compra USD/BRL"
   Purchase Server executa com sucesso
6. Saga completa ✅
```

## Fluxo de Execução (Exemplo 2: Falha com Compensação)

```
1. Client cria Saga: "Compra ETH/USDT + USD/BRL"
2. Client → Quotation Server: Recebe ETH/USDT: R$1,250 | USD/BRL: R$4.90
3. Client → Risk Server: Aprova operação
4. Client → Purchase Server: Compra ETH/USDT ✅
5. Client → Purchase Server: Compra USD/BRL ❌ FALHA
6. Compensação: Client → Purchase Server: Vende ETH/USDT
7. Saga falha ❌ (com rollback)
```

## Compilação

```bash
make                # Compila todos os binários
make clean          # Remove binários e temporários
```

Binários gerados:
- `quotation_server`: Servidor de cotações
- `risk_server`: Servidor de análise de risco
- `purchase_server`: Servidor de execução de compra
- `trading_client`: Cliente que orquestra o trading

## Execução

### Opção 1: Teste Automatizado

```bash
./run_test.sh
```

Executa todos os 4 servidores e cliente com configuração padrão em 1 comando.

### Opção 2: Execução Manual

**Terminal 1 - Quotation Server:**
```bash
./quotation_server 9001
```

**Terminal 2 - Risk Server (configurável):**
```bash
# Sintaxe: ./risk_server [port] [success%] [min_latency_ms] [max_latency_ms]
./risk_server 9002 90 10 200
```

**Terminal 3 - Purchase Server (configurável):**
```bash
# Sintaxe: ./purchase_server [port] [success%] [min_latency_ms] [max_latency_ms]
./purchase_server 9100 95 20 300
```

**Terminal 4 - Trading Client:**
```bash
./trading_client 127.0.0.1 9001
```

### Opção 3: Usando Make

```bash
make run-server      # Quotation Server (porta 9001)
make run-risk        # Risk Server (porta 9002)
make run-purchase    # Purchase Server (porta 9100)
make run-client      # Trading Client
make run-both        # Quotation Server + Trading Client
```

## Configuração de Falhas

O sistema permite simular cenários de falha através de parâmetros:

### Risk Server
```bash
./risk_server 9002 70 100 500    # 70% sucesso, latência 100-500ms
./risk_server 9002 50 10 50      # 50% sucesso, latência 10-50ms (instável)
```

### Purchase Server
```bash
./purchase_server 9100 85 50 200  # 85% sucesso, latência 50-200ms
./purchase_server 9100 40 30 100  # 40% sucesso, latência 30-100ms (muito instável)
```

## Estruturas de Dados

### MessageHeader (TTL)
```c
typedef struct {
    uint32_t message_id;    // ID único da mensagem
    MessageType type;       // Tipo (QUOTE_REQUEST, TRADE_ORDER, etc)
    time_t timestamp;       // Timestamp de criação
    int32_t ttl;           // Time-To-Live em segundos
} MessageHeader;
```

### SagaContext (Processo Manager)
```c
typedef struct {
    uint32_t saga_id;                      // ID da Saga
    SagaState current_state;               // Estado atual
    char asset_codes[MAX_ASSETS][32];      // Códigos dos ativos (ex: ETH/USDT)
    float quantities[MAX_ASSETS];          // Quantidades
    float prices[MAX_ASSETS];              // Preços cotados
    float total_value;                     // Valor total da operação
    float risk_score;                      // Score de risco
} SagaContext;
```

### TradeOrder (Requisição)
```c
typedef struct {
    MessageHeader header;
    char asset_codes[MAX_ASSETS][32];
    float quantities[MAX_ASSETS];
    float prices[MAX_ASSETS];
    float total_value;
    float risk_score;
} TradeOrder;
```

### TradeResponse (Resposta)
```c
typedef struct {
    MessageHeader header;
    uint32_t order_id;
    int success;                    // 1 = sucesso, 0 = falha
    char status_message[256];       // Mensagem descritiva
} TradeResponse;
```

## Garantias do Sistema

✅ **Atomicidade**: Operação completa (2 compras) ou nenhuma (com compensação)
✅ **Confiabilidade**: TTL garante que dados expirados não são usados
✅ **Redundância**: Cada integração é um processo independente
✅ **Configurabilidade**: Taxa de sucesso e latência controláveis
✅ **Rastreabilidade**: Logs detalhados de cada etapa com IDs únicos

## Requisitos Atendidos

- ✅ Implementação de 4 processos distribuídos independentes
- ✅ Processamento de até N ordens de compra (parametrizável)
- ✅ 4 integrações confiáveis e atômicas (cotação, risco, compra1, compra2)
- ✅ TTL com aborto de operação se excedido
- ✅ Falha em qualquer ativo cancela operação com compensação
- ✅ 3 padrões de projeto distribuído:
  1. **Saga Distribuído** (orquestração + compensação)
  2. **Request-Reply Pattern** (RPC síncrono)
  3. **TTL / Message Expiration** (confiabilidade temporal)
- ✅ Taxa de sucesso configurável em cada servidor
- ✅ Latência configurável (sleep aleatório)

## Exemplos de Saída

### Execução Bem-Sucedida
```
INICIANDO SAGA #1
▶ Etapa 1: Solicitando cotação de PETR4 (100.00 unidades)
◄ Etapa 2: Aguardando cotação de PETR4...
✓ Cotação recebida: PETR4 @ R$25.50
▶ Etapa 3: Solicitando cotação de VALE5 (50.00 unidades)
◄ Etapa 4: Aguardando cotação de VALE5...
✓ Cotação recebida: VALE5 @ R$75.30
🔍 Etapa 5: Enviando dados ao Risk Server...
✅ Risk Server aprovou
✈ Etapa 6: Enviando ordens de compra ao Purchase Server...
✓ Compra 1 executada
✓ Compra 2 executada
✅ SAGA #1 COMPLETADA COM SUCESSO!
```

### Falha com Compensação
```
✓ Compra 1 executada
❌ Falha na compra do segundo ativo
⚠ Iniciando compensação: vendendo ativo 1
❌ SAGA #1 FALHOU!
```

## Notas Técnicas
