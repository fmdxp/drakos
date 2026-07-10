CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2

TARGET = drk-packager
SRCS = main.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)
