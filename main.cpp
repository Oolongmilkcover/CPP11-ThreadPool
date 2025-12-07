#include "ThreadPool.hpp"

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