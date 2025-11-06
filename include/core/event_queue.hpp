#pragma once

#include "types.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>

namespace pmm {

class EventQueue {
private:
    std::queue<Event> queue_;
    mutable std::mutex mutex_; 
    std::condition_variable cv_;
    
public:
    void push(Event event);
    
    Event pop();
    
    bool empty() const;
    
    size_t size() const;
};

} // namespace pmm