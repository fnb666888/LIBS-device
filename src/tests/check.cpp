#include "check.hpp"

namespace testing {

int finish(const char* suite) {
    if (g_failures) {
        std::printf("FAILED %s: %d of %d checks failed\n", suite, g_failures, g_checks);
        return 1;
    }
    std::printf("OK %s: %d checks passed\n", suite, g_checks);
    return 0;
}

}  // namespace testing
