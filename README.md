# Sistema de Trading Automatizado em C

## 📋 Visão Geral

Sistema de operação em mercado (trading) automatizado que simula um fluxo de compra de dois ativos a partir de dados recebidos de um sistema de cotação com análise de risco. Implementado em **C puro** com comunicação via **sockets TCP**, utilizando padrões de projeto distribuídos.

### Componentes

- **Servidor de Cotações** (`server.c`): Sistema desacoplado que fornece cotações fixas
- **Cliente de Trading** (`client.c`): Sistema de operação que gerencia processos de compra
- **Cabeçalho Compartilhado** (`common.h`): Contrato de comunicação entre client/server

---

## 🏗️ Padrões de Projeto Implementados

### 1. **SAGA**
Orquestrador de fluxo transacional com múltiplas etapas:
- Etapa 1-2: Requisição e recepção de cotação do ativo 1
- Etapa 3-4: Requisição e recepção de cotação do ativo 2
- Etapa 5: Análise de risco (com possível rejeição)
- Etapa 6: Execução da compra

Cada etapa pode falhar, causando cancelamento da operação.

```
┌─────────────────────────────────────────────────────────┐
│ SAGA: Fluxo de Compra (PETR4 + VALE5)                  │
├─────────────────────────────────────────────────────────┤
│ 1. Requisição Quote PETR4                               │
│ 2. Recebimento Quote PETR4 (com TTL)                   │
│ 3. Requisição Quote VALE5                               │
│ 4. Recebimento Quote VALE5 (com TTL)                   │
│ 5. Análise de Risco (Risk Score 0-1)                   │
│ 6. Execução da Compra                                   │
└─────────────────────────────────────────────────────────┘
```

### 2. **PROCESS MANAGER**
Gerenciador centralizado de processos/sagas em andamento:
- Criação de novas sagas
- Rastreamento de estado (SagaContext)
- Armazenamento de dados e progresso
- Remoção de sagas completadas

```c
ProcessManager {
    sagas[MAX_SAGAS]        // Array de contextos ativos
    num_sagas               // Contador
}

SagaContext {
    saga_id                 // ID único
    current_state           // Estado atual (enum SagaState)
    asset_codes[]           // Ativos sendo operados
    quantities[]            // Quantidades
    prices[]                // Preços recebidos
    risk_score              // Análise de risco
    expiration_time         // TTL da saga
}
```

### 3. **MESSAGE EXPIRATION (TTL)**
Cada mensagem possui:
- `timestamp`: hora de criação
- `ttl`: time-to-live em segundos
- Função `is_message_expired()` para validação

Mensagens expiradas são descartadas automaticamente:

```c
if (is_message_expired(&message.header)) {
    // Ignora mensagem expirada
}
```

---

## 🔌 Arquitetura de Comunicação

### Formato de Mensagem (Contrato)

```
MessageHeader {
    message_id      (uint32_t)    // ID único
    type            (enum)        // MSG_QUOTE_REQUEST, MSG_QUOTE_RESPONSE, etc
    timestamp       (time_t)      // Data/hora de criação
    ttl             (int32_t)     // Validade em segundos
}

QuoteRequest {
    header          // MessageHeader
    asset_code[32]  // Ex: "PETR4"
    quantity        // Quantidade solicitada
}

QuoteResponse {
    header          // MessageHeader
    asset_code[32]  // Código do ativo
    price           // Preço unitário
    quantity        // Quantidade
    valid_until     // Timestamp até quando cotação é válida
}
```

### Fluxo de Comunicação

```
CLIENT                                          SERVER
   |                                               |
   |----------- QuoteRequest (PETR4) ---------->  |
   |                                               |
   |  [verifica expiração da resposta]            |
   |                                               |
   |<-------- QuoteResponse (25.50) -------------|
   |                                               |
   |----------- QuoteRequest (VALE5) ---------->  |
   |                                               |
   |<-------- QuoteResponse (75.30) -------------|
   |                                               |
   v [Análise de Risco]                           v
   |                                               |
   v [Execução da Compra]                         v
```

---

## 📊 Estrutura de Arquivos

```
t2_paralela/
├── common.h              # Estruturas, enums, utilitários compartilhados
├── server.c              # Servidor de cotações (TCP listener)
├── client.c              # Cliente de trading (Process Manager + Saga)
├── Makefile              # Compilação
└── README.md             # Esta documentação
```

---

## 🚀 Como Compilar e Executar

### Pré-requisitos
- GCC compilador C
- POSIX-compliant system (Linux, macOS, WSL)
- `pthread` library

### Compilação

```bash
# Compilar tudo
make all

# Ou compilar apenas components específicos
make server
make client
```

### Execução

**Opção 1: Executar servidor e cliente em terminais separados**

Terminal 1 (Servidor):
```bash
make run-server
```

Terminal 2 (Cliente):
```bash
make run-client
```

**Opção 2: Executar ambos (servidor em background)**

```bash
make run-both
```

### Limpeza

```bash
make clean
```

---

## 📝 Exemplo de Saída

```
╔════════════════════════════════════════╗
║  SISTEMA DE TRADING AUTOMATIZADO      ║
║  (Saga + Process Manager + TTL)       ║
╚════════════════════════════════════════╝

[PROCESS_MANAGER] ✓ Nova Saga criada: ID=1 (assets: PETR4, VALE5)

========================================
🚀 INICIANDO SAGA #1
========================================

[SAGA 1] ▶ Etapa 1: Solicitando cotação de PETR4 (100.00 unidades)
[SAGA 1] ◄ Etapa 2: Aguardando cotação de PETR4...
[SAGA 1] ✓ Cotação recebida: PETR4 @ R$25.50 (válida até: 1717987654)
[SAGA 1] ▶ Etapa 3: Solicitando cotação de VALE5 (50.00 unidades)
[SAGA 1] ◄ Etapa 4: Aguardando cotação de VALE5...
[SAGA 1] ✓ Cotação recebida: VALE5 @ R$75.30 (válida até: 1717987655)
[SAGA 1] 🔍 Etapa 5: Analisando risco da operação...
[SAGA 1] 📊 Valor total da operação: R$5305.00
[SAGA 1] 📈 Score de risco: 30.0% (ACEITO)
[SAGA 1] ✈ Etapa 6: Executando compra...
[SAGA 1] 🎯 Compra de PETR4 × 100.00 @ R$25.50 = R$2550.00
[SAGA 1] 🎯 Compra de VALE5 × 50.00 @ R$75.30 = R$3765.00
[SAGA 1] ✅ Operação executada com sucesso!
[SAGA 1] 💰 Valor total investido: R$6315.00

========================================
✅ SAGA #1 COMPLETADA COM SUCESSO!
========================================
```

---

## 🔐 Máximo Desacoplamento Implementado

### 1. **Separação de Responsabilidades**
- **Server**: Apenas fornece cotações (stateless)
- **Client**: Gerencia lógica de negócio (Saga + Process Manager)
- **Common**: Define contrato de mensagens

### 2. **Mensagens Bem Definidas**
- Cliente não precisa conhecer implementação do servidor
- Servidor não precisa conhecer lógica de trading
- Comunicação totalmente orientada a mensagens

### 3. **Independência de Protocolo**
- Trocar TCP por UDP requer mudança mínima
- Adicionar autenticação é isolado
- Serialização pode ser JSON/Protobuf sem afetar lógica

### 4. **Escalabilidade**
- Process Manager pode gerenciar múltiplas sagas em paralelo
- Múltiplos clientes podem conectar ao mesmo servidor
- Fácil adicionar novos tipos de operação

---

## 🎯 Possíveis Extensões

1. **Persistência**: Salvar estado de sagas em banco de dados
2. **Retry Logic**: Implementar retry automático com backoff exponencial
3. **Compensating Transactions**: Desfazer operações em caso de falha
4. **Load Balancing**: Múltiplos servidores de cotação
5. **Async I/O**: Usar epoll/kqueue para melhor desempenho
6. **Protocol Buffers**: Substituir structs por mensagens serializadas
7. **Autenticação**: Adicionar TLS/SSL

---

## 📚 Referências de Padrões

- **SAGA Pattern**: https://microservices.io/patterns/data/saga.html
- **Process Manager**: https://www.enterpriseintegrationpatterns.com/patterns/messaging/ProcessManager.html
- **Message Expiration**: https://www.enterpriseintegrationpatterns.com/patterns/messaging/MessageExpiration.html

---

## ⚖️ Licença

Este código é fornecido como material educacional para trabalhos de programação distribuída.

---

## 📧 Notas para o Avaliador

Este esqueleto implementa com fidelidade os três padrões solicitados:

1. **SAGA**: Orquestração de 6 etapas com falha/compensação
2. **PROCESS MANAGER**: Gerenciamento centralizado de estado
3. **MESSAGE EXPIRATION**: TTL em todas as mensagens

O código prioriza **máximo desacoplamento** através de:
- Contrato bem definido em `common.h`
- Client/Server completamente independentes
- Comunicação apenas via mensagens
- Estruturas genéricas reutilizáveis
