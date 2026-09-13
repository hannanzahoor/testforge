/// Performance suite — checks that the framework's own hot paths stay fast,
/// and that concurrency primitives behave under contention.
///
/// These are guard rails, not benchmarks. The thresholds are deliberately
/// loose (an order of magnitude above what the code actually does on modest
/// hardware) so they catch a regression like "someone made JSON parsing
/// quadratic" without failing on a loaded CI runner. Real measurements live in
/// benchmarks/, which reports numbers instead of asserting on them.

#include "testforge/core/Json.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/execution/ThreadPool.hpp"
#include "testforge/testing/ShortAssertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace testforge;

TESTFORGE_TEST_OPTS(perf,
                    json_parsing_throughput,
                    TestOptions{}
                        .describe("Parsing a 1000-element document stays well under a second.")
                        .withTags({"perf", "json"})
                        .withTimeout(30000)) {
    // Build a document of a realistic shape for this project: an array of
    // result-like objects.
    json::Value array = json::Value::array();
    for (int i = 0; i < 1000; ++i) {
        json::Value item = json::Value::object();
        item.set("test_id", "test_" + std::to_string(i));
        item.set("status", i % 7 == 0 ? "FAILED" : "PASSED");
        item.set("duration_ms", i * 3);
        item.set("tags", json::Value::array({json::Value("api"), json::Value("regression")}));
        array.push(item);
    }
    const std::string encoded = array.dump();
    ASSERT_GT(encoded.size(), std::size_t{10000});

    const Stopwatch watch;
    const json::Value parsed = json::parse(encoded);
    const Milliseconds elapsed = watch.elapsed();

    ASSERT_EQ(parsed.size(), std::size_t{1000});
    ASSERT_LT(elapsed.count(), std::int64_t{1000});

    ctx.addMetadata("document_bytes", static_cast<std::int64_t>(encoded.size()));
    ctx.addMetadata("parse_ms", millisOf(elapsed));
    ctx.log().info(
        "parsed document",
        {{"bytes", static_cast<std::int64_t>(encoded.size())}, {"ms", millisOf(elapsed)}});
}

TESTFORGE_TEST_OPTS(perf,
                    thread_pool_executes_all_tasks,
                    TestOptions{}
                        .describe("Every submitted task runs exactly once under contention.")
                        .withTags({"perf", "concurrency", "regression"})
                        .withTimeout(30000)) {
    constexpr int kTaskCount = 500;

    std::atomic<int> executed{0};
    std::atomic<int> sum{0};

    {
        ThreadPool pool(4, "perftest");
        std::vector<std::future<void>> pending;
        pending.reserve(kTaskCount);

        for (int i = 0; i < kTaskCount; ++i) {
            pending.push_back(pool.submit([&executed, &sum, i] {
                executed.fetch_add(1, std::memory_order_relaxed);
                sum.fetch_add(i, std::memory_order_relaxed);
            }));
        }
        for (std::future<void>& future : pending) {
            future.get();
        }
        pool.waitIdle();
    }

    ASSERT_EQ(executed.load(), kTaskCount);
    // n(n-1)/2 for n = 500. Verifying the sum catches a task running twice,
    // which a plain count would miss.
    ASSERT_EQ(sum.load(), (kTaskCount * (kTaskCount - 1)) / 2);
}

TESTFORGE_TEST_OPTS(perf,
                    thread_pool_survives_throwing_tasks,
                    TestOptions{}
                        .describe("A task that throws is captured in its future and does not "
                                  "kill its worker.")
                        .withTags({"perf", "concurrency", "regression"})
                        .withTimeout(20000)) {
    ThreadPool pool(2, "throwtest");

    std::future<int> bad = pool.submit(
        []() -> int { throw std::runtime_error("deliberate failure inside a pooled task"); });
    std::future<int> good = pool.submit([]() -> int { return 7; });

    bool caught = false;
    try {
        (void)bad.get();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    ASSERT_TRUE(caught);

    // The pool must still be usable: the worker that ran the throwing task is
    // not supposed to have died with it.
    ASSERT_EQ(good.get(), 7);
    std::future<int> after = pool.submit([]() -> int { return 11; });
    ASSERT_EQ(after.get(), 11);
}

TESTFORGE_TEST_OPTS(perf,
                    assertion_overhead_is_negligible,
                    TestOptions{}
                        .describe("10,000 passing assertions cost well under a second.")
                        .withTags({"perf", "fast"})
                        .withTimeout(30000)) {
    const Stopwatch watch;
    for (int i = 0; i < 10000; ++i) {
        ASSERT_LT(i, 10000);
    }
    const Milliseconds elapsed = watch.elapsed();

    ASSERT_LT(elapsed.count(), std::int64_t{1000});
    ctx.addMetadata("assertions", 10000);
    ctx.addMetadata("elapsed_ms", millisOf(elapsed));
}

TESTFORGE_TEST_OPTS(perf,
                    cooperative_sleep_respects_the_clock,
                    TestOptions{}
                        .describe("ctx.sleepFor waits approximately the requested time.")
                        .withTags({"perf", "concurrency"})
                        .withTimeout(20000)) {
    const Stopwatch watch;
    const bool completed = ctx.sleepFor(Milliseconds{200});
    const Milliseconds elapsed = watch.elapsed();

    ASSERT_TRUE(completed);
    ASSERT_GE(elapsed.count(), std::int64_t{190});
    // Generous upper bound: a loaded scheduler can add a lot of latency, and
    // this test is about correctness, not real-time guarantees.
    ASSERT_LT(elapsed.count(), std::int64_t{3000});
    ctx.addMetadata("slept_ms", millisOf(elapsed));
}
