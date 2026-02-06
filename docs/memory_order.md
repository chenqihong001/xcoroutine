# 内存序


线程A 
x = 1
y = 1


线程B
if(y == 1){
    assert(x == 1);// 可能会失败
}

内存序就是在告诉编译器：哪些重排不允许

可见性（Visibility）
CPU有缓存：核1写了，核2可能看不到

有序性（Ordering）
两行代码，别人看到的顺序可能不一样
x = 1;
y = 1;
别人可能看到 y = 1 先于 x = 1

c++内存序控制的是不同线程“观察到”的顺序
* std::memory_order_relaxed 只保证原子性
* std::memory_order_release/acquire 发布，获取