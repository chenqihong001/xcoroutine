# when_all
```cpp
class when_all_latch;// 计数器


class when_all_task_promise<void>
- start(when_all_latch& latch)
- final_suspend() / co_return时调用  (通过start传入的latch) latch->notify_awaitable_completed();


when_all_task make_when_all_task(Awaitable&& awaitable) {
    if constexpr (awaiter_result_t<Awaitable> == void){
        co_await std::forward<Awaitable>(awaitable); // 等待awaitable完成
        co_return;// 
    }else{
        co_yield co_await std::forward<Awaitable>(awaitable); // 同样等待，并收集结果，但是没有co_return
    }
}

// make_when_all_task 不是函数，是一个协程，不是直接调用执行的



auto when_all(Awaitable&& ... awaitable) {

    std::make_tuple(make_when_all_task(std::move(awaitable))...);
    auto when_all_task_tuple = std::make_tuple(task1,task2,task3...);
    return when_all_ready_awaitable(when_all_task_tuple);
}

co_await when_all_ready_awaitable;
-> awaitable.try_await(awaiting_coroutine);
bool try_await(std::coroutine_handle<>awaiting_coroutine) 
{
    std::apply(... task.start()) 开始依次按顺序执行所有收集的task
    
}


```