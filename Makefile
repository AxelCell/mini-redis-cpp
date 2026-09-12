CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I src

SRC  := src/resp.cpp src/store.cpp src/commands.cpp src/aof.cpp src/server.cpp
TEST := tests/test_resp.cpp src/resp.cpp

all: server

server: $(SRC) src/resp.hpp src/store.hpp src/commands.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: test_resp test_store test_commands test_aof
	./test_resp
	./test_store
	./test_commands
	./test_aof

test_resp: $(TEST) src/resp.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(TEST)

clean:
	rm -f server test_resp test_store test_commands test_aof

.PHONY: all test clean

test_store: tests/test_store.cpp src/store.cpp src/store.hpp
	$(CXX) $(CXXFLAGS) -o $@ tests/test_store.cpp src/store.cpp

test_commands: tests/test_commands.cpp src/commands.cpp src/store.cpp src/resp.cpp
	$(CXX) $(CXXFLAGS) -o $@ tests/test_commands.cpp src/commands.cpp src/store.cpp src/resp.cpp

test_aof: tests/test_aof.cpp src/aof.cpp src/store.cpp src/commands.cpp src/resp.cpp
	$(CXX) $(CXXFLAGS) -o $@ tests/test_aof.cpp src/aof.cpp src/store.cpp src/commands.cpp src/resp.cpp
