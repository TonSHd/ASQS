# ASQS - Application-Specific Quantum Simulation
# Zero-dependency build: g++ (C++20), GNU make. No external libraries.

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wshadow -pthread
BUILD    := build
CPPSRC   := cpp/src
INCLUDE  := cpp/include

SRCS := $(wildcard $(CPPSRC)/*.cpp)
OBJS := $(patsubst $(CPPSRC)/%.cpp,$(BUILD)/%.o,$(SRCS))
TEST_OBJS := $(BUILD)/test_main.o

.PHONY: all clean tests asqsd

all: asqsd

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(CPPSRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/asqsd: $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

$(BUILD)/asqs_tests: $(filter-out $(BUILD)/main.o,$(OBJS)) $(BUILD)/test_main.o
	$(CXX) $(CXXFLAGS) $(filter-out $(BUILD)/main.o,$(OBJS)) $(BUILD)/test_main.o -o $@

$(BUILD)/test_main.o: cpp/tests/test_main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE) -c $< -o $@

asqsd: $(BUILD)/asqsd

tests: $(BUILD)/asqs_tests
	./$(BUILD)/asqs_tests

clean:
	rm -rf $(BUILD)
