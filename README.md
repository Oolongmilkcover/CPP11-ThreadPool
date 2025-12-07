# CPP11-ThreadPool

## 一、项目名称：C++11 轻量级线程池
### 基于 C++11 实现的高可用线程池，支持动态扩缩容、异常任务安全处理、多线程并发调度，适配 Linux/Windows 平台。

## 二、项目介绍
### 该线程池核心解决「多线程任务调度的资源复用与安全问题」，相比原生 pthread 直接创建线程，具备以下优势：
1. 线程复用：避免频繁创建 / 销毁线程的系统开销。
2. 动态扩缩容：根据任务队列长度、忙线程数自动调整存活线程数。
3. 异常安全：单个任务抛异常不影响线程池整体运行，状态统计精准。
4. 内存安全：基于智能指针 + RAII 自动管理任务参数 / 线程资源，无内存泄漏。
5. 易用性：通用任务接口支持任意类型任务投递，配置化初始化（核心线程数 / 最大线程数 / 队列容量）。

## 三、核心功能
1. 动态扩缩容：自动扩容（最多批量创建 2 个线程），线程空闲时自动缩容（保留核心线程）。
2. 任务队列限流：大容量，满队列时阻塞添加任务，避免内存溢出。
3. 异常任务处理：行异常，保证 忙线程数/存活线程数 回滚，线程池不崩溃。
4. 线程池安全关闭：新任务，唤醒所有阻塞线程，优雅回收所有线程资源。
5. 多线程并发添加任务：同时投递任务，任务队列线程安全。
6. 状态监控：线程数、忙线程数、任务队列长度，便于调试或监控。

## 四、技术栈
1. 语言标准：C++11（原子变量、条件变量、智能指针、Lambda、移动语义）。
2. 核心组件：std::atomic（线程安全状态）、std::condition_variable（阻塞 / 唤醒）、
   std::mutex（互斥锁）、  std::unique_ptr （内存管理）。
3. 设计模式：RAII（资源管理）、生产者 - 消费者（任务队列调度）。
4. 测试：断言校验、多场景测试（队列满 / 并发添加 / 异常任务 / 缩容 / 关闭）。

## 五、快速开始
1. Linux下
    编译 g++ -std=c++11 main.cpp -o thread_pool -lpthread  
    运行 ./thread_pool
2. 基本使用示例（main）
        #include "ThreadPool.hpp"
        // 自定义任务函数
        void customTask(void* arg) {
            int num = *(static_cast<int*>(arg));
            // 用户需要的业务逻辑...
        }
        int main() {
            // 初始化线程池：核心线程2，最大线程8，队列容量20
            ThreadPool pool(2, 8, 20);
            
            // 添加任务
            for(int i = 0 ; i < 100 ; ++i){
                int* taskNum = new int(1+i);
                pool.addTask(customTask, taskNum);
            }
            // 手动关闭线程池(可选)
            std::this_thread::sleep_for(std::chrono::seconds(30)); //运行30秒再手动关闭
            pool.shutdownPool();

            return 0;
        }

## 六、测试用例说明
### 项目内置6类测试场景，覆盖线程池核心能力：
1. 初始化校验：验证核心线程数、队列初始状态。
2. 队列满测试：连续添加任务至队列满，验证阻塞 / 拒绝逻辑。
3. 并发添加任务：5 个线程并发添加 50 个任务，验证扩容逻辑。
4. 缩容测试：任务执行完毕后，验证自动缩容至核心线程数。
5. 关闭测试：关闭线程池后添加任务，验证拒绝逻辑。
6. 异常任务测试：单独测试异常任务执行，验证线程池稳定性。
    ps:异常任务测试与其他测试一起运行会相互干扰，所以我用了#if 0  #endif 来控制测试用例，
       将0改为1即可更换（只能有一个main函数）。

## 七、核心设计细节
### 任务结构体设计
1. 通用任务结构体支持任意类型任务投递，通过 std::unique_ptr+自定义删除器 管理参数，避免 delete void* 未定义行为：
    struct Task {
        using funcType = void(*)(void*);
        funcType function;
        std::unique_ptr<void, void(*)(void*)> arg; // 通用参数+自定义释放
    };


### WorkerStatus结构体设计&管理者清理细节
0.  将std::thread对象与线程状态(活跃/退出)封装进WorkerStatus结构体,当线程被要求退出(缩容)或是异常退出时,
    管理者可以基于这个标志判断是否清理(join)掉这个线程，以此来保证无用线程不占用资源。

1.  WorkerStatus结构体设计：
    struct WorkerStatus {
        std::thread thread;               // 工作线程对象（C++11线程句柄）
        std::atomic<bool> isFinished;     // 线程是否已完成（退出）：原子变量保证多线程读写安全
        std::thread::id tid;              // 缓存线程ID：避免线程退出后get_id()失效
        ...
    }

2.  管理者清理细节(joinable()时join并erase,!joinable()时erase):
    void managerFunc() {
        ...
        std::lock_guard<std::mutex> lock(poolLock);  // 加锁保护线程列表
        auto it = workerWorkers.begin();
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
        ...
    }


### 异常安全设计:
1. 工作线程执行任务时通过 RAII 自动管理忙线程数，无论正常 / 异常执行，析构时必回滚；
2. 异常捕获兜底：try-catch 捕获所有异常，异常退出时更新存活线程数，保证状态精准；
3. 线程池关闭时唤醒所有阻塞线程，避免永久阻塞。


### 动态扩缩容逻辑
1. 扩容条件：任务队列长度 > 存活线程数 且 存活线程数 < 最大线程数；
2. 缩容条件：忙线程数 * 2 <存活线程数 且 存活线程数> 核心线程数；
3. 批量扩缩容：每次扩容 / 缩容 2 个线程，减少频繁操作开销。

#if 0
## 八、小结与曾遇到的问题(大白话)
    这个项目学到了很多新东西，本来是学到了Linux中C++多线程(用的还是pthread.h库)，本来也只是学习一下基本原理。试着手写出来后我丢给AI优化时，它给出了更好的解决方案：就是用智能指针去自动释放任务中的内存，这便引起了我提前学C++11的兴趣(其实迟早要学),那我就想将C++11的特性结合到这个线程池里，结果越加入就变得越复杂，功能也就越齐全越全面不懂的东西也越来越多。
### 大致的路线就是：
    烂大街的基于[生产者-消费者]模型实现的c语言线程池
    ->用C++面向对象思想封装这个线程池
    ->引入智能指针
    ->封装Task任务结构体(std::move语句&左值右值概念)
    ->C++的thread库替换原有的pthread库
    ->condition_variable、mutex等都来了
    ->atomic原子变量的引入
    ->意识到缩容只是数字上的改变而工作线程数组内不会变，要是反复增缩容，那工作线程数组就会一直变大
    ->将线程与状态封装成WorkerStatus结构体并加入管理者线程清除功能
    ->加入异常捕获功能
    ->全面测试
    ->发现busyNum修改不对劲，引入RAII管理busyNum
    ->全面测试通过->烂大街的C++线程池(hhh)
    ->提前学习了GitHub的使用并了解下工作流开了波眼
    ->完成README.md
#endif




    