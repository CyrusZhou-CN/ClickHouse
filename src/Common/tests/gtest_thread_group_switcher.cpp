#include <thread>

#include <gtest/gtest.h>

#include <Common/CurrentThread.h>
#include <Common/ThreadGroupSwitcher.h>
#include <Common/ThreadStatus.h>
#include <Common/setThreadName.h>
#include <Common/tests/gtest_global_context.h>
#include <Interpreters/Context.h>

namespace DB
{

/// Regression test for https://github.com/ClickHouse/clickhouse-core-incidents/issues/1682
///
/// Root cause: ThreadStatus::detachFromGroup() was not exception-safe.
/// OSThreadNiceValue::set(0) can throw ErrnoException (from setpriority(2)) and it was
/// called BEFORE thread_group.reset(). If it threw, ThreadStatus::thread_group was left
/// set, so the thread remained attached to the (now stale) group.
///
/// Fix: save os_threads_nice_value to a local, call thread_group.reset() first, then
/// call OSThreadNiceValue::set(0) inside its own try/catch.
///
/// The test below verifies that after a ThreadGroupSwitcher scope exits, the thread is
/// always left detached — even when the thread group was created with a non-zero
/// os_threads_nice_value (the setting that activates the code path that could throw).
/// It also verifies that a subsequent ThreadGroupSwitcher on the same thread succeeds.
TEST(ThreadGroupSwitcher, DetachFromGroupIsExceptionSafe)
{
    auto context = getContext().context;

    std::exception_ptr ex;
    bool group_is_null_after_detach = false;
    bool second_switcher_succeeded = false;

    std::thread t([&]
    {
        try
        {
            ThreadStatus ts;

            /// os_threads_nice_value = 5 activates the OSThreadNiceValue::set() call inside
            /// detachFromGroup(). Before the fix, if set() threw, thread_group was never
            /// reset and the thread stayed attached.
            const Int32 nonzero_nice_value = 5;
            auto G1 = std::make_shared<ThreadGroup>(context, nonzero_nice_value);
            auto G2 = std::make_shared<ThreadGroup>(context, nonzero_nice_value);

            {
                ThreadGroupSwitcher switcher(G1, ThreadName::REMOTE_FS_READ_THREAD_POOL);
                ASSERT_EQ(getCurrentThreadGroup(), G1);
            }

            /// After the scope, the thread must be detached regardless of whether
            /// OSThreadNiceValue::set() succeeded or threw (it is a no-op without
            /// CAP_SYS_NICE, but thread_group.reset() must run either way).
            group_is_null_after_detach = (getCurrentThreadGroup() == nullptr);

            /// A second switcher on the same thread must not throw
            /// "Thread is already attached to a group".
            {
                ThreadGroupSwitcher switcher(G2, ThreadName::REMOTE_FS_READ_THREAD_POOL);
                second_switcher_succeeded = (getCurrentThreadGroup() == G2);
            }

            ASSERT_EQ(getCurrentThreadGroup(), nullptr);
        }
        catch (...)
        {
            ex = std::current_exception();
        }
    });
    t.join();

    if (ex)
        std::rethrow_exception(ex);

    EXPECT_TRUE(group_is_null_after_detach)
        << "Thread must be detached after ThreadGroupSwitcher scope exits, "
           "even with non-zero os_threads_nice_value";
    EXPECT_TRUE(second_switcher_succeeded)
        << "Second ThreadGroupSwitcher must succeed — thread must not stay attached "
           "to a stale group after the first switcher’s scope";
}

} // namespace DB
