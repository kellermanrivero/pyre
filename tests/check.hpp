#pragma once

// A test harness with no dependency: CHECK records a failure and goes on,
// REQUIRE stops the test. Each test is a function registered with TEST.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Test>& tests() {
    static std::vector<Test> t;
    return t;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Stop {};

struct Register {
    Register(const char* name, std::function<void()> fn) { tests().push_back({name, std::move(fn)}); }
};

inline int runAll(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed = 0;
    for (auto& t : tests()) {
        if (filter && std::string(t.name).find(filter) == std::string::npos) continue;
        const int before = failures();
        std::printf("[ RUN  ] %s\n", t.name);
        std::fflush(stdout);
        try {
            t.fn();
        } catch (Stop&) {
        }
        ++ran;
        const bool ok = failures() == before;
        failed += !ok;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::printf("%d tests, %d failed, %d checks failed\n", ran, failed, failures());
    return failed == 0 && ran > 0 ? 0 : 1;
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                                       \
    static void CHECK_CAT(test_, name)();                                                \
    static check::Register CHECK_CAT(reg_, name)(#name, &CHECK_CAT(test_, name));        \
    static void CHECK_CAT(test_, name)()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ++check::failures();                                                         \
            if (check::failures() < 50) std::printf("  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                                \
    } while (0)

#define REQUIRE(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ++check::failures();                                                         \
            std::printf("  %s:%d: REQUIRE(%s)\n", __FILE__, __LINE__, #cond);            \
            throw check::Stop{};                                                         \
        }                                                                                \
    } while (0)

#define CHECK_MAIN \
    int main(int argc, char** argv) { return check::runAll(argc, argv); }
