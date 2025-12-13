#ifndef H_THREADPOOL
#define H_THREADPOOL
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
#include<functional>
#include<future>
#include<any>
#include<algorithm>
#include<string>
#include<optional>
// 线程池工作模式枚举
// FIXED：固定线程数模式（核心线程数=最大线程数，无动态扩缩容）
// CACHED：缓存线程池模式（动态扩缩容，核心线程数≤存活线程数≤最大线程数）
enum class PoolMode {
	FIXED,
	CACHED,
};

// 通用任务结构体：封装任意类型任务（支持无返回值/有返回值+异常传递）
struct Task {
	// 包装后的任务可调用对象：无参数，返回std::any（统一存储任意类型返回值）
	std::function<std::any()> func;
	// 与future绑定的promise：用于任务执行后传递返回值或异常
	std::shared_ptr<std::promise<std::any>> promise;

	// 空构造函数（默认初始化）
	Task() = default;

	// 带参构造函数：可变参模板封装任意任务和参数
	// Func：任务函数类型，Args：任务参数类型包
	template<typename Func, typename... Args>
	explicit Task(Func&& func, Args&&... args) {
		// 用lambda捕获任务和参数，绑定为无参函数（统一任务接口）
		// std::forward：完美转发，保持参数的左值/右值属性
		this->func = [func = std::forward<Func>(func), ... args = std::forward<Args>(args)]() -> std::any {
			try {
				// 编译期判断任务是否有返回值（std::invoke_result_t获取函数返回类型）
				if constexpr (std::is_same_v<std::invoke_result_t<Func, Args...>, void>) {
					// 无返回值任务：直接执行，返回空std::any
					std::invoke(func, args...); // 调用任务函数（支持函数指针、仿函数、lambda等）
					return std::any();
				}
				else {
					// 有返回值任务：执行后将返回值存入std::any
					return std::invoke(func, args...);
				}
			}
			catch (...) {
				// 捕获所有异常，后续通过promise传递给future
				throw;
			}
			};
		// 创建promise对象，用于后续生成future（供用户获取结果）
		this->promise = std::make_shared<std::promise<std::any>>();
	}

	// 移动构造函数：Task不可拷贝（std::function和promise无拷贝语义）
	Task(Task&& other) noexcept
		: func(std::move(other.func))
		, promise(std::move(other.promise)) {
	}

	// 移动赋值运算符：支持容器中Task对象的赋值操作
	Task& operator=(Task&& other) noexcept {
		if (this != &other) { // 避免自赋值
			func = std::move(other.func);
			promise = std::move(other.promise);
		}
		return *this;
	}

	// 禁止拷贝构造和拷贝赋值（防止资源浅拷贝导致错误）
	Task(const Task&) = delete;
	Task& operator=(const Task&) = delete;
};

// 任务队列类：线程安全的任务存储容器（无容量限制）
// 职责：仅负责任务的入队、出队和状态查询，不参与线程阻塞/唤醒
class TaskQueue {
private:
	std::queue<Task> taskQ;        // 存储任务的队列（核心容器）
	std::atomic_int queueSize;     // 队列中任务数量（原子变量，线程安全读写）
	std::mutex queueMutex;         // 保护队列操作的互斥锁（避免并发冲突）
public:
	// 构造函数：初始化任务数量为0
	TaskQueue()
		:queueSize(0)
	{
	}
	// 析构函数：默认析构（无动态分配资源需要释放）
	~TaskQueue() {}

	// 任务入队：移动语义接收任务，线程安全
	// 参数：task - 待入队的任务（右值引用，避免拷贝）
	// 返回：bool - 入队成功返回true（无容量限制，恒为true）
	bool taskAdd(Task&& task) {
		std::lock_guard<std::mutex> lock(queueMutex); // 加锁：保证入队操作原子性
		taskQ.emplace(std::move(task));               // 任务移动入队（避免拷贝）
		queueSize++;                                  // 任务数量自增（原子操作）
		return true;
	}

	// 任务出队：线程安全，队列为空时返回空
	// 返回：std::optional<Task> - 有任务返回Task对象，无任务返回std::nullopt
	std::optional<Task> taskTake() {
		std::lock_guard<std::mutex> lock(queueMutex); // 加锁：保证出队操作原子性
		if (taskQ.empty()) {
			return std::nullopt; // 队列为空，返回空值
		}
		// 队列非空：取出队首任务并移动返回
		Task task = std::move(taskQ.front());
		taskQ.pop();
		queueSize--; // 任务数量自减（原子操作）
		return task;
	}

	// 获取当前队列中任务数量（线程安全）
	// 返回：int - 任务数量
	int getQueueSize() const {
		return queueSize.load(); // 原子变量加载（无锁，线程安全）
	}

	// 判断队列是否为空（线程安全）
	// 返回：bool - 空返回true，否则返回false
	bool empty() {
		std::lock_guard<std::mutex> lock(queueMutex); // 加锁：保证判断结果准确（避免并发修改）
		return taskQ.empty();
	}

	// 禁止拷贝和移动（避免队列被非法复制导致资源冲突）
	TaskQueue(const TaskQueue&) = delete;
	TaskQueue& operator=(const TaskQueue&) = delete;
	TaskQueue(TaskQueue&&) = delete;
	TaskQueue& operator=(TaskQueue&&) = delete;
};

// 线程池核心类：管理工作线程、任务调度、扩缩容、关闭/重启
class ThreadPool {
private:
	// 工作线程状态结构体：记录线程的核心信息
	struct WorkerStatus {
		std::thread thread;          // 工作线程对象
		std::atomic_bool isFinish;   // 线程是否已完成（标记为true表示可清理）
		std::thread::id tid;         // 线程ID（用于匹配和调试）

		// 默认构造函数：初始化完成状态为false

		// 带参构造函数：接收线程对象并初始化
		// 参数：t - 已创建的工作线程（右值引用）
		WorkerStatus(std::thread&& t)
			:thread(std::move(t))
			, isFinish(false)
			,tid(this->thread.get_id())
		{
		}

		// 移动构造函数：支持vector容器存储（vector扩容时移动元素）
		WorkerStatus(WorkerStatus&& other) noexcept
			:thread(std::move(other.thread))
			, isFinish(other.isFinish.load()) // 原子变量加载状态
			, tid(other.tid) {
		}

		// 移动赋值运算符：支持容器中元素的赋值操作
		WorkerStatus& operator=(WorkerStatus&& other) noexcept {
			if (this != &other) {
				thread = std::move(other.thread);
				isFinish = other.isFinish.load();
				tid = other.tid;
			}
			return *this;
		}

		// 禁止拷贝构造和赋值（避免线程对象被非法复制）
		WorkerStatus(const WorkerStatus&) = delete;
		WorkerStatus& operator=(const WorkerStatus&) = delete;
	};

	// RAII类：自动管理忙线程数（构造时+1，析构时-1）
	// 作用：确保任务执行前后忙线程数准确，避免手动操作遗漏
	class BusyNumGuard {
	public:
		// 构造函数：忙线程数+1（加锁保证原子性）
		BusyNumGuard(std::atomic_int& busyNum, std::mutex& lock)
			: m_busyNum(busyNum)
			, m_lock(lock) {
			std::lock_guard<std::mutex> guard(m_lock);
			m_busyNum++;
		}
		// 析构函数：忙线程数-1（加锁保证原子性）
		~BusyNumGuard() {
			std::lock_guard<std::mutex> guard(m_lock);
			m_busyNum--;
		}
		// 禁止拷贝（避免重复计数）
		BusyNumGuard(const BusyNumGuard&) = delete;
		BusyNumGuard& operator=(const BusyNumGuard&) = delete;
	private:
		std::atomic_int& m_busyNum; // 引用线程池的忙线程数
		std::mutex& m_lock;         // 保护忙线程数修改的互斥锁
	};

private:
	// 线程池核心状态变量（均为原子变量，线程安全读写）
	std::atomic_int liveNum;    // 当前存活的工作线程数
	std::atomic_int busyNum;    // 当前正在执行任务的忙线程数
	std::atomic_int exitNum;    // 待退出的线程数（CACHED模式缩容用）
	std::atomic_int minNum;     // 核心线程数（最小保留线程数）
	std::atomic_int maxNum;     // 最大线程数（CACHED模式上限）
	std::atomic_bool shutdown;  // 线程池关闭标志（true=关闭，false=运行）
	std::string PoolName;

	// 同步相关变量
	std::mutex poolMutex;       // 保护线程池核心操作的互斥锁（如线程列表、条件变量）
	std::mutex busyMutex;       // 保护忙线程数修改的互斥锁（与BusyNumGuard配合）
	std::condition_variable notEmpty; // 条件变量：任务队列非空或关闭时唤醒线程

	// 线程相关变量
	std::thread managerThread;  // 管理者线程（CACHED模式专属，负责扩缩容和线程清理）
	std::vector<WorkerStatus> workersThread; // 工作线程状态列表（管理所有工作线程）

	// 其他配置变量
	int ADDCOUNT;               // CACHED模式每次扩容的线程数
	bool isInitTrue;            // 线程池初始化标志（暂未使用，预留扩展）
	TaskQueue* TaskQ;           // 任务队列指针（支持多线程池共享一个队列）
	PoolMode poolmode;          // 线程池工作模式（FIXED/CACHED）

public:
	// 构造函数：FIXED模式（固定线程数）
	// 参数：
	//   taskq - 任务队列指针（外部传入，支持共享）
	//   mode - 工作模式（必须为FIXED）
	//   livenum - 固定线程数（默认值：CPU核心数）
	ThreadPool(std::string name,TaskQueue* taskq, PoolMode mode, int livenum = std::thread::hardware_concurrency())
		:liveNum(livenum)
		, maxNum(livenum)
		, minNum(livenum)
		, TaskQ(taskq)
		, poolmode(mode)
		, isInitTrue(true)
		, ADDCOUNT(2)
		, exitNum(0)
		, busyNum(0)
		, shutdown(false)
		, PoolName(name)
	{
		// 参数合法性检查：线程数>0、任务队列非空、模式为FIXED
		if (liveNum <= 0 || taskq == nullptr || mode == PoolMode::CACHED) {
			std::cout << "线程池初始化失败，退出程序中...." << std::endl;
			exit(EXIT_FAILURE);
		}
		// 预留线程列表容量（避免扩容时频繁移动元素）
		workersThread.reserve(livenum);

		// 创建固定数量的核心工作线程
		for (int i = 0; i < liveNum; i++) {
			// emplace_back：直接在vector中构造WorkerStatus对象（避免临时对象）
			// lambda捕获this：让线程函数能访问线程池的成员变量/函数
			workersThread.emplace_back(
				std::thread([this]() { this->workerFunc(); })
			);
			std::cout <<PoolName << "[初始化] 创建核心工作线程，tid=" << workersThread.back().tid << std::endl;
		}
	}

	// 构造函数：CACHED模式（动态扩缩容）
	// 参数：
	//   taskq - 任务队列指针（外部传入，支持共享）
	//   mode - 工作模式（必须为CACHED）
	//   min - 核心线程数（最小保留线程数）
	//   max - 最大线程数（扩缩容上限）
	ThreadPool(std::string name, TaskQueue* taskq, PoolMode mode, int min, int max )
		: liveNum(min)
		, maxNum(max)
		, minNum(min)
		, TaskQ(taskq)
		, poolmode(mode)
		, isInitTrue(true)
		, ADDCOUNT(2)
		, exitNum(0)
		, busyNum(0)
		, shutdown(false)
		, PoolName(name)
	{
		// 参数合法性检查：核心线程数>0、核心≤最大、任务队列非空、模式为CACHED
		if (min <= 0 || min > max || taskq == nullptr || mode == PoolMode::FIXED) {
			std::cout << "线程池初始化失败，退出程序中...." << std::endl;
			exit(EXIT_FAILURE);
		}
		// 预留线程列表容量（避免扩容时频繁移动元素）
		workersThread.reserve(max);

		// 创建核心工作线程（最小保留线程数）
		for (int i = 0; i < min; i++) {
			// emplace_back：直接在vector中构造WorkerStatus对象（避免临时对象）
			// lambda捕获this：让线程函数能访问线程池的成员变量/函数
			workersThread.emplace_back(
				std::thread([this]() { this->workerFunc(); })
			);
			std::cout << PoolName << "[初始化] 创建核心工作线程，tid=" << workersThread.back().tid << std::endl;
		}

		// 创建管理者线程（负责扩缩容和线程清理）
		managerThread = std::thread([this] {
			this->managerFunc();
			});
		std::cout << PoolName << "[初始化] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
	}

	// 析构函数：关闭线程池，回收所有线程资源
	~ThreadPool() {
		shutdownPool(); // 析构时自动关闭线程池，避免资源泄漏
	}

	// 提交任务到队列：支持任意任务+参数，返回future供获取结果
	// 参数：Func - 任务函数类型，Args - 任务参数类型包
	// 返回：std::future<std::any> - 用于获取任务结果或异常
	template<typename Func, typename... Args>
	std::future<std::any> submitTask(Func&& func, Args&&... args) {
		// 线程池已关闭，拒绝提交任务
		if (shutdown) {
			std::cerr << PoolName << "[ThreadPool] 线程池已关闭，拒绝提交任务" << std::endl;
			std::promise<std::any> emptyPromise;
			emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("ThreadPool is shutdown")));
			return emptyPromise.get_future();
		}
		// 任务队列为空，拒绝提交任务
		if (!TaskQ) {
			std::cerr << PoolName << "[ThreadPool] 任务队列为空，拒绝提交任务" << std::endl;
			std::promise<std::any> emptyPromise;
			emptyPromise.set_exception(std::make_exception_ptr(std::runtime_error("TaskQueue is null")));
			return emptyPromise.get_future();
		}

		// 封装任务：通过Task构造函数绑定任务和参数
		Task task(std::forward<Func>(func), std::forward<Args>(args)...);
		// 获取future：与task的promise绑定，供用户获取结果
		std::future<std::any> future = task.promise->get_future();
		// 任务入队
		TaskQ->taskAdd(std::move(task));

		// 唤醒一个阻塞的工作线程（有新任务需要处理）
		notEmpty.notify_one();
		return future;
	}

	// 关闭线程池：安全回收所有工作线程和管理者线程（区别于销毁）
	// 特点：等待所有正在执行的任务完成，关闭后不可提交新任务
	void shutdownPool() {
		if (shutdown) return; // 避免重复关闭
		shutdown = true; // 设置关闭标志
		std::cout << PoolName << "[ThreadPool] 开始关闭线程池，唤醒所有工作线程..." << std::endl;

		// 双重唤醒：确保所有阻塞在条件变量上的线程被唤醒（避免虚假唤醒）
		notEmpty.notify_all();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		notEmpty.notify_all();

		// 回收工作线程：加锁保护线程列表
		std::lock_guard<std::mutex> lock(poolMutex);
		int recycleCount = 0;
		for (auto& worker : workersThread) {
			if (worker.thread.joinable()) { // 线程可join（未被回收）
				auto tid = worker.tid;
				worker.thread.join(); // 等待线程执行完当前任务后退出
				std::cout << PoolName << "[ThreadPool] 回收工作线程，tid=" << tid << std::endl;
				recycleCount++;
			}
		}
		workersThread.clear(); // 清空线程状态列表

		// 回收管理者线程（仅CACHED模式有）
		if (poolmode == PoolMode::CACHED && managerThread.joinable()) {
			auto tid = managerThread.get_id();
			managerThread.join();
			std::cout << PoolName << "[ThreadPool] 回收管理者线程，tid=" << tid<< std::endl;
		}

		std::cout << PoolName << "[ThreadPool] 线程池关闭完成，共回收" << recycleCount << "个工作线程" << std::endl;
	}

	// 重启线程池：仅在关闭状态下可调用，重启后可重新提交任务
	void resumePool() {
		if (!shutdown) { // 未关闭状态，无需重启
			std::cerr << PoolName << "[ThreadPool] 线程池未关闭，无需重启" << std::endl;
			return;
		}

		// 重置线程池状态变量
		shutdown = false;    // 恢复运行状态
		exitNum = 0;         // 清空待退出线程数
		liveNum.store(minNum); // 存活线程数恢复为核心线程数

		// 重新创建核心工作线程：加锁保护线程列表
		std::lock_guard<std::mutex> lock(poolMutex);
		workersThread.reserve(maxNum); // 预留容量
		for (int i = 0; i < minNum; i++) {
			workersThread.emplace_back(
				std::thread([this]() { this->workerFunc(); })
			);
			std::cout << PoolName << "[重启] 创建核心工作线程，tid=" << workersThread.back().tid << std::endl;
		}

		// CACHED模式重新创建管理者线程
		if (poolmode == PoolMode::CACHED) {
			managerThread = std::thread([this]() { this->managerFunc(); });
			std::cout << PoolName << "[重启] 创建管理者线程，tid=" << managerThread.get_id() << std::endl;
		}

		std::cout << PoolName << "[ThreadPool] 线程池重启完成，当前核心线程数=" << minNum << std::endl;
	}

	// 辅助接口：设置CACHED模式每次扩容的线程数
	// 参数：num - 每次扩容的线程数
	void setEverytimeAddCount(int num) {
		if (poolmode == PoolMode::FIXED) { // FIXED模式无扩缩容，设置失败
			std::cout << "当前线程池模式为FIXED!设置失败...." << std::endl;
			return;
		}
		std::lock_guard<std::mutex> lock(poolMutex); // 加锁保证线程安全
		ADDCOUNT = num;
	}

	// 辅助接口：获取当前存活的工作线程数
	void Rename(std::string name) {
		if (!shutdown) {
			std::cout << PoolName << "正在运行中，请于线程池关闭时重命名..." << std::endl;
			return;
		}
		std::cout << PoolName << "[重命名]:成功重命名为"<<name<< std::endl;
		PoolName = name;
	}

	// 辅助接口：获取当前存活的工作线程数
	int getLiveNum() const {
		return liveNum.load();
	}

	// 辅助接口：获取当前忙线程数（正在执行任务的线程）
	int getBusyNum() const {
		return busyNum.load();
	}

	// 辅助接口：获取任务队列中未执行的任务数
	int getQueueSize() const {
		return TaskQ->getQueueSize();
	}

	// 辅助接口：获取工作线程列表的大小（已创建的线程数）
	int getWorkerListSize() {
		std::lock_guard<std::mutex> lock(poolMutex);  // 加锁保护线程列表
		return static_cast<int>(workersThread.size());
	}

private:
	// 工作线程核心函数：线程启动后循环执行的逻辑
	void workerFunc() {
		std::thread::id curTid = std::this_thread::get_id(); // 当前线程ID
		
		try {
			// 线程循环：未关闭且无退出指令时持续运行
			while (!shutdown) {
				// 加锁等待条件变量：队列非空 或 线程池关闭
				std::unique_lock<std::mutex> lock(poolMutex);
				notEmpty.wait(lock, [this]() {
					return !TaskQ->empty() || shutdown || (exitNum > 0 && liveNum > minNum);
					});

				// 线程池已关闭，退出循环
				if (shutdown) {
					lock.unlock();
					break;
				}

				// CACHED模式缩容：有退出指令且存活线程数>核心线程数
				if (exitNum > 0 && liveNum > minNum) {
					exitNum--;       // 待退出线程数减1
					liveNum--;       // 存活线程数减1
					std::cout << PoolName << "[缩容] 工作线程退出，tid=" << curTid << " 剩余存活线程数=" << liveNum << std::endl;
					break; // 退出循环，线程结束
				}

				// 从队列取任务（此时队列非空，因条件变量已保证）
				auto taskOpt = TaskQ->taskTake();
				lock.unlock(); // 释放锁：避免执行任务时占用线程池锁，提高并发

				// 空任务跳过（极端并发场景下可能出现，双重防护）
				if (!taskOpt) {
					continue;
				}

				// 执行任务：移动获取任务对象
				Task task = std::move(taskOpt.value());
				{
					BusyNumGuard guard(busyNum, busyMutex); // RAII：忙线程数+1
					std::cout << PoolName << "[执行任务] 线程" << curTid << " 开始执行，当前队列剩余任务数=" << TaskQ->getQueueSize() << std::endl;
					try {
						std::any result = task.func(); // 执行任务
						task.promise->set_value(std::move(result)); // 任务成功：传递返回值
					}
					catch (...) {
						// 任务异常：传递异常到promise
						task.promise->set_exception(std::current_exception());
						std::cerr << PoolName << "[执行任务] 线程" << curTid << " 执行异常" << std::endl;
					}
					std::cout << PoolName << "[执行任务] 线程" << curTid << " 执行完成" << std::endl;
				} // 析构BusyNumGuard：忙线程数-1
			}
		}
		catch (const std::exception& e) {
			// 捕获线程执行过程中的异常
			std::cerr << PoolName << "[异常] 工作线程" << curTid << " 异常退出：" << e.what() << std::endl;
			liveNum--;
		}
		catch (...) {
			// 捕获未知异常
			std::cerr << PoolName << "[异常] 工作线程" << curTid << " 未知异常退出" << std::endl;
			liveNum--;
		}

		// 线程退出前：标记为已完成（供管理者线程清理）
		std::lock_guard<std::mutex> lock(poolMutex);
		for (auto& worker : workersThread) {
			if (worker.tid == curTid) {
				worker.isFinish = true;
				break;
			}
		}
		std::cout << PoolName << "[工作线程] 退出，tid=" << curTid << std::endl;
	}

	// 管理者线程核心函数：CACHED模式专属，负责扩缩容和线程清理
	void managerFunc() {
		std::thread::id curTid = std::this_thread::get_id();
		std::cout << PoolName << "[管理者] 线程启动，tid=" << curTid << std::endl;

		// 循环监控：线程池未关闭时持续运行
		while (!shutdown) {
			// 每3秒检测一次：避免频繁检查浪费CPU
			std::this_thread::sleep_for(std::chrono::seconds(3));
			if (shutdown) { // 线程池已关闭，退出循环
				break;
			}

			// 获取当前线程池状态（原子变量加载，线程安全）
			int curLive = liveNum.load();    // 当前存活线程数
			int curBusy = busyNum.load();    // 当前忙线程数
			int curQueue = TaskQ->getQueueSize(); // 当前任务队列数
			int curListSize = getWorkerListSize(); // 当前线程列表大小

			// 打印监控日志（调试用，可根据需求关闭）
			std::cout << PoolName << "\n[管理者] 状态监控：存活线程=" << curLive
				<< " 忙线程=" << curBusy
				<< " 队列任务数=" << curQueue
				<< " 线程列表大小=" << curListSize << std::endl;

			// ========== 扩容逻辑：任务堆积，线程数不足 ==========
			// 条件1：存活线程数 < 核心线程数（核心线程被意外销毁，需补充）
			// 条件2：任务数 > 存活线程数（任务堆积，现有线程处理不过来）且 存活线程数 < 最大线程数（未达上限）
			if (curLive < minNum || (curQueue > curLive && curLive < maxNum)) {
				std::lock_guard<std::mutex> lock(poolMutex);  // 加锁保护线程列表
				int addCount = 0;
				// 批量新增线程：最多ADDCOUNT个，避免一次性创建过多线程
				while (curLive < maxNum && addCount < ADDCOUNT && (TaskQ->getQueueSize() > curLive || curLive < minNum)) {
					workersThread.emplace_back(
						std::thread([this]() { this->workerFunc(); })
					);
					curLive++;
					addCount++;
					liveNum++;  // 存活线程数更新（原子操作）
					std::cout << PoolName << "[扩容] 创建新工作线程，tid=" << workersThread.back().tid
						<< " 当前存活线程数=" << liveNum << std::endl;
				}
				if (addCount > 0) {
					std::cout << PoolName << "[扩容] 本次扩容" << addCount << "个线程，累计存活=" << liveNum << std::endl;
				}
			}

			// ========== 缩容逻辑：线程空闲，资源浪费 ==========
			// 条件：忙线程数*2 < 存活线程数（大部分线程空闲）且 存活线程数>核心线程数（不销毁核心线程）
			if (curBusy * 2 < curLive && curLive > minNum) {
				std::lock_guard<std::mutex> lock(poolMutex);  // 加锁保护exitNum
				exitNum = ADDCOUNT;  // 标记需要销毁的线程数
				notEmpty.notify_all();  // 唤醒所有空闲线程，触发退出判断
				std::cout << PoolName << "[缩容] 标记" << ADDCOUNT << "个线程退出，当前待退出数=" << exitNum << std::endl;
			}

			// ========== 清理逻辑：删除已标记为「完成」的线程 ==========
			{
				std::lock_guard<std::mutex> lock(poolMutex);  // 加锁保护线程列表
				auto it = workersThread.begin();
				int cleanCount = 0;
				while (it != workersThread.end()) {
					// 仅清理已标记为完成的线程（isFinish=true）
					if (it->isFinish) {
						std::thread::id tid = it->tid;
						// 回收线程系统资源：必须join，避免僵尸线程
						if (it->thread.joinable()) {
							it->thread.join();
							std::cout << PoolName << "[清理] 回收已完成线程，tid=" << tid << std::endl;
						}
						// 从列表中删除线程状态对象：释放容器内存
						it = workersThread.erase(it);
						cleanCount++;
						std::cout << PoolName << "[清理] 删除线程状态对象，tid=" << tid << std::endl;
					}
					else {
						++it;
					}
				}

				if (cleanCount > 0) {
					std::cout << PoolName << "[清理] 本次清理" << cleanCount << "个已完成线程，剩余列表大小=" << workersThread.size() << std::endl;
				}
			}
		}

		std::cout << PoolName << "[管理者] 线程退出，tid=" << curTid << std::endl;
	}
};

#endif // !H_THREADPOOL

