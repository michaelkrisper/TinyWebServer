# TinyWebServer - Makefile

CC ?= gcc
CFLAGS = -O3 -Wall -Wextra -pthread
LDFLAGS = -lpthread

# Link liburing on Linux if available
ifeq ($(shell uname -s 2>/dev/null),Linux)
  ifneq ($(wildcard /usr/include/liburing.h),)
    LDFLAGS += -luring
  endif
endif
TARGET = server

.PHONY: all clean bench test

all: $(TARGET)

$(TARGET): server.c
	@echo "Compiling $(TARGET) with $(CC)..."
	$(CC) $(CFLAGS) -o $(TARGET) server.c $(LDFLAGS)
	@echo "Successfully built $(TARGET)!"

clean:
	@echo "Cleaning up build artifacts..."
	rm -f $(TARGET)
	@echo "Clean complete."

bench:
	@echo "Running Professional Benchmark (Bombardier)..."
	python3 tests/bench_bombardier.py

test:
	@echo "Running ETag Cache Tests..."
	SERVER_URL="http://127.0.0.1:80" python3 tests/test.py
