# Compiler and Compile options.
C = gcc
CXXFLAGS = -g -Wall

# Macros specifying path for compile.
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
BIN = ./bin/
INC = ./include/

# Pre-Processor.
CPPFLAGS += -I$(INC)

# Compile command.
TARGET = shell
$(TARGET):$(OBJS)
		$(CC) $(CPPFLAGS) -o $(BIN)$(TARGET) $(SRCS)

# Delete binary & object files.
clean:
	rm $(BIN)$(TARGET) $(OBJS)
