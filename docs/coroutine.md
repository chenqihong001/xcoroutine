# coroutine 协程


## c++协程执行流程
```cpp
// 编译器展开伪代码：
{
    // 分配协程帧
    auto frame = allocate_coroutine_frame();
    
    // 构造promise
    auto& promise = construct_promise(frame);
    
    // 获取返回对象
    task<int> result = promise.get_return_object();
    
    // 初始挂起
    if (promise.initial_suspend().await_ready()) {
        // 立即执行
    } else {
        // 挂起，返回给调用者
        return result;
    }
    
    // 协程体
    try {
        std::cout << "Step 1\n";
        co_await something();  // 可能挂起
        std::cout << "Step 2\n";
        promise.return_value(42);
    } catch (...) {
        promise.unhandled_exception();
    }
    
    
   
    if(promise.final_suspend().await_ready()) {
        // 最终清理
    } else {
      
    }
  
}
```

## promise_type

## coroutine_handle
* destroy(): 销毁协程帧
