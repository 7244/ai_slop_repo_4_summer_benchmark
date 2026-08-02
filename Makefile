THREAD_COUNT ?= $(shell nproc)
ITER ?= 100000000000
BENCH_TARGET ?= $(ITER)
BENCH_SYNC_MS ?= 128

CXX ?= g++
CXXFLAGS ?= -O2 -march=native -std=c++20 -pthread
CPPFLAGS += -DTHREAD_COUNT=$(THREAD_COUNT) -DBENCH_TARGET=$(BENCH_TARGET) -DBENCH_SYNC_MS=$(BENCH_SYNC_MS)

.PHONY: run run1 run2 run3 run4 run5 run6 clean

run: bench
	./bench

SUMMER.h:
	curl -fsSL -o $@ https://raw.githubusercontent.com/7244/SUMMER/main/SUMMER.h

bench: bench.cpp SUMMER.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ bench.cpp

run1 run2 run3 run4 run5 run6: bench
	./bench $(patsubst run%,%,$@)

%:
	$(MAKE) run ITER=$@

clean:
	rm -f bench
