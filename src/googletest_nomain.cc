#include "task_handler.h"
#include "gtest/gtest.h"

using namespace conan;

TEST(test_example, test1) { EXPECT_EQ(1, 1); }

TEST(task_handler, test_blocked) {
  constexpr int num = 6;
  int num_tmp{};
#ifdef TASKHANDLER_COMPILED_LIB
  TaskHandler::init();
#endif
  TaskHandler::get_instance()->add_callable<Blocked>([&] { num_tmp = num; });
  EXPECT_EQ(num, num_tmp);
}

TEST(task_handler, test_queued) {
  constexpr int num = 6;
  int num_tmp{};
  TaskHandler::get_instance()->add_callable([&] { num_tmp = num; });
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(num, num_tmp);
}

TEST(task_handler, test_future) {
  constexpr int num = 6;
  int num_tmp{};
  auto future_void =
      TaskHandler::get_instance()->add_callable<Future>([&] { num_tmp = num; });
  future_void.wait();
  EXPECT_EQ(num, num_tmp);

  auto future_int =
      TaskHandler::get_instance()->add_callable<Future>([&] { return num; });
  EXPECT_EQ(num, std::any_cast<int>(future_int.get()));

  auto future_string = TaskHandler::get_instance()->add_callable<Future>(
      [&] { return std::string{"Conan Snow"}; });
  EXPECT_EQ(std::string{"Conan Snow"},
            std::any_cast<std::string>(future_string.get()));
}

TEST(task_handler, test_priority) {
  constexpr int num = 6;
  int num_tmp{};
  TaskHandler::get_instance()->add_callable([&] { num_tmp = num; });
  TaskHandler::get_instance()->add_callable([&] { num_tmp = 1; }, -1);
  TaskHandler::get_instance()->add_callable([&] { num_tmp = 2; }, -2);
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(num, num_tmp);
}

TEST(task_handler, test_multithread) {
  constexpr int num = 6;
  int num_tmp{};
  int num_tmp1{};
  int num_tmp2{};
  TaskHandler::get_instance()->add_callable([&] { num_tmp = num; });
  TaskHandler::get_instance<1>()->add_callable([&] { num_tmp1 = num; });
  TaskHandler::get_instance<2>()->add_callable([&] { num_tmp2 = num; });
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(num, num_tmp);
  EXPECT_EQ(num, num_tmp1);
  EXPECT_EQ(num, num_tmp2);
}

TEST(task_handler, test_bolck_recursion) {
  constexpr int num = 6;
  TaskHandler::get_instance()->add_callable<Blocked>(
      [&] { TaskHandler::get_instance()->add_callable<Blocked>([&] {}); });
}

TEST(task_handler, test_future_recursion) {
  constexpr int num = 6;
  int num_tmp{};
  TaskHandler::get_instance()->add_callable<Blocked>([&] {
    auto future_tmp =
        TaskHandler::get_instance()->add_callable<Future>([&] { return num; });
    num_tmp = std::any_cast<int>(future_tmp.get());
  });
  EXPECT_EQ(num, num_tmp);
}