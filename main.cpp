#include "ThreadPool.hpp"
#include <iostream>
#include <vector>
#include <numeric>
#include <chrono>
#include <cassert>

// 测试用加法函数（支持整数累加，模拟耗时任务）
int add(int start, int end) {
    int sum = 0;
    for (int i = start; i <= end; ++i) {
        sum += i;
        // 模拟任务耗时（可选，用于观察扩缩容）
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    return sum;
}

// 测试用异常任务（验证异常传递）
void throwExceptionTask(int id) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    throw std::runtime_error("异常任务" + std::to_string(id) + "触发测试异常");
}

// 测试1：批量加法任务（验证任务执行和返回值正确性）
void testAddTask() {
    std::cout << "\n===== 测试1：批量加法任务 =====" << std::endl;
    TaskQueue queue;
    // 创建FIXED模式线程池（4个线程，匹配CPU核心数）
    ThreadPool pool("加法池", &queue, PoolMode::FIXED, 4);

    const int TASK_COUNT = 8;    // 8个加法任务
    const int RANGE = 100;     // 每个任务累加10000个数
    std::vector<std::future<std::any>> futures;

    auto start = std::chrono::high_resolution_clock::now();

    // 提交8个加法任务（每个任务计算 [i*RANGE, (i+1)*RANGE-1] 的和）
    for (int i = 0; i < TASK_COUNT; ++i) {
        int startNum = i * RANGE;
        int endNum = (i + 1) * RANGE - 1;
        futures.emplace_back(pool.submitTask(add, startNum, endNum));
        std::cout << "提交加法任务" << i << "：计算 " << startNum << " 到 " << endNum << " 的和" << std::endl;
    }

    // 等待任务完成并验证结果
    int totalSum = 0;
    for (int i = 0; i < TASK_COUNT; ++i) {
        try {
            auto res = futures[i].get();
            int sum = std::any_cast<int>(res);
            totalSum += sum;
            std::cout << "加法任务" << i << "结果：" << sum << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "加法任务" << i << "异常：" << e.what() << std::endl;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 验证总结果（0到79999的和 = (79999*80000)/2 = 3199960000）
    int expectedTotal = (0 + (TASK_COUNT * RANGE - 1)) * (TASK_COUNT * RANGE) / 2;
    assert(totalSum == expectedTotal && "加法任务总结果错误！");
    std::cout << "所有加法任务完成，总结果：" << totalSum << "（预期：" << expectedTotal << "）" << std::endl;
    std::cout << "任务执行耗时：" << duration.count() << "ms" << std::endl;

    pool.shutdownPool();
}

// 测试2：多线程池共享任务队列
void testShareQueue() {
    std::cout << "\n===== 测试2：多池共享队列 =====" << std::endl;
    TaskQueue sharedQueue;

    // 创建两个线程池，共享同一个任务队列
    ThreadPool pool1("共享池1", &sharedQueue, PoolMode::FIXED, 2);    // FIXED模式（2线程）
    ThreadPool pool2("共享池2", &sharedQueue, PoolMode::CACHED, 1, 3); // CACHED模式（1-3线程）

    std::vector<std::future<std::any>> futures;
    // 提交6个加法任务（两个池共同处理）
    for (int i = 0; i < 6; ++i) {
        int start = i * 5000;
        int end = (i + 1) * 5000 - 1;
        if (i % 2 == 0) {
            futures.emplace_back(pool1.submitTask(add, start, end));
        }
        else {
            futures.emplace_back(pool2.submitTask(add, start, end));
        }
        std::cout << "提交共享任务" << i << "：" << start << "-" << end << std::endl;
    }

    // 等待结果
    for (int i = 0; i < 6; ++i) {
        try {
            auto res = futures[i].get();
            std::cout << "共享任务" << i << "结果：" << std::any_cast<int>(res) << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "共享任务" << i << "异常：" << e.what() << std::endl;
        }
    }

    pool1.shutdownPool();
    pool2.shutdownPool();
}

// 测试3：线程池重启功能
void testRestart() {
    std::cout << "\n===== 测试3：线程池重启 =====" << std::endl;
    TaskQueue queue;
    ThreadPool pool("重启池", &queue, PoolMode::FIXED, 2);

    // 重启前提交任务
    auto f1 = pool.submitTask(add, 1, 1000);
    std::cout << "重启前任务结果：" << std::any_cast<int>(f1.get()) << std::endl;

    // 关闭线程池
    pool.shutdownPool();

    // 重启线程池
    pool.resumePool();

    // 重启后提交新任务
    auto f2 = pool.submitTask(add, 1001, 2000);
    std::cout << "重启后任务结果：" << std::any_cast<int>(f2.get()) << std::endl;

    pool.shutdownPool();
}

// 测试4：异常任务处理（验证异常传递）
void testExceptionTask() {
    std::cout << "\n===== 测试4：异常任务 =====" << std::endl;
    TaskQueue queue;
    ThreadPool pool("异常池", &queue, PoolMode::FIXED, 2);

    // 提交1个正常加法任务和1个异常任务
    auto f1 = pool.submitTask(add, 1, 100);
    auto f2 = pool.submitTask(throwExceptionTask, 1);

    // 处理正常任务
    try {
        std::cout << "正常任务结果：" << std::any_cast<int>(f1.get()) << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "正常任务异常：" << e.what() << std::endl;
    }

    // 处理异常任务
    try {
        f2.get(); // 会抛出异常
    }
    catch (const std::exception& e) {
        std::cerr << "捕获异常任务：" << e.what() << std::endl;
    }

    pool.shutdownPool();
}

// 测试5：CACHED模式扩缩容（验证动态线程管理）
void testCachedResize() {
    std::cout << "\n===== 测试5：CACHED模式扩缩容 =====" << std::endl;
    TaskQueue queue;
    // CACHED模式：核心1线程，最大5线程，每次扩容2线程
    ThreadPool pool("扩缩容池", &queue, PoolMode::CACHED, 1, 5);
    pool.setEverytimeAddCount(2);

    std::vector<std::future<std::any>> futures;
    // 提交8个耗时加法任务（触发扩容）
    for (int i = 0; i < 8; ++i) {
        futures.emplace_back(pool.submitTask(add, i * 10000, (i + 1) * 10000 - 1));
    }

    // 等待任务执行（观察扩缩容日志，管理者线程每3秒打印状态）
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 等待所有任务完成（之后会触发缩容）
    for (auto& f : futures) {
        f.get();
    }

    // 等待缩容（管理者线程检测到空闲后缩容）
    std::cout << "所有任务完成，等待缩容..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(6));

    pool.shutdownPool();
}

int main() {
    // 执行所有测试用例
    testAddTask();
    testShareQueue();
    testRestart();
    testExceptionTask();
    testCachedResize();

    std::cout << "\n===== 所有测试完成 =====" << std::endl;
    return 0;
}