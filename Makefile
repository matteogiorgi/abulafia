# Makefile for abulafia - a toy Markov-chain chatbot.
#
# Targets:
#   make          build the "abulafia" executable (default target)
#   make run      build (if needed) and launch it against FILE
#   make clean    remove build artifacts

CC     := gcc
CFLAGS := -Wall -Wextra -O2 -std=c11

TARGET := abulafia
SRC    := abulafia.c

# Text file fed to the program by "make run". Override on the command
# line, e.g.:  make run FILE=mycorpus.txt
FILE ?= corpus.txt

# Targets that don't correspond to actual files with that name.
.PHONY: all run clean

# Default target: just build the executable.
all: $(TARGET)

# Single translation unit, so building is a straight compile+link in one step.
# Re-runs automatically whenever abulafia.c is newer than the binary.
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# Build (if needed) and start the interactive chatbot on $(FILE).
run: $(TARGET)
	./$(TARGET) $(FILE)

# Remove the compiled binary.
clean:
	rm -f $(TARGET)
