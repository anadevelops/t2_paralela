#!/bin/bash

# Script para testar cenário de FALHA com COMPENSAÇÃO

set -e

cd "$(dirname "$0")"

echo "=== Teste de Falha com Compensação ==="
echo "Configurando Purchase Server com 50% sucesso (alta taxa de falha)"
echo ""

# Inicia servidor de cotações em background
./quotation_server 9001 > /tmp/quotation_server.log 2>&1 &
QUOTE_PID=$!

# Inicia servidor de risco com 100% sucesso
./risk_server 9002 100 10 50 > /tmp/risk_server.log 2>&1 &
RISK_PID=$!

# Inicia servidor de compra com BAIXA taxa de sucesso (50%)
./purchase_server 9100 50 20 100 > /tmp/purchase_server.log 2>&1 &
PURCHASE_PID=$!

# Aguarda que os servidores estejam prontos
sleep 2

echo "[Executando cliente...]"
echo ""

# Executa o cliente
./trading_client 127.0.0.1 9001

# Aguarda um pouco
sleep 1

# Mata os servidores
kill $QUOTE_PID 2>/dev/null || true
kill $RISK_PID 2>/dev/null || true
kill $PURCHASE_PID 2>/dev/null || true

echo ""
echo "=== Teste Concluído ==="
echo ""
echo "Com sucesso em 50% das compras, há alta probabilidade de"
echo "que a segunda compra falhe, acionando a COMPENSAÇÃO (venda do primeiro ativo)"
