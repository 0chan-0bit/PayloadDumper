# payload-dumper-cpp — high-performance Android OTA payload extractor.
#
# Defaults to -march=native because this project is tuned for the machine it
# is built on. Use `make NATIVE=0` for x86-64-v3, or
# `make pgo` for a profile-guided build.

CXX      ?= g++
BUILD    ?= build
TARGET   ?= $(BUILD)/payload-dumper

SRCS := $(wildcard src/*.cpp) src/proto/update_metadata.pb.cc
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(filter %.cpp,$(SRCS))) \
        $(patsubst %.cc,$(BUILD)/%.o,$(filter %.cc,$(SRCS)))
DEPS := $(OBJS:.o=.d)

WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
OPT      := -O3 -DNDEBUG
NATIVE   ?= 1

ifeq ($(NATIVE),1)
  ARCH := -march=native -mtune=native
else
  ARCH := -march=x86-64-v3 -mtune=generic
endif

ifeq ($(DEBUG),1)
  OPT  := -O1 -g
  LTO  :=
else
  LTO  := -flto=auto -fuse-linker-plugin
endif

CXXFLAGS ?= -std=c++20 $(OPT) $(ARCH) -fno-plt -fno-semantic-interposition \
            -ffunction-sections -fdata-sections $(WARN) $(LTO)
CPPFLAGS += -Isrc -Isrc/proto
LDLIBS   := -lprotobuf-lite -lzstd -llzma -lbz2 -lbrotlidec -lz -lcrypto -lpthread
LDFLAGS  += $(LTO) -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed -Wl,-z,relro,-z,now

.PHONY: all clean test bench pgo gui
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD)

$(BUILD)/operation-scheduler-test: tests/operation_scheduler_test.cpp src/operation_scheduler.h src/pool.h
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) -std=c++20 -O2 -pthread $< -o $@

test: $(TARGET) $(BUILD)/operation-scheduler-test
	$(BUILD)/operation-scheduler-test
	python3 tests/run_tests.py

bench: $(TARGET)
	python3 tests/bench.py

pgo:
	bash tools/pgo.sh

gui: $(TARGET)
	PD_BIN="$(abspath $(TARGET))" ./ui/payload-dumper-gui
