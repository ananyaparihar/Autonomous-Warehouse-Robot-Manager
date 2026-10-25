// min_heap.hpp - binary min-heap for real-time task dispatch.
// Lower urgency value = more urgent. Ties broken by arrival order (FIFO).
//
// Complexity: push O(log n), pop O(log n), top O(1), build-by-pushing O(n log n).
// Space: O(n).

#pragma once
#include <stdexcept>
#include <string>
#include <vector>
#include <cstddef>
#include <utility>

struct Task {
    int id;                 // unique task / order id
    std::string label;      // e.g. "Pick SKU-K200"
    int urgency;            // lower = more urgent
    long long seq = 0;      // arrival order, set by the heap (tie-break)
};

class MinHeap {
public:
    void push(Task t) {
        t.seq = next_seq_++;
        data_.push_back(std::move(t));
        sift_up(data_.size() - 1);
    }

    // Returns the most urgent task without removing it.
    const Task& top() const {
        if (data_.empty()) throw std::runtime_error("heap is empty");
        return data_.front();
    }

    // Removes and returns the most urgent task.
    Task pop() {
        if (data_.empty()) throw std::runtime_error("heap is empty");
        Task result = std::move(data_.front());
        data_.front() = std::move(data_.back());
        data_.pop_back();
        if (!data_.empty()) sift_down(0);
        return result;
    }

    bool empty() const { return data_.empty(); }
    std::size_t size() const { return data_.size(); }

    // Read-only view of the underlying array (useful for the visual demo).
    const std::vector<Task>& raw() const { return data_; }

private:
    std::vector<Task> data_;
    long long next_seq_ = 0;

    // true if a should come out before b
    static bool before(const Task& a, const Task& b) {
        if (a.urgency != b.urgency) return a.urgency < b.urgency;
        return a.seq < b.seq;
    }

    void sift_up(std::size_t i) {
        while (i > 0) {
            std::size_t parent = (i - 1) / 2;
            if (!before(data_[i], data_[parent])) break;
            std::swap(data_[i], data_[parent]);
            i = parent;
        }
    }

    void sift_down(std::size_t i) {
        const std::size_t n = data_.size();
        while (true) {
            std::size_t left = 2 * i + 1, right = 2 * i + 2, best = i;
            if (left < n && before(data_[left], data_[best])) best = left;
            if (right < n && before(data_[right], data_[best])) best = right;
            if (best == i) break;
            std::swap(data_[i], data_[best]);
            i = best;
        }
    }
};
