# kfd-public-vram. Everything for this tool lives in this directory.
#
#   make
#   make build
#   make clean

ROCM ?= /opt/rocm
CXX  ?= g++

TARGET := kfd-public-vram

CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -D__HIP_PLATFORM_AMD__ -I$(ROCM)/include -pthread
LDFLAGS  := -L$(ROCM)/lib -lamdhip64 -Wl,-rpath,$(ROCM)/lib -pthread

.PHONY: build run clean

.DEFAULT_GOAL := run

build: $(TARGET)

$(TARGET): kfd_public_vram.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
