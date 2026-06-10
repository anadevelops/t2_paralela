.PHONY: all clean server client run-server run-client

# Compilador e flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -O2
LDFLAGS = -lpthread

# Targets
SERVER = quotation_server
CLIENT = trading_client

all: $(SERVER) $(CLIENT)

$(SERVER): server.c common.h
	$(CC) $(CFLAGS) -o $(SERVER) server.c $(LDFLAGS)
	@echo "✓ Servidor compilado: $(SERVER)"

$(CLIENT): client.c common.h
	$(CC) $(CFLAGS) -o $(CLIENT) client.c $(LDFLAGS)
	@echo "✓ Cliente compilado: $(CLIENT)"

# Executar servidor em background
run-server: $(SERVER)
	./$(SERVER) 9001

# Executar cliente (conecta ao servidor)
run-client: $(CLIENT)
	./$(CLIENT) 127.0.0.1 9001

# Executar ambos (servidor em background, cliente em foreground)
run-both: $(SERVER) $(CLIENT)
	./$(SERVER) 9001 &
	sleep 1
	./$(CLIENT) 127.0.0.1 9001

# Limpeza
clean:
	rm -f $(SERVER) $(CLIENT) *.o
	@echo "✓ Arquivos temporários removidos"

# Ajuda
help:
	@echo "Alvos disponíveis:"
	@echo "  make all         - Compila server e client"
	@echo "  make server      - Compila apenas o servidor"
	@echo "  make client      - Compila apenas o cliente"
	@echo "  make run-server  - Executa o servidor"
	@echo "  make run-client  - Executa o cliente"
	@echo "  make run-both    - Executa servidor + cliente"
	@echo "  make clean       - Remove binários"
