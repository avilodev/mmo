CC = gcc
CFLAGS = -Wall -Wextra -g -pthread -I/usr/include/postgresql -I../../database/include -I../../common/include -I../../realm_server/include
LDFLAGS = -lpq -lpthread -lhiredis -lm

# Source paths (relative to tests/3/)
DB_SRC_DIR = ../../database/src

# Object files
OBJ = database_operations.o

# Test executable
TEST = test_character_creation

all: $(TEST)

$(TEST): test_character_creation.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

database_operations.o: $(DB_SRC_DIR)/database_operations.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TEST)

run-test: $(TEST)
	./$(TEST)

.PHONY: all clean run-test