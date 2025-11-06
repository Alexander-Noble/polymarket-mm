#include "core/event_queue.hpp"

namespace pmm {

    void EventQueue::push(Event event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(event));
        }
        cv_.notify_one();
    }

    Event EventQueue::pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return !queue_.empty(); });
        Event event = std::move(queue_.front());
        queue_.pop();
        return event;
    }

    bool EventQueue::empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t EventQueue::size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

} // namespace pmm