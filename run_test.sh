#!/bin/bash

# Script para executar o teste completo do sistema de trading

set -e

cd "$(dirname "$0")"

echo "=== Iniciando Servidores ==="

# Inicia servidor de cotações em background
echo "[1/4] Iniciando Quotation Server (porta 9001)..."
./quotation_server 9001 > /tmp/quotation_server.log 2>&1 &
QUOTE_PID=$!

# Inicia servidor de risco em background
echo "[2/4] Iniciando Risk Server (porta 9002, success=90%, latency=10-200ms)..."
./risk_server 9002 90 10 200 > /tmp/risk_server.log 2>&1 &
RISK_PID=$!

# Inicia servidor de compra em background
echo "[3/4] Iniciando Purchase Server (porta 9100, success=95%, latency=20-300ms)..."
./purchase_server 9100 95 20 300 > /tmp/purchase_server.log 2>&1 &
PURCHASE_PID=$!

# Aguarda que os servidores estejam prontos
sleep 2

# Executa o cliente
echo "[4/4] Iniciando Trading Client..."
./trading_client 127.0.0.1 9001

# Aguarda um pouco para os servidores processarem
sleep 1

# Mata os servidores
echo ""
echo "=== Encerrando Servidores ==="
kill $QUOTE_PID 2>/dev/null || true
kill $RISK_PID 2>/dev/null || true
kill $PURCHASE_PID 2>/dev/null || true

echo ""
echo "=== Logs dos Servidores ==="
echo ""
echo "--- Quotation Server ---"
head -20 /tmp/quotation_server.log
echo ""
echo "--- Risk Server ---"
head -20 /tmp/risk_server.log
echo ""
echo "--- Purchase Server ---"
head -20 /tmp/purchase_server.log

echo ""
echo "✓ Teste concluído!"
