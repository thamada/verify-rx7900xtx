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
	@printf '%s\n' \
	  '#include <linux/kfd_ioctl.h>' \
	  '#ifndef KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS' \
	  '#error missing' \
	  '#endif' \
	  | $(CXX) $(CXXFLAGS) -fsyntax-only -x c++ - >/dev/null 2>&1 \
	  || { \
	    lead=; reset=; \
	    if [ -t 2 ] && [ -z "$${NO_COLOR:-}" ]; then \
	      lead=$$(printf '\033[1;91m'); \
	      reset=$$(printf '\033[0m'); \
	    fi; \
	    printf '%s%s%s\n' "$$lead" "NOTE: linux/kfd_ioctl.h does not define KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS." "$$reset"; \
	    printf '%s%s%s\n' "$$lead" "      This build uses the upstream flag value (1 << 23)." "$$reset"; \
	    printf '%s%s%s\n' "$$lead" "      Contiguous VRAM tests run only when the kernel reports KFD ioctl 1.16 or newer." "$$reset"; \
	    printf '%s%s%s\n' "$$lead" "      A full run needs a newer KFD UAPI and a matching kernel/amdgpu/KFD driver." "$$reset"; \
	  }
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
