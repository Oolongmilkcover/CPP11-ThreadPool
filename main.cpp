#include "ThreadPool.hpp"
#include <cassert>
// 测试用任务函数：模拟实际任务（打印任务编号+休眠1秒）
void testTaskFunc(void* arg) {
    int num = *(static_cast<int*>(arg));  // 将void*转回int*，获取任务编号
    std::cout << "[任务执行] tid=" << std::this_thread::get_id() << " 处理任务编号：" << num << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 模拟任务耗时
}

// 线程池状态校验函数参数一: 线程池引用 参数二：预期存活线程数 参数三：预期忙线程数 参数四：预期线程列表大小 参数五：测试阶段描述
void checkPoolStatus(ThreadPool& pool, int expectLive, int expectBusy, int expectListSize, const std::string& stage) {
    std::this_thread::sleep_for(std::chrono::seconds(1)); // 等待状态稳定
    int live = pool.getLiveNum();
    int busy = pool.getBusyNum();
    int listSize = pool.getWorkerListSize();
    std::cout << "\n[ 校验-" << stage
        << " ] 存活线程=" << live << " ( 预期±1 ：" << expectLive
        << " ) | 忙线程=" << busy << " ( 预期±1 ：" << expectBusy
        << " ) | 列表大小=" << listSize << " ( 预期±1 ：" << expectListSize << " )"
        << std::endl;
    assert(abs(live - expectLive) <= 1 && "存活线程数异常");
    assert(abs(busy - expectBusy) <= 1 && "忙线程数异常");
    assert(abs(listSize - expectListSize) <= 1 && "线程列表大小异常");
}

// 多线程添加任务函数
void addTasksInThread(ThreadPool& pool, int start, int end) {
    for (int i = start; i <= end; i++) {
        int* num = new int(i);
        bool ret = pool.addTask(testTaskFunc, num);
        if (!ret) {
            std::cerr << "[并发添加] 任务" << i << "添加失败" << std::endl;
        }
    }
}

// 队列满测试函数
void testQueueFull(ThreadPool& pool) {
    std::cout << "\n===== 测试：任务队列满 =====" << std::endl;
    // 连续添加任务直到队列满
    for (int i = 1001; i <= 1015; i++) {
        int* num = new int(i);
        bool ret = pool.addTask(testTaskFunc, num);
        std::cout << "[队列满测试] 添加任务" << i << "：" << (ret ? "成功" : "失败") << std::endl;
        if (!ret) break; // 队列满后退出
    }
}


//新增测试 poolMode模式测试
#if 1
int main() {
    
    ThreadPool pool1(PoolMode::FIXED, 100);

    for (int i = 0; i <= 400; i++) {
        int* num = new int(i);
        bool ret = pool1.addTask(testTaskFunc, num);
    }
    std::this_thread::sleep_for(std::chrono::seconds(40));

    return 0;
}


#endif



// 主函数：测试线程池功能
//从7个点测试线程池的各项功能：
//队列满场景、并发添加任务、异常任务执行、任务完成后缩容、关闭线程池时添加任务、最终状态校验
#if 0
int main() {
    std::cout << "===== 线程池测试开始 =====" << std::endl;

    // 1. 初始化线程池（小队列容量，便于测试队列满）
    int minNum = 2, maxNum = 5, queueCap = 10;
    ThreadPool pool(PoolMode::CACHED, queueCap,minNum, maxNum);
    checkPoolStatus(pool, minNum, 0, minNum, "初始化");

    // 2. 测试1：队列满场景
    testQueueFull(pool); //这里一共添加了15个任务

    // 3. 测试2：并发添加任务（5个线程，每个添加10个任务），一共添加50个任务
    std::cout << "\n===== 测试：并发添加任务 =====" << std::endl;
    std::vector<std::thread> addThreads;
    for (int i = 0; i < 5; i++) {
        addThreads.emplace_back(addTasksInThread, std::ref(pool), i * 10 + 1, (i + 1) * 10);
    }
    // 等待所有添加线程完成
    for (auto& t : addThreads) {
        t.join();
    }
    checkPoolStatus(pool, maxNum, maxNum, maxNum, "并发添加后（扩容）");


    // 5. 测试4：任务执行完毕，触发缩容
    std::cout << "\n===== 测试：任务完成后缩容 =====" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10)); // 等待所有任务执行完毕
    checkPoolStatus(pool, minNum, 0, minNum, "缩容后");

    // 6. 测试5：关闭线程池时添加任务
    std::cout << "\n===== 测试：关闭线程池时添加任务 =====" << std::endl;
    // 手动触发shutdown
    pool.shutdownPool();
    int* finalNum = new int(999);
    bool ret = pool.addTask(testTaskFunc, finalNum);
    std::cout << "[关闭测试] 添加任务999：" << (ret ? "成功（程序错误）" : "失败（程序正确）") << std::endl;
    assert(ret == false && "线程池关闭后应拒绝添加任务");

    // 7. 最终状态校验
    checkPoolStatus(pool, minNum, 0, minNum, "最终状态");

    std::cout << "\n===== 线程池测试结束 =====" << std::endl;
    return 0;
}
#endif


#if 0
//单独定义出来两个函数来更明显地测试异常任务
// 异常任务函数
void exceptionTaskFunc(void* arg) {
    int num = *(static_cast<int*>(arg));
    std::cout << "[异常任务] 开始执行任务" << num << std::endl;
    throw std::runtime_error("模拟任务执行异常");
}

// 普通任务函数（验证线程池是否正常工作）
void normalTaskFunc(void* arg) {
    int num = *(static_cast<int*>(arg));
    std::cout << "[普通任务] 成功执行任务" << num << std::endl;
}

// 8. 测试6：单独测试异常任务执行
int main() {
    std::cout << "===== 异常任务独立测试开始 =====" << std::endl;

    // 1. 初始化极简线程池（避免干扰）
    int minNum = 1, maxNum = 1, queueCap = 1;
    ThreadPool pool(PoolMode::CACHED,minNum, maxNum, queueCap);
    std::cout << "[初始化] 线程池状态：存活线程=" << pool.getLiveNum()
        << " 忙线程=" << pool.getBusyNum() << std::endl;

    // 2. 添加并执行异常任务
    std::cout << "\n[步骤1] 添加异常任务" << std::endl;
    int* errNum = new int(-1);
    bool addRet = pool.addTask(exceptionTaskFunc, errNum);
    assert(addRet == true && "异常任务添加失败"); // 验证任务添加成功

    // 等待异常任务执行完成
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 3. 验证异常后的状态
    std::cout << "\n[步骤2] 验证异常后状态" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3)); //等待管理者线程创建新线程并清理异常线程
    int live = pool.getLiveNum();
    int busy = pool.getBusyNum();
    std::cout << "当前状态：存活线程=" << live << " 忙线程=" << busy << std::endl;
    assert(live == minNum && "异常后线程池应保留核心线程"); // 核心线程未崩溃
    assert(busy == 0 && "异常后忙线程数应回滚为0"); // 忙线程数正确减1

    // 4. 验证线程池仍能正常工作（添加普通任务）
    std::cout << "\n[步骤3] 验证线程池可用性" << std::endl;
    int* normalNum = new int(1);
    addRet = pool.addTask(normalTaskFunc, normalNum);
    assert(addRet == true && "异常后线程池无法添加任务"); // 线程池未崩溃

    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "[普通任务] 执行完成，线程池正常" << std::endl;

    std::cout << "\n===== 异常任务独立测试通过 =====" << std::endl;
    return 0;
}

#endif