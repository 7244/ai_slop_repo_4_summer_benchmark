#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <new>

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef THREAD_COUNT
#error "THREAD_COUNT must be defined (see Makefile)"
#endif

#if THREAD_COUNT < 1 || THREAD_COUNT >= 0x100
#error "THREAD_COUNT must be in [1, 255] (SUMMER max_threads_t is uint8_t)"
#endif

#ifndef BENCH_TARGET
#define BENCH_TARGET 100000000000ULL
#endif

#ifndef BENCH_SYNC_MS
#define BENCH_SYNC_MS 128
#endif

#define COUNTER_COUNT (1ULL << 23)
#define READ_ENTRIES (1ULL << 30)

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

#define SUMMER_set_name bench_summer
#define SUMMER_set_max_threads THREAD_COUNT
#define SUMMER_set_elem_count 4096
#include "SUMMER.h"

#undef CONCAT
#undef CONCAT_IMPL

using namespace std::chrono;

static uint64_t g_progress;
static std::atomic<int> g_stop{0};
static std::atomic<int> g_ready{0};
static std::atomic<int> g_go{0};
static std::atomic<uint64_t> g_end_ns{0};
static std::atomic<uint64_t> g_sync_cas{0};
static std::atomic<uint64_t> g_sync_win{0};
static steady_clock::time_point g_t0;
static uint64_t g_iter[THREAD_COUNT];

static uint64_t g_counters[COUNTER_COUNT];
static uint64_t g_l_counters[THREAD_COUNT][COUNTER_COUNT];
static uint32_t g_last_sync[COUNTER_COUNT];
static bench_summer_t g_summer;

static uint64_t* g_read_buf;

static inline uint64_t xorshift64star(uint64_t& s)
{
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
}

static inline void pin_thread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if(sched_setaffinity(0, sizeof(set), &set) != 0){
        perror("sched_setaffinity");
        exit(1);
    }
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - g_t0).count();
}

static inline void check_progress(uint64_t iter)
{
    if((iter % 10000) == 0){
        if(__atomic_add_fetch(&g_progress, 10000, __ATOMIC_SEQ_CST) >= BENCH_TARGET){
            if(g_stop.exchange(1) == 0){
                g_end_ns.store((uint64_t)duration_cast<nanoseconds>(steady_clock::now() - g_t0).count(), std::memory_order_relaxed);
            }
        }
    }
}

template <bool COLLIDE>
static void bench_v1(int th)
{
    uint64_t state = 0x9E3779B97F4A7C15ULL ^ (uint64_t)th * 0xBF58476D1CE4E5B9ULL;
    uint64_t iter = 0;

    while(!g_stop.load(std::memory_order_relaxed)){
        state = xorshift64star(state);
        uint64_t v = g_read_buf[state & (READ_ENTRIES - 1)];
        asm volatile("" : "+r"(v) :: "memory");

        for(int k = 0; k < 8; ++k){
            state = xorshift64star(state);
            uint64_t i = COLLIDE ? k : state & (COUNTER_COUNT - 1);
            __atomic_add_fetch(&g_counters[i], 1, __ATOMIC_SEQ_CST);
        }

        ++iter;
        check_progress(iter);
    }

    g_iter[th] = iter;
}

template <bool COLLIDE>
static void bench_v2(int th)
{
    uint64_t state = 0x9E3779B97F4A7C15ULL ^ (uint64_t)th * 0xBF58476D1CE4E5B9ULL;
    uint64_t iter = 0;
    uint32_t now = 0;
    uint64_t* local = g_l_counters[th];

    while(!g_stop.load(std::memory_order_relaxed)){
        if((iter & 1023) == 0){
            now = now_ms();
        }

        state = xorshift64star(state);
        uint64_t v = g_read_buf[state & (READ_ENTRIES - 1)];
        asm volatile("" : "+r"(v) :: "memory");

        for(int k = 0; k < 8; ++k){
            state = xorshift64star(state);
            uint32_t i = COLLIDE ? k : state & (COUNTER_COUNT - 1);

            __atomic_add_fetch(&local[i], 1, __ATOMIC_RELAXED);

            if((uint32_t)(now - g_last_sync[i]) >= BENCH_SYNC_MS){
                g_sync_cas.fetch_add(1, std::memory_order_relaxed);
                uint32_t expected = __atomic_load_n(&g_last_sync[i], __ATOMIC_RELAXED);
                if(__atomic_compare_exchange_n(&g_last_sync[i], &expected, now, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)){
                    g_sync_win.fetch_add(1, std::memory_order_relaxed);
                    uint64_t sum = 0;
                    for(int t = 0; t < THREAD_COUNT; ++t){
                        sum += __atomic_load_n(&g_l_counters[t][i], __ATOMIC_RELAXED);
                    }
                    __atomic_exchange_n(&g_counters[i], sum, __ATOMIC_RELAXED);
                }
            }
        }

        ++iter;
        check_progress(iter);
    }

    g_iter[th] = iter;
}

template <bool COLLIDE>
static void bench_v3(int th)
{
    using key_t = bench_summer_t::thread_t::elem_t::key_t;

    uint64_t state = 0x9E3779B97F4A7C15ULL ^ (uint64_t)th * 0xBF58476D1CE4E5B9ULL;
    uint64_t iter = 0;

    while(!g_stop.load(std::memory_order_relaxed)){
        state = xorshift64star(state);
        uint64_t v = g_read_buf[state & (READ_ENTRIES - 1)];
        asm volatile("" : "+r"(v) :: "memory");

        for(int k = 0; k < 8; ++k){
            state = xorshift64star(state);
            key_t key{&g_counters[COLLIDE ? k : state & (COUNTER_COUNT - 1)]};
            g_summer.sum((uint8_t)th, key, 1);
        }

        ++iter;
        check_progress(iter);
    }

    g_iter[th] = iter;
}

static void run_thread(int th, int variant)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if(th < ncpu){
        pin_thread(th);
    }

    g_ready.fetch_add(1, std::memory_order_release);
    while(!g_go.load(std::memory_order_acquire)){
    }

    switch(variant){
        case 1: bench_v1<false>(th); break;
        case 2: bench_v2<false>(th); break;
        case 3: bench_v3<false>(th); break;
        case 4: bench_v1<true>(th); break;
        case 5: bench_v2<true>(th); break;
        case 6: bench_v3<true>(th); break;
    }
}

static void touch_memory(void* p, size_t sz)
{
    volatile char* c = (volatile char*)p;
    for(size_t i = 0; i < sz; i += 4096){
        c[i] = 0;
    }
}

static void run_variant(int variant)
{
    memset(g_counters, 0, sizeof(g_counters));
    memset(g_l_counters, 0, sizeof(g_l_counters));
    memset(g_last_sync, 0, sizeof(g_last_sync));
    memset(&g_summer, 0, sizeof(g_summer));
    g_progress = 0;
    g_stop.store(0, std::memory_order_relaxed);
    g_ready.store(0, std::memory_order_relaxed);
    g_go.store(0, std::memory_order_relaxed);
    g_end_ns.store(0, std::memory_order_relaxed);
    g_sync_cas.store(0, std::memory_order_relaxed);
    g_sync_win.store(0, std::memory_order_relaxed);
    memset(g_iter, 0, sizeof(g_iter));

    std::thread threads[THREAD_COUNT];
    for(int i = 0; i < THREAD_COUNT; ++i){
        threads[i] = std::thread(run_thread, i, variant);
    }

    while(g_ready.load(std::memory_order_acquire) != THREAD_COUNT){
    }

    g_t0 = steady_clock::now();
    g_go.store(1, std::memory_order_release);

    for(int i = 0; i < THREAD_COUNT; ++i){
        threads[i].join();
    }

    uint64_t end_ns = g_end_ns.load(std::memory_order_relaxed);
    uint64_t total_iter = 0;
    for(int i = 0; i < THREAD_COUNT; ++i){
        total_iter += g_iter[i];
    }

    static const char* const variant_names[6] = {
        "atomic",
        "local+batch",
        "summer",
        "atomic, colliding",
        "local+batch, colliding",
        "summer, colliding",
    };

    printf("variant %d (%s): %llu ms (%llu total iterations, %llu progress, %llu cas, %llu syncs)\n",
           variant,
           variant_names[variant - 1],
           (unsigned long long)(end_ns / 1000000ULL),
           (unsigned long long)total_iter,
           (unsigned long long)g_progress,
           (unsigned long long)g_sync_cas.load(std::memory_order_relaxed),
           (unsigned long long)g_sync_win.load(std::memory_order_relaxed));
}

int main(int argc, char** argv)
{
    int variant = 0;
    if(argc == 2){
        variant = atoi(argv[1]);
        if(variant < 1 || variant > 6){
            fprintf(stderr, "usage: %s [<1|2|3|4|5|6>] (no arg runs all variants)\n", argv[0]);
            return 1;
        }
    }

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if(ncpu < THREAD_COUNT){
        fprintf(stderr, "warning: THREAD_COUNT=%d > online cpus %ld, some threads will be oversubscribed\n", THREAD_COUNT, ncpu);
    }

    size_t buf_size = (size_t)READ_ENTRIES * sizeof(uint64_t);
    void* p = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(p == MAP_FAILED){
        perror("mmap");
        return 1;
    }
    madvise(p, buf_size, MADV_HUGEPAGE);
    touch_memory(p, buf_size);
    g_read_buf = (uint64_t*)p;

    if(variant){
        run_variant(variant);
    }
    else{
        for(variant = 1; variant <= 6; ++variant){
            run_variant(variant);
        }
    }

    munmap(p, buf_size);
    return 0;
}
