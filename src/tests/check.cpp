#include "check.hpp"

namespace testing {

int finish(const char* suite) {
    // 先清 scratch 再报结果：失败的用例目录也一并清掉，免得"这次挂了"
    // 留下一堆 build/tmp_* 让人误以为是别的用例的残留。要留现场就手工拷走。
    purge_scratch();
    if (g_failures) {
        std::printf("FAILED %s: %d of %d checks failed\n", suite, g_failures, g_checks);
        return 1;
    }
    std::printf("OK %s: %d checks passed\n", suite, g_checks);
    return 0;
}

}  // namespace testing
