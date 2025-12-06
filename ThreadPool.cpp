#include<iostream>
#include<vector>
#include<queue>
#include<atomic>       
#include<mutex>        
#include<thread>       
#include<chrono>       
#include<condition_variable> 
#include<memory>       
#include<cerrno>       
#include<utility>      
#include<cstdlib>      
#include<exception>    

// 每次扩容/缩容的线程数（批量操作，避免频繁创建/销毁）
constexpr int ADDCOUNT = 2;

// 任务结构体：封装「回调函数+参数」，实现任意类型任务的通用管理
struct Task {
    // 回调函数类型别名：接收void*参数，无返回值（通用任务格式）
    using funcType = void(*)(void*);
    funcType function = nullptr;  // 指向实际执行的任务函数（如testTaskFunc）

    // 任务参数：用unique_ptr管理，支持任意类型+自动释放
    // 模板参数1：void*（通用指针，适配任意类型）
    // 模板参数2：删除器（函数指针，指定如何释放参数内存）
    std::unique_ptr<void, void(*)(void*)> arg;

    // 空构造函数：初始化空状态，避免野指针
    // 智能指针绑定「空指针+空删除器」，保证对象默认构造后安全
    Task() : arg(nullptr, [](void*) {}) {}

    // 带参构造函数：初始化任务（禁止隐式转换，避免意外类型转换）
    // T：参数的实际类型（如int、自定义结构体）
    // callback：任务执行的回调函数
    // num：任务参数（堆内存指针，由智能指针接管）
    template<class T>
    explicit Task(funcType callback, T* num)
        : function(callback)
        // 智能指针绑定参数+自定义删除器：将void*转回T*再delete，避免delete void*的未定义行为
        , arg(num, [](void* p) { delete static_cast<T*>(p); })
    {
    }

    // 移动构造函数：转移任务资源所有权（unique_ptr不可拷贝，只能移动）
    // other：右值引用（临时对象），转移后原对象变为空且安全
    Task(Task&& other) noexcept
        : function(other.function)
        , arg(std::move(other.arg))  // 转移智能指针所有权
    {
        other.function = nullptr;  // 原对象回调函数置空，避免野指针
    }

    // 移动赋值运算符：给已有Task对象转移资源
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other) {  // 避免自赋值
            function = other.function;
            arg = std::move(other.arg);  // 转移资源
            other.function = nullptr;    // 原对象置空
        }
        return *this;
    }

    // 禁止拷贝：unique_ptr是独占所有权智能指针，拷贝会导致双重释放
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

// 线程池核心类：管理工作线程、任务队列、动态扩缩容、资源回收
class ThreadPool {
private:
    // 工作线程状态结构体：封装「线程对象+完成标志」，实现线程状态的精准管理
    struct WorkerStatus {
        std::thread thread;               // 工作线程对象（C++11线程句柄）
        std::atomic<bool> isFinished;     // 线程是否已完成（退出）：原子变量保证多线程读写安全
        std::thread::id tid;              // 缓存线程ID：避免线程退出后get_id()失效

        // 构造函数：初始化线程+完成标志
        // t：右值引用（线程对象不可拷贝，只能移动）
        WorkerStatus(std::thread&& t)
            : thread(std::move(t))        // 转移线程对象所有权
            , isFinished(false)           // 初始状态：未完成
            , tid(this->thread.get_id())  // 缓存线程ID
        {
        }

        // 移动构造：支持vector容器存储WorkerStatus（容器扩容时移动元素）
        WorkerStatus(WorkerStatus&& other) noexcept
            : thread(std::move(other.thread))
            , isFinished(other.isFinished.load())  // 原子变量加载值
            , tid(other.tid)
        {
        }

        // 移动赋值运算符：支持容器中元素的赋值操作
        WorkerStatus& operator=(WorkerStatus&& other) noexcept {
            if (this != &other) {
                thread = std::move(other.thread);
                isFinished = other.isFinished.load();
                tid = other.tid;
            }
            return *this;
        }

        // 禁止拷贝：线程对象不可拷贝，避免资源竞争
        WorkerStatus(const WorkerStatus&) = delete;
        WorkerStatus& operator=(const WorkerStatus&) = delete;
    };

    // ========== 核心成员变量 ==========
    std::queue<Task> TaskQ;                // 任务队列：先进先出（FIFO）存储待执行任务
    int queueCapacity;                     // 任务队列最大容量：防止任务过多导致内存溢出
    std::atomic<int> queueSize;            // 当前任务数：原子变量，多线程安全读写

    std::mutex poolLock;                   // 线程池全局锁：保护任务队列、线程列表等核心资源
    std::mutex busyNumLock;                // 忙线程数锁：单独保护busyNum，减少锁竞争

    std::thread managerThread;             // 管理者线程：监控线程池状态，执行扩缩容+清理
    std::vector<WorkerStatus> workerWorkers; // 工作线程状态列表：存储所有工作线程的状态

    std::condition_variable isFull;        // 条件变量：任务队列满时，阻塞添加任务的线程
    std::condition_variable isEmpty;       // 条件变量：任务队列为空时，阻塞工作线程

    std::atomic<int> liveNum;              // 当前存活线程数：原子变量，多线程安全
    std::atomic<int> busyNum;              // 当前忙线程数：原子变量，多线程安全
    std::atomic<int> minNum;               // 最小核心线程数：线程池保留的基础线程数
    std::atomic<int> maxNum;               // 最大线程数：线程池可创建的线程上限
    std::atomic<int> exitNum;              // 待退出线程数：管理者线程标记需要销毁的线程数
    std::atomic<bool> shutdown;            // 线程池关闭标志：true=关闭，false=运行

public:
    // 构造函数：初始化线程池参数，创建核心工作线程和管理者线程
    // min：最小核心线程数；max：最大线程数；queueCapacity：任务队列容量
    ThreadPool(int min, int max, int queueCapacity)
        : queueCapacity(queueCapacity)
        , queueSize(0)          // 初始任务数：0
        , liveNum(min)          // 初始存活线程数=核心线程数
        , busyNum(0)            // 初始忙线程数：0
        , minNum(min)
        , maxNum(max)
        , exitNum(0)
        , shutdown(false)       // 初始状态：运行中
    {
        // 参数合法性校验：避免无效配置（如min<=0、max<min）
        if (min <= 0 || queueCapacity <= 0 || min > max) {
            std::cerr << "线程池参数非法：min=" << min << " max=" << max << " queueCapacity=" << queueCapacity << std::endl;
            exit(EXIT_FAILURE);  // 非法参数直接退出程序
        }

        // 预留线程列表空间：避免vector频繁扩容导致线程对象拷贝（线程不可拷贝）
        workerWorkers.reserve(maxNum);

        // 创建最小核心工作线程：保证线程池启动后有基础线程处理任务
        for (int i = 0; i < min; i++) {
            // emplace_back：直接在vector中构造WorkerStatus对象（避免临时对象）
            // lambda捕获this：让线程函数能访问线程池的成员变量/函数
            workerWorkers.emplace_back(
                std::thread([this]() { this->workerFunc(); })
            );
            std::cout << "[初始化] 创建核心工作线程，tid=" << workerWorkers.back().tid << std::endl;
        }

        // 创建管理者线程：独立线程监控线程池状态，执行扩缩容和清理
        managerThread = std::thread([this]() { this->managerFunc(); });
        std::cout << "[初始化] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
    }

    // 析构函数：安全关闭线程池，回收所有线程和资源
    ~ThreadPool() {
        std::cout << "[析构] 开始关闭线程池..." << std::endl;

        // 标记线程池关闭：通知所有线程准备退出
        shutdown = true;

        // 唤醒所有阻塞的线程：避免线程永久阻塞在条件变量上
        isFull.notify_all();   // 唤醒阻塞在「任务队列满」的添加任务线程
        isEmpty.notify_all();  // 唤醒阻塞在「任务队列为空」的工作线程

        // 回收管理者线程：等待其执行完退出
        if (managerThread.joinable()) {
            managerThread.join();
            std::cout << "[析构] 管理者线程已回收，tid=" << managerThread.get_id() << std::endl;
        }

        // 回收所有剩余工作线程：加锁保护线程列表
        std::lock_guard<std::mutex> lock(poolLock);
        int recycleCount = 0;
        for (auto& worker : workerWorkers) {
            // 仅回收可join的线程（避免重复join）
            if (worker.thread.joinable()) {
                worker.thread.join();
                std::cout << "[析构] 回收工作线程，tid=" << worker.tid << std::endl;
                recycleCount++;
            }
        }
        // 清空线程状态列表：释放内存
        workerWorkers.clear();

        std::cout << "[析构] 线程池销毁完成，共回收" << recycleCount << "个工作线程" << std::endl;
    }

    // 对外接口：获取当前存活线程数（线程安全）
    int getLiveNum() {
        return liveNum.load();  // 原子变量加载值
    }

    // 对外接口：获取当前忙线程数（线程安全）
    int getBusyNum() {
        return busyNum.load();  // 原子变量加载值
    }

    // 对外接口：获取当前线程列表大小（调试用，线程安全）
    int getWorkerListSize() {
        std::lock_guard<std::mutex> lock(poolLock);  // 加锁保护线程列表
        return workerWorkers.size();
    }

    // 对外接口：添加任务到线程池（线程安全）
    // T：任务参数的实际类型；func：任务回调函数；arg：任务参数（堆内存指针）
    template<class T>
    bool addTask(Task::funcType func, T* arg) {
        // 1. 参数合法性校验：避免空任务/空参数
        if (func == nullptr || arg == nullptr) {
            std::cerr << "[添加任务] 参数非法：func=nullptr 或 arg=nullptr" << std::endl;
            return false;
        }

        // 2. 加锁保护任务队列：避免多线程同时操作队列
        std::unique_lock<std::mutex> lock(poolLock);

        // 3. 等待队列有空位（或线程池关闭）：防止任务溢出
        // wait逻辑：释放锁→阻塞→被唤醒后重新加锁→检查条件
        // lambda条件：队列未满 或 线程池关闭（防止虚假唤醒）
        isFull.wait(lock, [this]() {
            return queueSize < queueCapacity || shutdown;
            });

        // 4. 线程池已关闭：释放参数内存并返回
        if (shutdown) {
            delete arg;
            std::cerr << "[添加任务] 线程池已关闭，任务被拒绝" << std::endl;
            return false;
        }

        // 5. 添加任务到队列：emplace直接构造Task对象（避免拷贝）
        TaskQ.emplace(func, arg);
        queueSize++;  // 原子变量自增，线程安全

        // 6. 唤醒一个空闲工作线程：有新任务需要处理
        isEmpty.notify_one();

        std::cout << "[添加任务] 任务" << *static_cast<int*>(arg) << "添加成功，当前队列任务数=" << queueSize << std::endl;
        return true;
    }

private:
    // 工作线程核心函数：循环等待任务→执行任务→更新状态
    void workerFunc() {
        std::thread::id curTid = std::this_thread::get_id();  // 当前线程ID
        try {
            // 线程池运行中，循环等待任务
            while (!shutdown) {
                // 1. 加锁保护任务队列
                std::unique_lock<std::mutex> lock(poolLock);

                // 2. 等待任务/关闭/缩容信号：避免空轮询（浪费CPU）
                // 条件：任务队列非空 或 线程池关闭 或 需要缩容
                isEmpty.wait(lock, [this]() {
                    return !TaskQ.empty() || shutdown || (exitNum > 0 && liveNum > minNum);
                    });

                // 3. 线程池已关闭：退出循环
                if (shutdown) {
                    break;
                }

                // 4. 缩容：当前线程需要退出（管理者线程标记了exitNum）
                if (exitNum > 0 && liveNum > minNum) {
                    exitNum--;    // 待退出线程数减1
                    liveNum--;    // 存活线程数减1
                    std::cout << "[缩容] 工作线程退出，tid=" << curTid << " 剩余存活线程数=" << liveNum << std::endl;
                    break;
                }

                // 5. 防御性检查：队列空则继续等待（防止虚假唤醒）
                if (TaskQ.empty()) {
                    continue;
                }

                // 6. 取出任务：移动语义（Task不可拷贝）
                Task task = std::move(TaskQ.front());
                TaskQ.pop();
                queueSize--;  // 任务数减1

                // 7. 唤醒等待添加任务的线程：队列有空位了
                isFull.notify_one();
                lock.unlock();  // 释放锁：执行任务无需占用队列锁，减少竞争

                // 8. 执行任务：标记为忙线程
                {
                    std::lock_guard<std::mutex> lock(busyNumLock);  // 单独加锁保护busyNum
                    busyNum++;
                }
                std::cout << "[执行任务] 线程" << curTid << " 开始执行任务" << *static_cast<int*>(task.arg.get()) << std::endl;
                if (task.function != nullptr) {
                    task.function(task.arg.get());  // 调用回调函数执行任务
                }
                std::cout << "[执行任务] 线程" << curTid << " 完成任务" << *static_cast<int*>(task.arg.get()) << std::endl;

                {
                    std::lock_guard<std::mutex> lock(busyNumLock);
                    busyNum--;  // 任务完成，忙线程数减1
                }
            }
        }
        // 异常处理：确保线程退出时标记完成
        catch (const std::exception& e) {
            std::cerr << "[异常] 工作线程" << curTid << " 异常退出：" << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "[异常] 工作线程" << curTid << " 未知异常退出" << std::endl;
        }

        // 核心：线程退出时，标记自己为「已完成」（供管理者线程清理）
        std::lock_guard<std::mutex> lock(poolLock);
        for (auto& worker : workerWorkers) {
            if (worker.tid == curTid) {  // 匹配线程ID
                worker.isFinished = true;
                break;
            }
        }

        std::cout << "[退出] 工作线程" << curTid << " 已标记为完成，准备被清理" << std::endl;
    }

    // 管理者线程核心函数：监控线程池状态→执行扩缩容→清理已完成线程
    void managerFunc() {
        std::thread::id curTid = std::this_thread::get_id();
        std::cout << "[管理者] 线程启动，tid=" << curTid << std::endl;

        // 线程池运行中，持续监控
        while (!shutdown) {
            // 1. 每3秒检测一次：避免频繁检查浪费CPU
            std::this_thread::sleep_for(std::chrono::seconds(3));

            // 2. 获取当前状态（原子变量加载，线程安全）
            int curLive = liveNum.load();    // 当前存活线程数
            int curBusy = busyNum.load();    // 当前忙线程数
            int curQueue = queueSize.load(); // 当前任务队列数
            int curListSize = getWorkerListSize(); // 当前线程列表大小

            // 3. 打印监控日志（调试用）
            std::cout << "\n[管理者] 状态监控：存活线程=" << curLive
                << " 忙线程=" << curBusy
                << " 队列任务数=" << curQueue
                << " 线程列表大小=" << curListSize << std::endl;

            // ========== 扩容逻辑：任务堆积，线程数不足 ==========
            // 条件：任务数>存活线程数（处理不过来） 且 存活线程数<最大线程数（未达上限）
            if (curQueue > curLive && curLive < maxNum) {
                std::lock_guard<std::mutex> lock(poolLock);  // 加锁保护线程列表
                int addCount = 0;
                // 批量新增线程：最多ADDCOUNT个，避免一次性创建过多
                while (curLive < maxNum && addCount < ADDCOUNT && queueSize > curLive) {
                    workerWorkers.emplace_back(
                        std::thread([this]() { this->workerFunc(); })
                    );
                    curLive++;
                    addCount++;
                    liveNum++;  // 存活线程数更新
                    std::cout << "[扩容] 创建新工作线程，tid=" << workerWorkers.back().tid
                        << " 当前存活线程数=" << liveNum << std::endl;
                }
                if (addCount > 0) {
                    std::cout << "[扩容] 本次扩容" << addCount << "个线程，累计存活=" << liveNum << std::endl;
                }
            }

            // ========== 缩容逻辑：线程空闲，资源浪费 ==========
            // 条件：忙线程数*2 < 存活线程数（大部分线程空闲） 且 存活线程数>最小核心数（不销毁核心线程）
            if (curBusy * 2 < curLive && curLive > minNum) {
                std::lock_guard<std::mutex> lock(poolLock);  // 加锁保护exitNum
                exitNum = ADDCOUNT;  // 标记需要销毁的线程数
                isEmpty.notify_all();  // 唤醒所有空闲线程，触发退出
                std::cout << "[缩容] 标记" << ADDCOUNT << "个线程退出，当前待退出数=" << exitNum << std::endl;
            }

            // ========== 清理逻辑：删除已标记为「完成」的线程 ==========
            {
                std::lock_guard<std::mutex> lock(poolLock);  // 加锁保护线程列表
                auto it = workerWorkers.begin();
                int cleanCount = 0;

                while (it != workerWorkers.end()) {
                    // 仅清理已完成的线程
                    if (it->isFinished) {
                        std::thread::id tid = it->tid;
                        // 1. 回收线程系统资源：必须join（避免僵尸线程）
                        if (it->thread.joinable()) {
                            it->thread.join();
                            std::cout << "[清理] 回收已完成线程，tid=" << tid << std::endl;
                        }
                        // 2. 删除线程状态对象：释放容器内存
                        it = workerWorkers.erase(it);
                        cleanCount++;
                        std::cout << "[清理] 删除线程状态对象，tid=" << tid << std::endl;
                    }
                    else {
                        ++it;
                    }
                }

                if (cleanCount > 0) {
                    std::cout << "[清理] 本次清理" << cleanCount << "个已完成线程，剩余列表大小=" << workerWorkers.size() << std::endl;
                }
            }
        }

        std::cout << "[管理者] 线程退出，tid=" << curTid << std::endl;
    }
};

// 测试用任务函数：模拟实际任务（打印任务编号+休眠1秒）
void testTaskFunc(void* arg) {
    int num = *(static_cast<int*>(arg));  // 将void*转回int*，获取任务编号
    std::cout << "[任务执行] tid=" << std::this_thread::get_id() << " 处理任务编号：" << num << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 模拟任务耗时
}

// 主函数：测试线程池功能
int main() {
    std::cout << "===== 线程池测试开始 =====" << std::endl;

    // 创建线程池：2个核心线程，12个最大线程，任务队列容量100
    ThreadPool pool(2, 12, 100);

    // 阶段1：添加50个任务，触发扩容
    std::cout << "\n===== 阶段1：添加50个任务 =====" << std::endl;
    for (int i = 1; i <= 50; i++) {
        int* num = new int(i);  // 动态分配任务参数（堆内存，由Task智能指针接管）
        pool.addTask(testTaskFunc, num);
    }

    // 阶段2：等待任务执行完毕（12线程处理50任务≈5秒，留10秒冗余）
    std::cout << "\n===== 阶段2：等待10秒，任务执行完毕 =====" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 阶段3：等待管理者线程检测并清理已完成线程（每3秒检测一次，留9秒冗余）
    std::cout << "\n===== 阶段3：等待9秒，触发缩容和清理 =====" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(9));

    // 阶段4：打印最终状态，验证扩缩容+清理效果
    std::cout << "\n===== 阶段4：最终状态 =====" << std::endl;
    std::cout << "当前存活线程数：" << pool.getLiveNum() << std::endl;
    std::cout << "当前忙线程数：" << pool.getBusyNum() << std::endl;
    std::cout << "当前线程列表大小：" << pool.getWorkerListSize() << std::endl;

    // 阶段5：主线程退出，触发线程池析构（自动回收所有资源）
    std::cout << "\n===== 线程池测试结束 =====" << std::endl;
    return 0;
}