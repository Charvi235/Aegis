#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/lru_cache.hpp
//
// A thread-safe, fixed-capacity LRU (Least-Recently-Used) cache.
//
// Template parameters:
//   Key    – hashable type used to look up entries (e.g. std::string)
//   Value  – the cached value (e.g. std::string for a serialised HTTP response)
//
// ── Data structure ────────────────────────────────────────────────────────────
// Two containers are kept in sync:
//
//   list_  : std::list<pair<Key,Value>>  — doubly linked list, ordered
//             most-recently-used (front) to least-recently-used (back).
//
//   index_ : std::unordered_map<Key, list::iterator>  — maps each key to its
//             position in the list.
//
// Both get() and put() are O(1):
//   get(key):  index_ lookup → O(1); splice to front → O(1).
//   put(key):  index_ lookup → O(1); splice or insert at front → O(1);
//              evict back element if over capacity → O(1).
//
// ── Why list + unordered_map and not a single ordered structure? ───────────
//   An ordered structure like std::map gives O(log N) access.  We need O(1)
//   both for the hash lookup AND for repositioning on access.  std::list
//   iterators are stable (insertions and splices don't invalidate them), so
//   we can store a list iterator in the map and splice in O(1) without
//   invalidating anything.  That's the classic LRU trick.
//
// ── Thread safety ─────────────────────────────────────────────────────────────
// A single std::mutex protects all mutations.  Same rationale as RateLimiter:
// the critical section is tiny (a map lookup + pointer reshuffle), so a
// single lock is simple and correct.  For very high concurrency you'd use a
// sharded mutex or a concurrent hash map, but that's premature here.
//
// ── Implementation in the header ─────────────────────────────────────────────
// LruCache is a class template, so the full implementation must be visible at
// every instantiation site.  Placing it here avoids explicit instantiation
// boilerplate and keeps related code together.
// ─────────────────────────────────────────────────────────────────────────────

#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace aegis {

template <typename Key, typename Value>
class LruCache {
public:
    // capacity – maximum number of entries before the LRU item is evicted.
    // Throws std::invalid_argument if capacity is 0.
    explicit LruCache(std::size_t capacity)
        : capacity_{capacity}
    {
        if (capacity == 0)
            throw std::invalid_argument{"LruCache: capacity must be > 0"};
    }

    // Non-copyable, non-movable — the mutex and list iterators make copying
    // semantically tricky.  If you need to pass one around, use shared_ptr.
    LruCache(const LruCache&)            = delete;
    LruCache& operator=(const LruCache&) = delete;

    // ── get ───────────────────────────────────────────────────────────────
    // Look up `key`.
    // If found:   moves the entry to the front of the list (most-recent),
    //             returns a copy of the value wrapped in std::optional.
    // If absent:  returns std::nullopt.
    //
    // Returns a copy (not a reference) because returning a reference into
    // the list would be unsafe — the caller might hold it while another
    // thread evicts the entry.
    std::optional<Value> get(const Key& key)
    {
        std::lock_guard<std::mutex> lock{mutex_};

        auto it = index_.find(key);
        if (it == index_.end())
            return std::nullopt;   // cache miss

        // Promote to most-recently-used by moving the node to the front.
        // std::list::splice is O(1) and doesn't copy or allocate.
        list_.splice(list_.begin(), list_, it->second);
        return it->second->second;  // return a copy of the value
    }

    // ── put ───────────────────────────────────────────────────────────────
    // Insert or update `key` → `value`.
    //   - If the key already exists: update the value and promote to front.
    //   - If the key is new:         insert at front; evict LRU if over cap.
    void put(const Key& key, Value value)
    {
        std::lock_guard<std::mutex> lock{mutex_};

        auto it = index_.find(key);
        if (it != index_.end()) {
            // Update existing entry and promote.
            it->second->second = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        // New entry: insert at front.
        list_.emplace_front(key, std::move(value));
        index_.emplace(key, list_.begin());

        // Evict the least-recently-used item if we've exceeded capacity.
        if (list_.size() > capacity_) {
            index_.erase(list_.back().first);
            list_.pop_back();
        }
    }

    // ── evict ─────────────────────────────────────────────────────────────
    // Remove a specific key.  No-op if the key is not present.
    // Useful for cache invalidation when the backend response changes.
    void evict(const Key& key)
    {
        std::lock_guard<std::mutex> lock{mutex_};

        auto it = index_.find(key);
        if (it == index_.end()) return;

        list_.erase(it->second);
        index_.erase(it);
    }

    // ── Observers ─────────────────────────────────────────────────────────

    // Number of entries currently in the cache.
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return list_.size();
    }

    std::size_t capacity() const { return capacity_; }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return list_.empty();
    }

private:
    // The list stores (Key, Value) pairs so eviction can erase the index
    // entry using only the list node — no reverse lookup needed.
    using Entry    = std::pair<Key, Value>;
    using List     = std::list<Entry>;
    using ListIter = typename List::iterator;

    std::size_t                            capacity_;
    mutable std::mutex                     mutex_;
    List                                   list_;
    std::unordered_map<Key, ListIter>      index_;
};

} // namespace aegis
