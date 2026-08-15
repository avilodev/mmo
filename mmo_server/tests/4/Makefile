CC = gcc
CFLAGS = -Wall -Wextra -g -pthread -I../../database/include -I../../common/include -I../../realm_server/include -I../../world_server/data
LDFLAGS = -lpthread -lm

# Test executable
TEST = test

all: $(TEST)

$(TEST): test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built integration test -> $@"

clean:
	rm -f $(TEST)

run: $(TEST)
	./$(TEST)

# Run with custom username
run-user: $(TEST)
	./$(TEST) CustomUser123

# Run with custom username and password
run-full: $(TEST)
	./$(TEST) CustomUser123 MySecurePass456

test_items_enhanced: test_items_enhanced.c
	gcc -o test_items_enhanced test_items_enhanced.c -lpthread

test_items: test_items_enhanced
	./test_items_enhanced

.PHONY: test_items

help:
	@echo "Integration Test Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make         - Build the integration test"
	@echo "  make run     - Build and run with auto-generated username"
	@echo "  make run-user   - Run with: ./integration_test CustomUser123"
	@echo "  make run-full   - Run with: ./integration_test CustomUser123 MySecurePass456"
	@echo "  make clean   - Remove built files"
	@echo ""
	@echo "Manual usage:"
	@echo "  ./test                     # Auto-generate username"
	@echo "  ./test MyUsername          # Custom username"
	@echo "  ./test MyUser MyPass       # Custom username and password"

.PHONY: all clean run run-user run-full help