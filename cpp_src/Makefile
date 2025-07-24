CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

all: orderbook

orderbook: orderbook.cpp
	$(CXX) $(CXXFLAGS) -o orderbook orderbook.cpp

clean:
	rm -f orderbook