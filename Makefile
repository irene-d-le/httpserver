# Makefile for httpserver

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -O2 -pthread -D_POSIX_C_SOURCE=200809L
TARGET  = httpserver
SRC     = httpserver.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Build successful: ./$(TARGET)"

# Quick smoke-test: start server for 3 seconds, curl /health, stop.
test: $(TARGET)
	@echo "--- Starting server on port 9090 with 4 workers ---"
	./$(TARGET) 9090 4 /tmp/test_audit.log &
	@sleep 1
	curl -s http://localhost:9090/health
	@echo ""
	curl -s http://localhost:9090/echo
	@echo ""
	@kill $$(lsof -t -i:9090) 2>/dev/null || true
	@echo "--- Test done. Audit log: /tmp/test_audit.log ---"
	@cat /tmp/test_audit.log

clean:
	rm -f $(TARGET)