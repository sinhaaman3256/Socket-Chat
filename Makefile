CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -g -D_GNU_SOURCE
LDFLAGS :=

PROJECT := chat
BUILD_DIR := build
SRC_DIR := src
COMMON_DIR := $(SRC_DIR)/common
SERVER_DIR := $(SRC_DIR)/server
CLIENT_DIR := $(SRC_DIR)/client
INCLUDE_DIRS := -I$(COMMON_DIR)

SERVER_SRCS := \
    $(COMMON_DIR)/common.cpp \
    $(SERVER_DIR)/server.cpp

CLIENT_SRCS := \
    $(COMMON_DIR)/common.cpp \
    $(CLIENT_DIR)/client.cpp

SERVER_OBJS := $(SERVER_SRCS:%.cpp=$(BUILD_DIR)/%.o)
CLIENT_OBJS := $(CLIENT_SRCS:%.cpp=$(BUILD_DIR)/%.o)

SERVER_BIN := $(BUILD_DIR)/src/server/server
CLIENT_BIN := $(BUILD_DIR)/src/client/client

.PHONY: all clean dirs

all: dirs $(SERVER_BIN) $(CLIENT_BIN)

dirs:
	mkdir -p $(BUILD_DIR)/$(COMMON_DIR) $(BUILD_DIR)/$(SERVER_DIR) $(BUILD_DIR)/$(CLIENT_DIR)

$(BUILD_DIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDE_DIRS) -c $< -o $@

$(SERVER_BIN): $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

print-%:
	@echo $*=$($*)



