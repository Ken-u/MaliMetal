/*
 * How fast is each core, and where do threads land? The board has four Cortex-A55 (cores 0-3) and four Cortex-A76 (4-7), and the kernel's scheduler does
 * not know the difference (the registration gives every core the same cluster and type), so the render server's threads run on whatever core is idle.
 *
 * Two parts, both on the console through say():
 *   1. eight threads (QOS user-interactive, as the render server's encoding thread) run the same fixed work, six times each; the core a thread is on is
 *      in TPIDR_EL0 on arm64 XNU (what <os/tsd.h>'s _os_cpu_number() reads), read before and after each piece of work: only a piece that stayed on one
 *      core counts, and each core's best time is what that core can do.
 *   2. 32 times, a new thread runs a short piece of the work on an otherwise idle system and says which core the scheduler gave it, then the main thread
 *      does the same after a sleep (the way a woken thread lands).
 */
#include <pthread.h>
#include <pthread/qos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "usertest.h"

#define CORES 8
#define THREADS 8
#define REPS 6
#define CHASE_WORDS (32u * 1024u)    /* 128 KiB: inside the L2 of both kinds of core */
#define BIG_WORDS (4u * 1024u * 1024u)   /* 16 MiB: past every core's L2, in the L3 and the memory */
#define COPY_BYTES (8u << 20)

/* iOS 17's XNU keeps the core's number in the low 12 bits of TPIDR_EL0 and the logical cluster in the 8 bits above (what _os_cpu_number() reads). */
static inline unsigned cpu_number(void)
{
   uint64_t p;
   __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(p));
   return (unsigned)(p & 0xfff);
}

static inline unsigned cluster_number(void)
{
   uint64_t p;
   __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(p));
   return (unsigned)((p >> 12) & 0xff);
}

static double now_ns(void)
{
   return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* A dependent chain of multiply-adds: the clock, whatever the kind of core. */
static uint64_t work_alu(uint64_t iterations)
{
   uint64_t x = 0x9E3779B97F4A7C15ull;
   for (uint64_t i = 0; i < iterations; i++)
      x = x * 0xD6E8FEB86659FD93ull + i;
   return x;
}

/* Pointer chasing over a random cycle: the latency of a load that hits the L2. */
static uint32_t work_chase(const uint32_t *cycle, uint64_t steps)
{
   uint32_t at = 0;
   for (uint64_t i = 0; i < steps; i++)
      at = cycle[at];
   return at;
}

/* Data-dependent branches and table loads, a stand-in for general code (a driver's command recording, an interpreter): where a wide out-of-order core
 * with a good predictor and an in-order one part ways. */
static uint64_t work_mix(const uint32_t *table, uint32_t words, uint64_t operations)
{
   uint64_t rng = 88172645463325252ull, acc = 0;
   for (uint64_t i = 0; i < operations; i++) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      const uint32_t v = table[rng & (words - 1)];
      switch ((rng >> 40) & 7) {
      case 0: acc += v; break;
      case 1: acc ^= (uint64_t)v << 3; break;
      case 2: if (v & 1) acc += 3; else acc -= 1; break;
      case 3: acc = (acc << 1) | (v >> 31); break;
      case 4: acc += table[(acc + v) & (words - 1)]; break;
      case 5: if (v & 2) acc ^= rng; break;
      case 6: acc += (v & 0xff) * 5; break;
      default: acc -= v >> 7; break;
      }
   }
   return acc;
}

static void work_copy(uint8_t *dst, const uint8_t *src)
{
   memcpy(dst, src, COPY_BYTES);
   __asm__ volatile("" : : "r"(dst), "r"(src) : "memory");
}

static volatile uint64_t sink;

enum { P_ALU, P_CHASE, P_MIX, P_COPY, P_BIG_CHASE, P_BIG_MIX, PIECES };

typedef struct {
   double value[PIECES];       /* best per core (alu: ns per iteration, chase: ns per step, mix: ns per operation, copy: GB/s) */
   unsigned samples[PIECES];   /* how many times the piece ran start to end on that core */
   unsigned cluster;
} CoreBest;

static CoreBest best[CORES];
static pthread_mutex_t best_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *cycle_table, *big_table;
/* This platform has no pthread barriers: the workers spin until all of them are there (which also puts one on each core). */
static unsigned arrived;
static unsigned expected = ~0u;

typedef struct {
   unsigned index;
} Arg;

#define ALU_ITERATIONS 4000000ull
#define CHASE_STEPS 1500000ull
#define MIX_OPERATIONS 1500000ull
#define BIG_STEPS 150000ull

static void record(unsigned from, unsigned to, unsigned cluster, unsigned piece, double value)
{
   if (from != to || from >= CORES)
      return;
   pthread_mutex_lock(&best_lock);
   CoreBest *b = &best[from];
   b->cluster = cluster;
   if (!b->samples[piece] || (piece == P_COPY ? value > b->value[piece] : value < b->value[piece]))
      b->value[piece] = value;
   b->samples[piece]++;
   pthread_mutex_unlock(&best_lock);
}

static void *worker(void *argument)
{
   const Arg *arg = argument;
   uint8_t *src = malloc(COPY_BYTES), *dst = malloc(COPY_BYTES);
   if (!src || !dst)
      return NULL;
   memset(src, (int)arg->index + 1, COPY_BYTES);
   memset(dst, 0, COPY_BYTES);
   __atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
   while (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < __atomic_load_n(&expected, __ATOMIC_SEQ_CST))
      ;
   for (unsigned rep = 0; rep < REPS; rep++) {
      /* Each piece counts on its own, and only if the thread stayed on one core from its start to its end: a whole repetition is long enough for the
       * scheduler to move a thread once, and every core would then have no sample. */
      unsigned c0 = cpu_number(), cl = cluster_number();
      double t = now_ns();
      sink += work_alu(ALU_ITERATIONS);
      record(c0, cpu_number(), cl, P_ALU, (now_ns() - t) / (double)ALU_ITERATIONS);

      c0 = cpu_number(), cl = cluster_number();
      t = now_ns();
      sink += work_chase(cycle_table, CHASE_STEPS);
      record(c0, cpu_number(), cl, P_CHASE, (now_ns() - t) / (double)CHASE_STEPS);

      c0 = cpu_number(), cl = cluster_number();
      t = now_ns();
      sink += work_mix(cycle_table, CHASE_WORDS, MIX_OPERATIONS);
      record(c0, cpu_number(), cl, P_MIX, (now_ns() - t) / (double)MIX_OPERATIONS);

      c0 = cpu_number(), cl = cluster_number();
      t = now_ns();
      work_copy(dst, src);
      work_copy(src, dst);
      record(c0, cpu_number(), cl, P_COPY, 2.0 * COPY_BYTES / (now_ns() - t));   /* bytes per ns = GB/s */

      c0 = cpu_number(), cl = cluster_number();
      t = now_ns();
      sink += work_chase(big_table, BIG_STEPS);
      record(c0, cpu_number(), cl, P_BIG_CHASE, (now_ns() - t) / (double)BIG_STEPS);

      c0 = cpu_number(), cl = cluster_number();
      t = now_ns();
      sink += work_mix(big_table, BIG_WORDS, BIG_STEPS);
      record(c0, cpu_number(), cl, P_BIG_MIX, (now_ns() - t) / (double)BIG_STEPS);
   }
   free(src);
   free(dst);
   return NULL;
}

static unsigned landed[CORES];
static double landed_mix[CORES];

static void note_landing(unsigned c0, unsigned c1, double mix_ns)
{
   if (c0 == c1 && c0 < CORES) {
      landed[c0]++;
      if (!landed_mix[c0] || mix_ns < landed_mix[c0])
         landed_mix[c0] = mix_ns;
   }
}

static void *one_shot(void *unused)
{
   (void)unused;
   const unsigned c0 = cpu_number();
   const double t = now_ns();
   sink += work_mix(cycle_table, CHASE_WORDS, MIX_OPERATIONS / 4);
   const double mix = (now_ns() - t) / (double)(MIX_OPERATIONS / 4);
   note_landing(c0, cpu_number(), mix);
   return NULL;
}

void cpu_bench(void)
{
   cycle_table = malloc(CHASE_WORDS * sizeof(uint32_t));
   if (!cycle_table)
      return;
   /* One cycle through all the words, in a random order (Sattolo). */
   for (uint32_t i = 0; i < CHASE_WORDS; i++)
      cycle_table[i] = i;
   uint64_t rng = 0x2545F4914F6CDD1Dull;
   for (uint32_t i = CHASE_WORDS - 1; i > 0; i--) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      const uint32_t j = (uint32_t)(rng % i);
      const uint32_t t = cycle_table[i];
      cycle_table[i] = cycle_table[j];
      cycle_table[j] = t;
   }

   big_table = malloc((size_t)BIG_WORDS * sizeof(uint32_t));
   if (!big_table) {
      free(cycle_table);
      return;
   }
   for (uint32_t i = 0; i < BIG_WORDS; i++)
      big_table[i] = i;
   for (uint32_t i = BIG_WORDS - 1; i > 0; i--) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      const uint32_t j = (uint32_t)(rng % i);
      const uint32_t t = big_table[i];
      big_table[i] = big_table[j];
      big_table[j] = t;
   }

   say("cpu-bench: %u threads at user-interactive QoS run the same work %u times; best time of each core (alu: ns per dependent multiply-add, chase: ns per "
       "dependent load in 128 KiB, mix: ns per branchy operation in 128 KiB, copy: GB/s of memcpy over 8 MiB, big: the same two over 16 MiB)", THREADS, REPS);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
   pthread_t threads[THREADS];
   Arg args[THREADS];
   unsigned started = 0;
   for (unsigned i = 0; i < THREADS; i++) {
      args[i].index = i;
      if (pthread_create(&threads[i], &attr, worker, &args[i]) == 0)
         started++;
      else
         break;
   }
   __atomic_store_n(&expected, started, __ATOMIC_SEQ_CST);
   for (unsigned i = 0; i < started; i++)
      pthread_join(threads[i], NULL);
   for (unsigned c = 0; c < CORES; c++) {
      unsigned total = 0;
      char text[PIECES][16];
      static const char *const specs[PIECES] = {"%6.3f", "%6.2f", "%6.2f", "%5.2f", "%6.1f", "%6.1f"};
      for (unsigned p = 0; p < PIECES; p++) {
         total += best[c].samples[p];
         if (best[c].samples[p])
            snprintf(text[p], sizeof text[p], specs[p], best[c].value[p]);
         else
            snprintf(text[p], sizeof text[p], "     -");
      }
      if (!total) {
         say("cpu-bench: core %u: no piece of work ran there from start to end", c);
         continue;
      }
      say("cpu-bench: core %u (cluster %u): alu %s  chase %s  mix %s  copy %s  big chase %s  big mix %s  (pieces %u %u %u %u %u %u)", c, best[c].cluster,
          text[P_ALU], text[P_CHASE], text[P_MIX], text[P_COPY], text[P_BIG_CHASE], text[P_BIG_MIX], best[c].samples[P_ALU], best[c].samples[P_CHASE],
          best[c].samples[P_MIX], best[c].samples[P_COPY], best[c].samples[P_BIG_CHASE], best[c].samples[P_BIG_MIX]);
   }

   say("cpu-bench: where an idle system puts a new thread (32 times, QoS user-interactive, 2 ms apart), then the main thread after a 5 ms sleep (32 times)");
   memset(landed, 0, sizeof landed);
   memset(landed_mix, 0, sizeof landed_mix);
   for (unsigned i = 0; i < 32; i++) {
      pthread_t t;
      if (pthread_create(&t, &attr, one_shot, NULL) == 0)
         pthread_join(t, NULL);
      usleep(2000);
   }
   unsigned seen[CORES], total = 0;
   memcpy(seen, landed, sizeof seen);
   for (unsigned c = 0; c < CORES; c++)
      total += landed[c];
   {
      char line[256] = "";
      for (unsigned c = 0; c < CORES; c++)
         snprintf(line + strlen(line), sizeof line - strlen(line), " core%u %2u (mix %.1f)", c, seen[c], landed_mix[c]);
      say("cpu-bench: new threads (%u counted):%s", total, line);
   }
   memset(landed, 0, sizeof landed);
   memset(landed_mix, 0, sizeof landed_mix);
   for (unsigned i = 0; i < 32; i++) {
      usleep(5000);
      one_shot(NULL);
   }
   total = 0;
   for (unsigned c = 0; c < CORES; c++)
      total += landed[c];
   {
      char line[256] = "";
      for (unsigned c = 0; c < CORES; c++)
         snprintf(line + strlen(line), sizeof line - strlen(line), " core%u %2u (mix %.1f)", c, landed[c], landed_mix[c]);
      say("cpu-bench: the main thread after a sleep (%u counted):%s", total, line);
   }
   pthread_attr_destroy(&attr);
   free(cycle_table);
   free(big_table);
   cycle_table = big_table = NULL;
}
