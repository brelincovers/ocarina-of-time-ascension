"""CPU-only checks of the current replay profiler, compiled with ASan/UBSan.

The fixture extracts the exact cp_* implementation from nvn_mtl.m. It replaces
only the clocks and log sink; no copied implementation, Metal device, host build
or gameplay is involved. Thread-local state and frame transitions are real.
Mode 2 reports replay windows: interleaved queues can produce multiple windows
for the same logical frame. Nested phase durations are deliberately inclusive.
"""
import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'nvn/nvn_mtl.m'
PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
static _Thread_local uint64_t wall = 1000000000, cpu = 100000000, clocks;
static atomic_int logs;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char output[12000];
static uint64_t fake_clock(int id) {
    clocks++; return id == CLOCK_THREAD_CPUTIME_ID ? cpu : wall;
}
static void test_log(const char *format, ...) {
    pthread_mutex_lock(&log_mu);
    size_t n = strlen(output); va_list args; va_start(args, format);
    int added = vsnprintf(output + n, sizeof output - n, format, args);
    va_end(args);
    assert(added >= 0 && (size_t)added + n + 1 < sizeof output);
    strcat(output, "\n"); atomic_fetch_add(&logs, 1);
    pthread_mutex_unlock(&log_mu);
}
#define clock_gettime_nsec_np fake_clock
#define MLOG test_log
'''

HARNESS = r'''
#undef clock_gettime_nsec_np
static int mode;
static void work(int phase, uint64_t w, uint64_t c) {
    cp_stamp s = cp_begin(); wall += w; cpu += c; cp_end(phase, s);
}
static unsigned occurrences(const char *needle) {
    unsigned n = 0;
    for (const char *p = output; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
static void *worker(void *arg) {
    uint64_t frame = (uintptr_t)arg;
    cp_frame(frame);
    if (!mode) {
        work(4, 60000000, 20000000); cp_report(0); cp_frame(frame + 1);
        assert(clocks == 0 && logs == 0); return NULL;
    }
    if (mode == 1) {
        work(4, 2000000000, 1000000000);
        uint64_t calls = clocks; cp_report(0);
        assert(clocks == calls && logs == 0);
        work(4, 3000000000, 1000000000); calls = clocks; cp_report(0);
        assert(clocks == calls && logs == 1);
        assert(strstr(output, "texture 2 calls 2000.00/5000.00 ms CPU/wall max-call 3000.00 ms"));
        return NULL;
    }
    if (mode == 3) {
        // Two queues can alternate frames on ONE replay thread. Returning to a
        // frame starts a fresh window, not a fabricated logical-frame total.
        work(4, 60000000, 20000000); cp_frame(1064);
        work(4, 70000000, 30000000); cp_frame(1053);
        work(4, 80000000, 40000000); cp_frame(1065);
        assert(logs == 3);
        assert(occurrences("frame 1053 thread") == 2);
        assert(occurrences("frame 1064 thread") == 1);
        assert(strstr(output, "texture 1 calls 20.00/60.00 ms CPU/wall"));
        assert(strstr(output, "texture 1 calls 30.00/70.00 ms CPU/wall"));
        assert(strstr(output, "texture 1 calls 40.00/80.00 ms CPU/wall"));
        return NULL;
    }
    // Nested samples overlap; they must retain their separate inclusive totals.
    cp_stamp outer = cp_begin(); work(4, 60000000, 20000000);
    wall += 10000000; cpu += 5000000; cp_end(5, outer);
    uint64_t calls = clocks; cp_report(0); assert(clocks == calls);
    int before = logs; cp_frame(frame + 1);
    assert(logs == before + 1 && clocks == calls);
    assert(strstr(output, "texture 1 calls 20.00/60.00 ms CPU/wall max-call 60.00 ms"));
    assert(strstr(output, "draw 1 calls 25.00/70.00 ms CPU/wall max-call 70.00 ms"));
    // Discarding a short window must not contaminate the next slow window.
    work(4, 3000000, 2000000); calls = clocks; cp_frame(frame + 2);
    assert(logs == before + 1 && clocks == calls);
    work(4, 50000000, 25000000); cp_frame(frame + 3);
    assert(logs == before + 2);
    assert(strstr(output, "texture 1 calls 25.00/50.00 ms CPU/wall max-call 50.00 ms"));
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2); mode = atoi(argv[1]);
    setenv("DM_NVN_CPUPROF", mode >= 2 ? "2" : argv[1], 1);
    cp_frame(999); // Guest/present thread has independent, empty counters.
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, worker, (void *)(uintptr_t)1053));
    assert(!pthread_join(thread, NULL));
    int count = logs; uint64_t calls = clocks; cp_report(1);
    assert(logs == count && clocks == calls);
    assert(!strstr(output, "frame 999 thread"));
    if (mode == 2) {
        assert(strstr(output, "frame 1053 thread"));
        assert(strstr(output, "frame 1055 thread"));
        assert(!pthread_create(&thread, NULL, worker, (void *)(uintptr_t)1064));
        assert(!pthread_join(thread, NULL)); assert(logs == 4);
        assert(strstr(output, "frame 1064 thread"));
    }
    printf("fixture %d passed; %d reports; no Metal device\n", mode, logs);
    return 0;
}
'''


@unittest.skipUnless(platform.system() == 'Darwin' and shutil.which('clang'),
                     'requires macOS clang; does not require a GPU')
class ReplayProfiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = SOURCE.read_text()
        start = source.index('typedef struct { uint64_t wall, cpu; } cp_stamp;')
        end = source.index('#define AP_B', start)
        block = source[start:end]
        cls.block_digest = hashlib.sha256(block.encode()).hexdigest()
        cls.tmp = tempfile.TemporaryDirectory(prefix='ascension-cpuprof-')
        cls.addClassCleanup(cls.tmp.cleanup)
        src = Path(cls.tmp.name) / 'fixture.c'
        src.write_text(PRELUDE + block + HARNESS)
        cls.exe = Path(cls.tmp.name) / 'fixture'
        compiled = subprocess.run([
            shutil.which('clang'), '-O1', '-g', '-Wall', '-Wextra',
            '-fsanitize=address,undefined', '-Wno-unused-function',
            str(src), '-o', str(cls.exe),
        ], capture_output=True, text=True, timeout=30)
        if compiled.returncode:
            raise AssertionError(f'current profiler did not compile:\n{compiled.stderr}')

    def check_mode(self, mode):
        result = subprocess.run([str(self.exe), str(mode)], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0,
                         f'current block SHA256 {self.block_digest}\n{result.stdout}\n{result.stderr}')

    def test_disabled_reads_no_clocks(self):
        self.check_mode(0)

    def test_periodic_owner_thread_reporting(self):
        self.check_mode(1)

    def test_slow_windows_nested_phases_reset_and_thread_isolation(self):
        self.check_mode(2)

    def test_interleaved_queue_frames_produce_independent_windows(self):
        self.check_mode(3)


if __name__ == '__main__':
    unittest.main()
