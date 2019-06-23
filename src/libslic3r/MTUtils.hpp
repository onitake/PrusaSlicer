#ifndef MTUTILS_HPP
#define MTUTILS_HPP

#include <atomic>       // for std::atomic_flag and memory orders
#include <mutex>        // for std::lock_guard
#include <functional>   // for std::function
#include <utility>      // for std::forward

namespace Slic3r {

/// Handy little spin mutex for the cached meshes.
/// Implements the "Lockable" concept
class SpinMutex {
    std::atomic_flag m_flg;
    static const /*constexpr*/ auto MO_ACQ = std::memory_order_acquire;
    static const /*constexpr*/ auto MO_REL = std::memory_order_release;
public:
    inline SpinMutex() { m_flg.clear(MO_REL); }
    inline void lock() { while(m_flg.test_and_set(MO_ACQ)); }
    inline bool try_lock() { return !m_flg.test_and_set(MO_ACQ); }
    inline void unlock() { m_flg.clear(MO_REL); }
};

/// A wrapper class around arbitrary object that needs thread safe caching.
template<class T> class CachedObject {
public:
    // Method type which refreshes the object when it has been invalidated
    using Setter = std::function<void(T&)>;
private:
    T m_obj;            // the object itself
    bool m_valid;       // invalidation flag
    SpinMutex m_lck;    // to make the caching thread safe

    // the setter will be called just before the object's const value is about
    // to be retrieved.
    std::function<void(T&)> m_setter;
public:

    // Forwarded constructor
    template<class...Args> inline CachedObject(Setter fn, Args&&...args):
        m_obj(std::forward<Args>(args)...), m_valid(false), m_setter(fn) {}

    // invalidate the value of the object. The object will be refreshed at the
    // next retrieval (Setter will be called). The data that is used in
    // the setter function should be guarded as well during modification so the
    // modification has to take place in fn.
    inline void invalidate(std::function<void()> fn) {
        std::lock_guard<SpinMutex> lck(m_lck); fn(); m_valid = false;
    }

    // Get the const object properly updated.
    inline const T& get() {
        std::lock_guard<SpinMutex> lck(m_lck);
        if(!m_valid) { m_setter(m_obj); m_valid = true; }
        return m_obj;
    }
};

/// An std compatible random access iterator which uses indices to the source
/// vector thus resistant to invalidation caused by relocations. It also "knows"
/// its container. No comparison is neccesary to the container "end()" iterator.
/// The template can be instantiated with a different value type than that of
/// the container's but the types must be compatible. E.g. a base class of the
/// contained objects is compatible.
///
/// For a constant iterator, one can instantiate this template with a value
/// type preceded with 'const'.
template<class Vector,  // The container type, must be random access...
         class Value = typename Vector::value_type // The value type
         >
class IndexBasedIterator {
    static const size_t NONE = size_t(-1);

    std::reference_wrapper<Vector> m_index_ref;
    size_t m_idx = NONE;
public:

    using value_type = Value;
    using pointer = Value *;
    using reference = Value &;
    using difference_type = long;
    using iterator_category = std::random_access_iterator_tag;

    inline explicit
    IndexBasedIterator(Vector& index, size_t idx):
        m_index_ref(index), m_idx(idx) {}

    // Post increment
    inline IndexBasedIterator operator++(int) {
        IndexBasedIterator cpy(*this); ++m_idx; return cpy;
    }

    inline IndexBasedIterator operator--(int) {
        IndexBasedIterator cpy(*this); --m_idx; return cpy;
    }

    inline IndexBasedIterator& operator++() {
        ++m_idx; return *this;
    }

    inline IndexBasedIterator& operator--() {
        --m_idx; return *this;
    }

    inline IndexBasedIterator& operator+=(difference_type l) {
        m_idx += size_t(l); return *this;
    }

    inline IndexBasedIterator operator+(difference_type l) {
        auto cpy = *this; cpy += l; return cpy;
    }

    inline IndexBasedIterator& operator-=(difference_type l) {
        m_idx -= size_t(l); return *this;
    }

    inline IndexBasedIterator operator-(difference_type l) {
        auto cpy = *this; cpy -= l; return cpy;
    }

    operator difference_type() { return difference_type(m_idx); }

    /// Tesing the end of the container... this is not possible with std
    /// iterators.
    inline bool is_end() const { return m_idx >= m_index_ref.get().size();}

    inline Value & operator*() const {
        assert(m_idx < m_index_ref.get().size());
        return m_index_ref.get().operator[](m_idx);
    }

    inline Value * operator->() const {
        assert(m_idx < m_index_ref.get().size());
        return &m_index_ref.get().operator[](m_idx);
    }

    /// If both iterators point past the container, they are equal...
    inline bool operator ==(const IndexBasedIterator& other) {
        size_t e = m_index_ref.get().size();
        return m_idx == other.m_idx || (m_idx >= e && other.m_idx >= e);
    }

    inline bool operator !=(const IndexBasedIterator& other) {
        return !(*this == other);
    }

    inline bool operator <=(const IndexBasedIterator& other) {
        return (m_idx < other.m_idx) || (*this == other);
    }

    inline bool operator <(const IndexBasedIterator& other) {
        return m_idx < other.m_idx && (*this != other);
    }

    inline bool operator >=(const IndexBasedIterator& other) {
        return m_idx > other.m_idx || *this == other;
    }

    inline bool operator >(const IndexBasedIterator& other) {
        return m_idx > other.m_idx && *this != other;
    }
};

/// A very simple range concept implementation with iterator-like objects.
template<class It> class Range {
    It from, to;
public:

    // The class is ready for range based for loops.
    It begin() const { return from; }
    It end() const { return to; }

    // The iterator type can be obtained this way.
    using Type = It;

    Range() = default;
    Range(It &&b, It &&e):
        from(std::forward<It>(b)), to(std::forward<It>(e)) {}

    // Some useful container-like methods...
    inline size_t size() const { return end() - begin(); }
    inline bool empty() const { return size() == 0; }
};

}

#endif // MTUTILS_HPP
