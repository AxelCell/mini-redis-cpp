CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I src

SRC  := src/resp.cpp src/commands.cpp src/server.cpp
TEST := tests/test_resp.cpp src/resp.cpp

all: server

server: $(SRC) src/resp.hpp src/store.hpp src/commands.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: test_resp
	./test_resp

test_resp: $(TEST) src/resp.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(TEST)

clean:
	rm -f server test_resp

.PHONY: all test clean
