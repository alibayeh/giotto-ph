/*

 Ripser: a lean C++ code for computation of Vietoris-Rips persistence barcodes

 MIT License

 Copyright (c) 2015–2019 Ulrich Bauer

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

 You are under no obligation whatsoever to provide any bug fixes, patches, or
 upgrades to the features, functionality or performance of the source code
 ("Enhancements") to anyone; however, if you choose to make your Enhancements
 available either publicly, or directly to the author of this software, without
 imposing a separate written license agreement for such Enhancements, then you
 hereby grant the following license: a non-exclusive, royalty-free perpetual
 license to install, use, modify, prepare derivative works, incorporate into
 other computer software, distribute, and sublicense such enhancements or
 derivative works thereof, in binary and source code form.

*/

//#define USE_COEFFICIENTS

// #define INDICATE_PROGRESS

// #define USE_TRIVIAL_CONCURRENT_HASHMAP
#define USE_JUNCTION
#define USE_THREAD_POOL
#define CUSTOM_PARALLEL_SORT
#define SORT_BARCODES

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>  // rand
#include <cstring>  // memcpy
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <queue>
#include <sstream>
#include <unordered_map>

#include <future>
#include <thread>

/* Memory Manager */
#include <reclamation.hpp>

#ifdef USE_THREAD_POOL
#include <ctpl_stl.h>
#endif

using std::chrono::duration;
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

#if defined(USE_JUNCTION)
#include <junction/ConcurrentMap_Leapfrog.h>

template <class K, class D>
class TrivialIterator
{
private:
    std::pair<K, D> pair;

public:
    using key_type = K;
    using mapped_type = D;
    using value_type = std::pair<K, D>;
    using reference = value_type&;

    TrivialIterator(const key_type& k = key_type(),
                    const mapped_type& d = mapped_type())
        : pair(k, d)
    {
    }

    const std::pair<K, D>& operator*() { return pair; }
    std::pair<K, D>* operator->() { return &pair; }

    inline bool operator==(const TrivialIterator& r) const
    {
        return pair.first == r.pair.first;
    }
    inline bool operator!=(const TrivialIterator& r) const
    {
        return pair.first != r.pair.first;
    }
};

template <typename T>
struct ValueTraits {
    using IntType = T;

    static constexpr IntType NullValue = 0;
    static constexpr IntType Redirect = -1;
};

template <typename T>
constexpr T ValueTraits<T>::NullValue;

template <typename T>
constexpr T ValueTraits<T>::Redirect;

template <class Key, class T, class H, class E>
class hash_map
{
private:
    using junc_dflt_type = junction::DefaultKeyTraits<Key>;
    using internal_table_type =
        junction::ConcurrentMap_Leapfrog<Key, T, junc_dflt_type,
                                         ValueTraits<T>>;
    std::unique_ptr<internal_table_type> hash;
    junction::QSBR::Context qsbrContext;

    size_t next_power_2(size_t n)
    {
        size_t temp = 1;
        while (n >= temp)
            temp <<= 1;
        return temp;
    }

public:
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<const key_type, mapped_type>;
    using iterator = TrivialIterator<key_type, mapped_type>;
    using insert_return_type = std::pair<iterator, bool>;

    const T value(iterator it) const { return it->second; }
    hash_map() : hash(std::make_unique<internal_table_type>())
    {
        qsbrContext = junction::DefaultQSBR.createContext();
    }
    hash_map(size_t cap)
        : hash(std::make_unique<internal_table_type>(next_power_2(cap) << 1))
    {
        qsbrContext = junction::DefaultQSBR.createContext();
    }
    ~hash_map() { junction::DefaultQSBR.destroyContext(qsbrContext); }

    hash_map(const hash_map&) = delete;
    hash_map& operator=(const hash_map&) = delete;

    hash_map(hash_map&& rhs) = default;
    // hash_map& operator=(hash_map&& rhs) = default;
    hash_map& operator=(hash_map&& other)
    {
        if (this != &other) {
            this->hash = std::move(other.hash);
        }
        return *this;
    }

    iterator find(const key_type& k)
    {
        mapped_type r = hash->get(k + 1);

        if (r != ValueTraits<T>::NullValue)
            return iterator(k, r - 1);
        else
            return end();
    }

    insert_return_type insert(const key_type& k, const mapped_type& d)
    {
        auto mutator = hash->insertOrFind(k + 1);
        auto inserted = mutator.getValue() == ValueTraits<T>::NullValue;

        if (inserted)
            mutator.assignValue(d + 1);

        return insert_return_type(iterator(k, d), inserted);
    }

    insert_return_type insert(const value_type& d)
    {
        return insert(d.first, d.second);
    }

    bool update(iterator& it, T& expected, T desired)
    {
        return hash->exchange(it->first + 1, desired + 1) == (expected + 1);
    }

    void quiescent(void) { junction::DefaultQSBR.update(qsbrContext); }
    iterator end() { return iterator(-1); }
    void reserve(size_t hint) {}
    template <class F>
    void foreach (const F& f) const
    {
        for (typename internal_table_type::Iterator it(*hash); it.isValid();
             it.next()) {
            f(value_type(it.getKey() - 1, it.getValue() - 1));
        }
    }
};
#else
template <class Key, class T, class H, class E>
class hash_map : public std::unordered_map<Key, T, H, E>
{
public:
    using Parent = std::unordered_map<Key, T, H, E>;
    using iterator = typename Parent::iterator;

    Key key(iterator it) const { return it->first; }
    T value(iterator it) const { return it->second; }

    bool update(iterator it, T& expected, T desired)
    {
        it->second = desired;
        return true;
    }

    template <class F>
    void foreach (const F& f) const
    {
        for (auto& x : (*this))
            f(x);
    }
};
#endif

typedef float value_t;
typedef int64_t index_t;
typedef uint16_t coefficient_t;

#ifdef INDICATE_PROGRESS
static const std::chrono::milliseconds time_step(40);
#endif

static const std::string clear_line("\r\033[K");

static const size_t num_coefficient_bits = 8;

// 1L on windows is ALWAYS 32 bits, when on unix systems is pointer size
static const index_t max_simplex_index =
    (uintptr_t(1) << (8 * sizeof(index_t) - 1 - num_coefficient_bits)) - 1;

void check_overflow(index_t i)
{
    if
#ifdef USE_COEFFICIENTS
        (i > max_simplex_index)
#else
        (i < 0)
#endif
        throw std::overflow_error(
            "simplex index " + std::to_string((uint64_t) i) +
            " in filtration is larger than maximum index " +
            std::to_string(max_simplex_index));
}

class binomial_coeff_table
{
    using row_bc = std::vector<index_t>;
    using mat_bc = std::vector<row_bc>;
    mat_bc B;

public:
    /* Transposed binomial table
     * It's transposed because access where done over the rows and not the
     * columns */
    binomial_coeff_table(index_t k, index_t n) : B(n + 1, row_bc(k + 1, 0))
    {
        for (index_t i = 0; i <= k; ++i) {
            B[0][i] = 1;
            if (i <= n)
                B[i][i] = 1;
            for (index_t j = 1; j < std::min(i, n + 1); ++j) {
                B[j][i] = B[j - 1][i - 1] + B[j][i - 1];
            }
            check_overflow(B[std::min(i >> 1, n)][i]);
        }
    }

    index_t operator()(index_t k, index_t n) const
    {
        assert(n < B.size() && k < B[n].size() && n >= k - 1);
        return B[n][k];
    }
};

std::vector<coefficient_t> multiplicative_inverse_vector(const coefficient_t m)
{
    std::vector<coefficient_t> inverse(m);
    inverse[1] = 1;
    // m = a * (m / a) + m % a
    // Multiplying with inverse(a) * inverse(m % a):
    // 0 = inverse(m % a) * (m / a) + inverse(a)  (mod m)
    for (coefficient_t a = 2; a < m; ++a)
        inverse[a] = m - (inverse[m % a] * (m / a)) % m;
    return inverse;
}

#ifdef USE_COEFFICIENTS

// https://stackoverflow.com/a/3312896/13339777
#ifdef _MSC_VER
#define PACK(...) __pragma(pack(push, 1)) __VA_ARGS__ __pragma(pack(pop))
#else
#define PACK(...) __attribute__((__packed__)) __VA_ARGS__
#endif

PACK(struct entry_t {
    index_t index : 8 * sizeof(index_t) - num_coefficient_bits;
    index_t coefficient : num_coefficient_bits;
    entry_t(index_t _index, coefficient_t _coefficient)
        : index(_index), coefficient(_coefficient)
    {
    }
    constexpr entry_t(index_t _index) : index(_index), coefficient(0) {}
    constexpr entry_t() : index(0), coefficient(0) {}
    bool operator==(const entry_t& rhs) const { return index == rhs.index; }
    bool operator!=(const entry_t& rhs) const { return !(*this == rhs); }
    explicit operator index_t() const { return index; }
    entry_t operator+(index_t idx) const
    {
        return entry_t(index + idx, coefficient);
    }
    entry_t operator-(index_t idx) const
    {
        return entry_t(index - idx, coefficient);
    }
});

static_assert(sizeof(entry_t) == sizeof(index_t),
              "size of entry_t is not the same as index_t");

entry_t make_entry(index_t i, coefficient_t c) { return entry_t(i, c); }
index_t get_index(const entry_t& e) { return e.index; }
index_t get_coefficient(const entry_t& e) { return e.coefficient; }
void set_coefficient(entry_t& e, const coefficient_t c) { e.coefficient = c; }

std::ostream& operator<<(std::ostream& stream, const entry_t& e)
{
    stream << get_index(e) << ":" << get_coefficient(e);
    return stream;
}

#else

typedef index_t entry_t;
const index_t get_index(const entry_t& i) { return i; }
index_t get_coefficient(const entry_t& i) { return 1; }
entry_t make_entry(index_t _index, coefficient_t _value)
{
    return entry_t(_index);
}
void set_coefficient(entry_t& e, const coefficient_t c) {}

#endif

const entry_t& get_entry(const entry_t& e) { return e; }

typedef std::pair<value_t, index_t> diameter_index_t;
value_t get_diameter(const diameter_index_t& i) { return i.first; }
index_t get_index(const diameter_index_t& i) { return i.second; }

typedef std::pair<index_t, value_t> index_diameter_t;
index_t get_index(const index_diameter_t& i) { return i.first; }
value_t get_diameter(const index_diameter_t& i) { return i.second; }

struct diameter_entry_t : std::pair<value_t, entry_t> {
    using std::pair<value_t, entry_t>::pair;
    diameter_entry_t(value_t _diameter, index_t _index,
                     coefficient_t _coefficient)
        : diameter_entry_t(_diameter, make_entry(_index, _coefficient))
    {
    }
    diameter_entry_t(const diameter_index_t& _diameter_index,
                     coefficient_t _coefficient)
        : diameter_entry_t(get_diameter(_diameter_index),
                           make_entry(get_index(_diameter_index), _coefficient))
    {
    }
    diameter_entry_t(const diameter_index_t& _diameter_index)
        : diameter_entry_t(get_diameter(_diameter_index),
                           make_entry(get_index(_diameter_index), 0))
    {
    }
    diameter_entry_t(const index_t& _index) : diameter_entry_t(0, _index, 0) {}
};

const entry_t& get_entry(const diameter_entry_t& p) { return p.second; }
entry_t& get_entry(diameter_entry_t& p) { return p.second; }
const index_t get_index(const diameter_entry_t& p)
{
    return get_index(get_entry(p));
}
const coefficient_t get_coefficient(const diameter_entry_t& p)
{
    return get_coefficient(get_entry(p));
}
const value_t& get_diameter(const diameter_entry_t& p) { return p.first; }
void set_coefficient(diameter_entry_t& p, const coefficient_t c)
{
    set_coefficient(get_entry(p), c);
}

#if defined(CUSTOM_PARALLEL_SORT)
template <typename Iter, typename Comp>
void mergesort_mt3(Iter begin, Iter end,
                   Comp comp = std::less<diameter_entry_t>(),
                   unsigned int N = std::thread::hardware_concurrency() / 2
#if defined(USE_THREAD_POOL)
                   ,
                   ctpl::thread_pool* p = nullptr)
#else
)
#endif
{
    auto len = std::distance(begin, end);
    if (len <= 1024 || N < 2) {
        std::sort(begin, end, comp);
        return;
    }

    const size_t increment = len / N;

#if defined(USE_THREAD_POOL)
    std::vector<std::future<void>> futures;
    futures.reserve(N - 1);
#else
    std::vector<std::thread> threads;
    threads.reserve(N - 1);
#endif

    /* Sorting */
    for (size_t i = 1; i < N; ++i) {
        auto from = begin + i * increment;
        auto to = (i < (N - 1)) ? begin + (i + 1) * increment : end;

        futures.emplace_back(
#if defined(USE_THREAD_POOL)
            p->push([&, from, to](int idx) { std::sort(from, to, comp); }));
#else
        threads.emplace_back([&, from, to]() {
            std::sort(from, to, comp); });
#endif
    }

    std::sort(begin, begin + increment, comp);
#if defined(USE_THREAD_POOL)
    for (auto& fut : futures)
        fut.get();
#else
    for (auto& th : threads)
        th.join();
#endif

    /* Merging */
    size_t nb_chunks = N;
    size_t chunk_size = increment;
    size_t mid_off = chunk_size;

    while (nb_chunks > 2) {
        const bool is_even = (nb_chunks & 1) == 0;
        mid_off += chunk_size;
        chunk_size *= 2;
        nb_chunks /= 2;
#if defined(USE_THREAD_POOL)
        futures.clear();
#else
        threads.clear();
#endif

        if (nb_chunks > 1) {
            for (size_t j = 1; j < nb_chunks; ++j) {
                bool is_last = j == (nb_chunks - 1);
                auto from = begin + chunk_size * j;
                auto mid = from + chunk_size / 2;
                auto to = (is_last && is_even) ? end : from + chunk_size;

#if defined(USE_THREAD_POOL)
                futures.emplace_back(p->push([&, from, mid, to](int idx) {
                    std::inplace_merge(from, mid, to, comp);
                }));
#else
                threads.emplace_back([&, from, mid, to]() {
                    std::inplace_merge(from, mid, to, comp);
                });
#endif
            }
        }

        std::inplace_merge(begin, begin + chunk_size / 2, begin + chunk_size,
                           comp);

#if defined(USE_THREAD_POOL)
        for (auto& fut : futures)
            fut.get();
#else
        for (auto& th : threads)
            th.join();
#endif
        nb_chunks += is_even ? 0 : 1;
    }

    std::inplace_merge(begin, begin + mid_off, end, comp);
}

#endif

template <typename Entry>
struct greater_diameter_or_smaller_index {
    bool operator()(const Entry& a, const Entry& b) const
    {
        return (get_diameter(a) > get_diameter(b)) ||
               ((get_diameter(a) == get_diameter(b)) &&
                (get_index(a) < get_index(b)));
    }
};

enum compressed_matrix_layout { LOWER_TRIANGULAR, UPPER_TRIANGULAR };

template <compressed_matrix_layout Layout>
struct compressed_distance_matrix {
    std::vector<value_t> distances;
    std::vector<value_t*> rows;

    compressed_distance_matrix(std::vector<value_t>&& _distances)
        : distances(std::move(_distances)),
          rows((1 + std::sqrt(1 + 8 * distances.size())) / 2)
    {
        assert(distances.size() == size() * (size() - 1) / 2);
        init_rows();
    }

    template <typename DistanceMatrix>
    compressed_distance_matrix(const DistanceMatrix& mat)
        : distances(mat.size() * (mat.size() - 1) / 2), rows(mat.size())
    {
        init_rows();

        for (size_t i = 1; i < size(); ++i)
            for (size_t j = 0; j < i; ++j)
                rows[i][j] = mat(i, j);
    }

    value_t operator()(const index_t i, const index_t j) const;

    size_t size() const { return rows.size(); }
    void init_rows();
};

typedef compressed_distance_matrix<LOWER_TRIANGULAR>
    compressed_lower_distance_matrix;
typedef compressed_distance_matrix<UPPER_TRIANGULAR>
    compressed_upper_distance_matrix;

template <>
void compressed_lower_distance_matrix::init_rows()
{
    value_t* pointer = &distances[0];
    for (size_t i = 1; i < size(); ++i) {
        rows[i] = pointer;
        pointer += i;
    }
}

template <>
void compressed_upper_distance_matrix::init_rows()
{
    value_t* pointer = &distances[0] - 1;
    for (size_t i = 0; i < size() - 1; ++i) {
        rows[i] = pointer;
        pointer += size() - i - 2;
    }
}

template <>
value_t compressed_lower_distance_matrix::operator()(const index_t i,
                                                     const index_t j) const
{
    return i == j ? 0 : i < j ? rows[j][i] : rows[i][j];
}

template <>
value_t compressed_upper_distance_matrix::operator()(const index_t i,
                                                     const index_t j) const
{
    return i == j ? 0 : i > j ? rows[j][i] : rows[i][j];
}

struct sparse_distance_matrix {
    std::vector<std::vector<index_diameter_t>> neighbors;
    std::vector<value_t> vertex_births;
    index_t num_edges;

    sparse_distance_matrix(
        std::vector<std::vector<index_diameter_t>>&& _neighbors,
        index_t _num_edges)
        : neighbors(std::move(_neighbors)), vertex_births(neighbors.size(), 0),
          num_edges(_num_edges)
    {
    }

    template <typename DistanceMatrix>
    sparse_distance_matrix(const DistanceMatrix& mat, const value_t threshold)
        : neighbors(mat.size()), vertex_births(mat.size(), 0), num_edges(0)
    {
        for (size_t i = 0; i < size(); ++i)
            for (size_t j = 0; j < size(); ++j)
                if (i != j && mat(i, j) <= threshold) {
                    ++num_edges;
                    neighbors[i].push_back({j, mat(i, j)});
                }
    }

    // Initialize from COO format
    sparse_distance_matrix(index_t* I, index_t* J, value_t* V, int NEdges,
                           int N, const value_t threshold)
        : neighbors(N), vertex_births(N, 0), num_edges(0)
    {
        int i, j;
        value_t val;
        for (int idx = 0; idx < NEdges; idx++) {
            i = I[idx];
            j = J[idx];
            val = V[idx];
            if (i < j && val <= threshold) {
                neighbors[i].push_back(std::make_pair(j, val));
                neighbors[j].push_back(std::make_pair(i, val));
                ++num_edges;
            } else if (i == j) {
                vertex_births[i] = val;
            }
        }

        for (size_t i = 0; i < neighbors.size(); ++i)
            std::sort(neighbors[i].begin(), neighbors[i].end());
    }

    value_t operator()(const index_t i, const index_t j) const
    {
        auto neighbor =
            std::lower_bound(neighbors[i].begin(), neighbors[i].end(),
                             index_diameter_t{j, vertex_births[j]});
        return (neighbor != neighbors[i].end() && get_index(*neighbor) == j)
                   ? get_diameter(*neighbor)
                   : std::numeric_limits<value_t>::infinity();
    }

    size_t size() const { return neighbors.size(); }
};

struct euclidean_distance_matrix {
    std::vector<std::vector<value_t>> points;

    euclidean_distance_matrix(std::vector<std::vector<value_t>>&& _points)
        : points(std::move(_points))
    {
        for (auto p : points) {
            assert(p.size() == points.front().size());
        }
    }

    value_t operator()(const index_t i, const index_t j) const
    {
        assert(i < points.size());
        assert(j < points.size());
        return std::sqrt(std::inner_product(
            points[i].begin(), points[i].end(), points[j].begin(), value_t(),
            std::plus<value_t>(),
            [](value_t u, value_t v) { return (u - v) * (u - v); }));
    }

    size_t size() const { return points.size(); }
};

class union_find
{
    std::vector<index_t> parent;
    std::vector<uint8_t> rank;
    std::vector<value_t> birth;

public:
    union_find(const index_t n) : parent(n), rank(n, 0), birth(n, 0)
    {
        // Fills the range [first, last) with sequentially increasing values
        std::iota(parent.begin(), parent.end(), 0);
    }

    void set_birth(index_t i, value_t val) { birth[i] = val; }
    value_t get_birth(index_t i) { return birth[i]; }

    index_t find(index_t x)
    {
        index_t y = x, z = parent[y];
        while (z != y) {
            y = z;
            z = parent[y];
        }
        y = parent[x];
        while (z != y) {
            parent[x] = z;
            x = y;
            y = parent[x];
        }
        return z;
    }

    void link(index_t x, index_t y)
    {
        x = find(x);
        y = find(y);
        if (x == y)
            return;
        if (rank[x] > rank[y]) {
            parent[y] = x;
            birth[x] = std::min(birth[x], birth[y]);  // Elder rule
        } else {
            parent[x] = y;
            birth[y] = std::min(birth[x], birth[y]);  // Elder rule
            if (rank[x] == rank[y])
                ++rank[y];
        }
    }
};

template <typename T>
T begin(std::pair<T, T>& p)
{
    return p.first;
}
template <typename T>
T end(std::pair<T, T>& p)
{
    return p.second;
}

template <typename ValueType>
class compressed_sparse_matrix
{
public:
    using Column = std::vector<ValueType>;

    compressed_sparse_matrix(size_t n) : columns(n)
    {
        for (auto& cell : columns) {
            /* init not thread safe, initialization is only done in 1 thread */
            std::atomic_init(&cell, static_cast<Column*>(nullptr));
        }
    }
    ~compressed_sparse_matrix()
    {
        for (auto& cell : columns)
            if (cell)
                delete cell;
    }

    Column* column(const index_t index) { return columns[index].load(); }

    Column* exchange(const index_t index, Column* desired) noexcept
    {
        return columns[index].exchange(desired);
    }

private:
    std::vector<std::atomic<Column*>> columns;
};

template <class Predicate>
index_t get_max(index_t top, index_t count, const Predicate pred)
{
    while (count > 0) {
        index_t step = count >> 1, mid = top - step;
        if (!pred(mid)) {
            top = mid - 1;
            step = count - (step + 1);
        }
        count = step;
    }
    return top;
}

/* This is the data structure from which the results of running ripser can be
 * returned */
typedef struct {
    /* The first variable is a vector of unrolled persistence diagrams
       so, for example births_and_deaths_by_dim[0] contains a list of
                [birth0, death0, birth1, death1, ..., birthk, deathk]
       for k points in the 0D persistence diagram
       and likewise for d-dimensional persistence in births_and_deaths_by_dim[d]
    */
    std::vector<std::vector<value_t>> births_and_deaths_by_dim;
    /* The second variable is the number of edges that were added during the
     * computation*/
    int num_edges;
} ripserResults;

template <typename DistanceMatrix>
class ripser
{
    const DistanceMatrix dist;
    const index_t n, dim_max;
    const value_t threshold;
    const float ratio;
    const coefficient_t modulus;
    const unsigned num_threads;
    const binomial_coeff_table binomial_coeff;
    const std::vector<coefficient_t> multiplicative_inverse;
    ctpl::thread_pool p;

    struct entry_hash {
        std::size_t operator()(const entry_t& e) const
        {
            return std::hash<index_t>()(::get_index(e));
        }
    };

    struct equal_index {
        bool operator()(const entry_t& e, const entry_t& f) const
        {
            return ::get_index(e) == ::get_index(f);
        }
    };

    using entry_hash_map = hash_map<index_t, entry_t, entry_hash, equal_index>;
    using Matrix = compressed_sparse_matrix<diameter_entry_t>;
    using MatrixColumn = typename Matrix::Column;
    using WorkingColumn = std::priority_queue<
        diameter_entry_t, std::vector<diameter_entry_t>,
        greater_diameter_or_smaller_index<diameter_entry_t>>;
    using mat_simplicies_t = std::vector<std::vector<diameter_index_t>>;

private:
    const bool is_not_present(entry_hash_map& hm,
                              const diameter_entry_t& elem) const
    {
        return hm.find(get_index(get_entry(elem))) == hm.end();
    }

public:
    mutable std::vector<std::vector<value_t>> births_and_deaths_by_dim;

    ripser(DistanceMatrix&& _dist, index_t _dim_max, value_t _threshold,
           float _ratio, coefficient_t _modulus, unsigned _num_threads)
        : dist(std::move(_dist)), n(dist.size()), dim_max(_dim_max),
          threshold(_threshold), ratio(_ratio), modulus(_modulus),
          num_threads((!_num_threads ? 1 : _num_threads)),
          binomial_coeff(n, dim_max + 2),
          multiplicative_inverse(multiplicative_inverse_vector(_modulus)),
          p(num_threads)
    {
    }

    void copy_results(ripserResults& res)
    {
        res.births_and_deaths_by_dim = births_and_deaths_by_dim;
    }

    index_t get_max_vertex(const index_t idx, const index_t k,
                           const index_t n) const
    {
        auto pred = [&](index_t w) -> bool {
            return (binomial_coeff(w, k) <= idx);
        };

        const index_t cnt = n - (k - 1);
        if (pred(n) || (cnt <= 0))
            return n;
        return get_max(n, cnt, pred);
    }

    index_t get_edge_index(const index_t i, const index_t j) const
    {
        return binomial_coeff(i, 2) + j;
    }

    template <typename OutputIterator>
    OutputIterator get_simplex_vertices(index_t idx, const index_t dim,
                                        index_t n, OutputIterator out) const
    {
        --n;
        for (index_t k = dim + 1; k > 0; --k) {
            n = get_max_vertex(idx, k, n);
            *out++ = n;
            idx -= binomial_coeff(n, k);
        }
        return out;
    }

    value_t compute_diameter(const index_t index, const index_t dim) const
    {
        std::vector<index_t> vertices;
        value_t diam = -std::numeric_limits<value_t>::infinity();

        vertices.resize(dim + 1);
        get_simplex_vertices(index, dim, dist.size(), vertices.rbegin());

        for (index_t i = 0; i <= dim; ++i)
            for (index_t j = 0; j < i; ++j) {
                diam = std::max(diam, dist(vertices[i], vertices[j]));
            }
        return diam;
    }

    class simplex_coboundary_enumerator;

    class simplex_boundary_enumerator
    {
    private:
        index_t idx_below, idx_above, j, k;
        diameter_entry_t simplex;
        index_t dim;
        const coefficient_t modulus;
        const binomial_coeff_table& binomial_coeff;
        const ripser& parent;

    public:
        simplex_boundary_enumerator(const diameter_entry_t _simplex,
                                    const index_t _dim, const ripser& _parent)
            : idx_below(get_index(_simplex)), idx_above(0), j(_parent.n - 1),
              k(_dim), simplex(_simplex), modulus(_parent.modulus),
              binomial_coeff(_parent.binomial_coeff), parent(_parent)
        {
        }

        simplex_boundary_enumerator(const index_t _dim, const ripser& _parent)
            : simplex_boundary_enumerator(-1, _dim, _parent)
        {
        }

        void set_simplex(const diameter_entry_t _simplex, const index_t _dim)
        {
            idx_below = get_index(_simplex);
            idx_above = 0;
            j = parent.n - 1;
            k = _dim;
            simplex = _simplex;
            dim = _dim;
        }

        bool has_next() { return (k >= 0); }

        diameter_entry_t next()
        {
            j = parent.get_max_vertex(idx_below, k + 1, j);

            index_t face_index =
                idx_above - binomial_coeff(j, k + 1) + idx_below;

            value_t face_diameter =
                parent.compute_diameter(face_index, dim - 1);

            coefficient_t face_coefficient =
                (k & 1 ? -1 + modulus : 1) * get_coefficient(simplex) % modulus;

            idx_below -= binomial_coeff(j, k + 1);
            idx_above += binomial_coeff(j, k);

            --k;

            return diameter_entry_t(face_diameter, face_index,
                                    face_coefficient);
        }
    };

    diameter_entry_t get_zero_pivot_facet(const diameter_entry_t simplex,
                                          const index_t dim)
    {
        simplex_boundary_enumerator facets(0, *this);
        facets.set_simplex(simplex, dim);
        while (facets.has_next()) {
            diameter_entry_t facet = facets.next();
            if (get_diameter(facet) == get_diameter(simplex))
                return facet;
        }
        return diameter_entry_t(-1);
    }

    diameter_entry_t get_zero_pivot_cofacet(const diameter_entry_t simplex,
                                            const index_t dim)
    {
        simplex_coboundary_enumerator cofacets(*this);
        cofacets.set_simplex(simplex, dim);
        while (cofacets.has_next()) {
            diameter_entry_t cofacet = cofacets.next();
            if (get_diameter(cofacet) == get_diameter(simplex))
                return cofacet;
        }
        return diameter_entry_t(-1);
    }

    diameter_entry_t get_zero_apparent_facet(const diameter_entry_t simplex,
                                             const index_t dim)
    {
        diameter_entry_t facet = get_zero_pivot_facet(simplex, dim);
        return ((get_index(facet) != -1) &&
                (get_index(get_zero_pivot_cofacet(facet, dim - 1)) ==
                 get_index(simplex)))
                   ? facet
                   : diameter_entry_t(-1);
    }

    diameter_entry_t get_zero_apparent_cofacet(const diameter_entry_t simplex,
                                               const index_t dim)
    {
        diameter_entry_t cofacet = get_zero_pivot_cofacet(simplex, dim);
        return ((get_index(cofacet) != -1) &&
                (get_index(get_zero_pivot_facet(cofacet, dim + 1)) ==
                 get_index(simplex)))
                   ? cofacet
                   : diameter_entry_t(-1);
    }

    bool is_in_zero_apparent_pair(const diameter_entry_t simplex,
                                  const index_t dim)
    {
        return (get_index(get_zero_apparent_cofacet(simplex, dim)) != -1) ||
               (get_index(get_zero_apparent_facet(simplex, dim)) != -1);
    }

    void
    assemble_columns_to_reduce(std::vector<diameter_index_t>& simplices,
                               std::vector<diameter_index_t>& columns_to_reduce,
                               entry_hash_map& pivot_column_index, index_t dim)
    {
#ifdef INDICATE_PROGRESS
        std::cerr << clear_line << "assembling columns" << std::flush;
        std::chrono::steady_clock::time_point next =
            std::chrono::steady_clock::now() + time_step;
#endif
        const bool check_clearing = !!columns_to_reduce.size();
        columns_to_reduce.clear();
        std::vector<diameter_index_t> next_simplices;
        size_t chunk_size = (simplices.size() / num_threads) >> 2;
        chunk_size = (chunk_size) ? chunk_size : 1;
        std::atomic<size_t> achunk(0);

#ifdef INDICATE_PROGRESS
        std::atomic<int> progress{0};
#endif

        const size_t n_thr = (chunk_size < num_threads) ? 1 : num_threads;
        mat_simplicies_t next_simplices_vec(n_thr);
        mat_simplicies_t columns_to_reduce_vec(n_thr);
#if defined(USE_THREAD_POOL)
        std::vector<std::future<void>> futures;
        futures.reserve(n_thr);
        for (unsigned i = 0; i < n_thr; ++i)
            futures.emplace_back(p.push([&, i](int t_idx) {
#else
        std::vector<std::thread> threads;
        threads.reserve(n_thr);
        for (unsigned i = 0; i < n_thr; ++i)
            threads.emplace_back([&, i]() {
#endif

#ifdef INDICATE_PROGRESS
                int indicate_progress = progress++;
#endif
                simplex_coboundary_enumerator cofacets(*this);
                for (size_t cur_chunk = achunk++;
                     cur_chunk * chunk_size < simplices.size();
                     cur_chunk = achunk++) {
                    size_t from = cur_chunk * chunk_size;
                    size_t to = std::min((cur_chunk + 1) * chunk_size,
                                         simplices.size());

                    for (; from < to; ++from) {
                        const auto& simplex = simplices[from];
                        cofacets.set_simplex(diameter_entry_t(simplex, 1),
                                             dim - 1);
                        while (cofacets.has_next(false)) {
#ifdef INDICATE_PROGRESS
                            if (indicate_progress == 0) {
                                if (std::chrono::steady_clock::now() > next) {
                                    std::cerr
                                        << clear_line
                                        << "assembling columns (processing "
                                        << from << "/" << simplices.size()
                                        << " simplices)" << std::flush;
                                    next = std::chrono::steady_clock::now() +
                                           time_step;
                                }
                            }
#endif
                            const auto& cofacet = cofacets.next();
                            if (get_diameter(cofacet) <= threshold) {
                                if (dim != dim_max) {
                                    next_simplices_vec[i].push_back(
                                        {get_diameter(cofacet),
                                         get_index(cofacet)});
                                }

                                if (!is_in_zero_apparent_pair(cofacet, dim) &&
                                    (!check_clearing ||
                                     (check_clearing &&
                                      is_not_present(pivot_column_index,
                                                     cofacet))))
                                    columns_to_reduce_vec[i].push_back(
                                        {get_diameter(cofacet),
                                         get_index(cofacet)});
                            }
                        }
                    }
                }
#if defined(USE_THREAD_POOL)
            }));
        for (auto& fut : futures)
            fut.get();
#else
            });
        for (auto& thread : threads)
            thread.join();
#endif
        /* flatten 2d vectors */
        size_t simplices_prefix(0);
        size_t columns_to_reduce_prefix(0);

        for (const auto& sub : next_simplices_vec)
            simplices_prefix += sub.size();
        for (const auto& sub : columns_to_reduce_vec)
            columns_to_reduce_prefix += sub.size();

        next_simplices.reserve(simplices_prefix);
        columns_to_reduce.reserve(columns_to_reduce_prefix);

        for (const auto& sub : next_simplices_vec)
            next_simplices.insert(next_simplices.end(), sub.begin(), sub.end());
        for (const auto& sub : columns_to_reduce_vec)
            columns_to_reduce.insert(columns_to_reduce.end(), sub.begin(),
                                     sub.end());
        // copy into the arrays
        simplices.swap(next_simplices);

#ifdef INDICATE_PROGRESS
        std::cerr << clear_line << "sorting " << columns_to_reduce.size()
                  << " columns" << std::flush;
#endif

#if defined(USE_THREAD_POOL)
        /* Pre-allocate in parallel the hash map for next dimension */
        if (columns_to_reduce.size()) {
            auto fut = p.push([&](int idx) {
                pivot_column_index =
                    std::move(entry_hash_map(columns_to_reduce.size()));
            });

            mergesort_mt3(columns_to_reduce.begin(), columns_to_reduce.end(),
                          greater_diameter_or_smaller_index<diameter_index_t>(),
                          num_threads - 1, &p);

            fut.get();
        }
#else
        std::thread thread_(
            mergesort_mt3<decltype(columns_to_reduce.begin()),
                          greater_diameter_or_smaller_index<diameter_index_t>>,
            columns_to_reduce.begin(), columns_to_reduce.end(),
            greater_diameter_or_smaller_index<diameter_index_t>(), num_threads);
        pivot_column_index =
            std::move(entry_hash_map(columns_to_reduce.size()));
        thread_.join();
#endif
#ifdef INDICATE_PROGRESS
        std::cerr << clear_line << std::flush;
#endif
    }

    value_t get_vertex_birth(index_t i);
    void compute_dim_0_pairs(std::vector<diameter_index_t>& edges,
                             std::vector<diameter_index_t>& columns_to_reduce)
    {
        // TODO: Get correct birth times if the edges are negative (required for
        // lower star)
        union_find dset(n);
        for (index_t i = 0; i < n; i++) {
            dset.set_birth(i, get_vertex_birth(i));
        }

        edges = get_edges();
        mergesort_mt3(edges.rbegin(), edges.rend(),
                      greater_diameter_or_smaller_index<diameter_index_t>(),
                      num_threads
#if defined(USE_THREAD_POOL)
                      ,
                      &p);
#endif
        std::vector<index_t> vertices_of_edge(2);
        columns_to_reduce.resize(edges.size());
        size_t i = 0;
        for (auto e : edges) {
            get_simplex_vertices(get_index(e), 1, n, vertices_of_edge.rbegin());
            index_t u = dset.find(vertices_of_edge[0]),
                    v = dset.find(vertices_of_edge[1]);

            if (u != v) {
                if (get_diameter(e) != 0) {
                    // Elder rule; youngest class (max birth time of u and v)
                    // dies first
                    value_t birth =
                        std::max(dset.get_birth(u), dset.get_birth(v));
                    value_t death = get_diameter(e);
                    if (death > birth) {
                        births_and_deaths_by_dim[0].push_back(birth);
                        births_and_deaths_by_dim[0].push_back(death);
                    }
                }
                dset.link(u, v);
            } else if (get_index(get_zero_apparent_cofacet(e, 1)) == -1)
                columns_to_reduce[i++] = e;
        }
        columns_to_reduce.resize(i);
        std::reverse(columns_to_reduce.begin(), columns_to_reduce.end());

        for (index_t i = 0; i < n; ++i)
            if (dset.find(i) == i) {
                births_and_deaths_by_dim[0].push_back(dset.get_birth(i));
                births_and_deaths_by_dim[0].push_back(
                    std::numeric_limits<value_t>::infinity());
            }
    }

    diameter_entry_t pop_pivot(WorkingColumn& column)
    {
        diameter_entry_t pivot(-1);
#ifdef USE_COEFFICIENTS
        while (!column.empty()) {
            if (get_coefficient(pivot) == 0)
                pivot = column.top();
            else if (get_index(column.top()) != get_index(pivot))
                return pivot;
            else
                set_coefficient(pivot, (get_coefficient(pivot) +
                                        get_coefficient(column.top())) %
                                           modulus);
            column.pop();
        }
        return (get_coefficient(pivot) == 0) ? -1 : pivot;
#else
        while (!column.empty()) {
            pivot = column.top();
            column.pop();
            if (column.empty() || get_index(column.top()) != get_index(pivot))
                return pivot;
            column.pop();
        }
        return -1;
#endif
    }

    diameter_entry_t get_pivot(WorkingColumn& column)
    {
        diameter_entry_t result = pop_pivot(column);
        if (get_index(result) != -1)
            column.push(result);
        return result;
    }

    std::pair<diameter_entry_t, bool> init_coboundary_and_get_pivot(
        const diameter_entry_t simplex, WorkingColumn& working_coboundary,
        const index_t& dim, entry_hash_map& pivot_column_index,
        const index_t index_column_to_reduce)
    {
        std::vector<diameter_entry_t> cofacet_entries;
        bool check_for_emergent_pair = true;
        cofacet_entries.clear();
        cofacet_entries.reserve(dim + 1);
        simplex_coboundary_enumerator cofacets(*this);
        cofacets.set_simplex(simplex, dim);
        while (cofacets.has_next()) {
            diameter_entry_t cofacet = cofacets.next();
            if (get_diameter(cofacet) <= threshold) {
                cofacet_entries.push_back(cofacet);
                if (check_for_emergent_pair &&
                    (get_diameter(simplex) == get_diameter(cofacet))) {
                    if (is_not_present(pivot_column_index, cofacet) &&
                        (get_index(get_zero_apparent_facet(cofacet, dim + 1)) ==
                         -1))
                        if (pivot_column_index
                                .insert({get_index(get_entry(cofacet)),
                                         make_entry(index_column_to_reduce,
                                                    get_coefficient(cofacet))})
                                .second)
                            return {cofacet, true};
                    check_for_emergent_pair = false;
                }
            }
        }
        for (auto cofacet : cofacet_entries)
            working_coboundary.push(cofacet);
        return {get_pivot(working_coboundary), false};
    }

    void add_simplex_coboundary(const diameter_entry_t simplex,
                                const index_t& dim,
                                WorkingColumn& working_reduction_column,
                                WorkingColumn& working_coboundary,
                                bool add_diagonal = true)
    {
        if (add_diagonal)
            working_reduction_column.push(simplex);
        simplex_coboundary_enumerator cofacets(*this);
        cofacets.set_simplex(simplex, dim);
        while (cofacets.has_next()) {
            diameter_entry_t cofacet = cofacets.next();
            if (get_diameter(cofacet) <= threshold)
                working_coboundary.push(cofacet);
        }
    }

    void add_coboundary(MatrixColumn* reduction_column_to_add,
                        const std::vector<diameter_index_t>& columns_to_reduce,
                        const index_t index_column_to_add,
                        const coefficient_t factor, const size_t& dim,
                        WorkingColumn& working_reduction_column,
                        WorkingColumn& working_coboundary,
                        bool add_diagonal = true)
    {
        diameter_entry_t column_to_add(columns_to_reduce[index_column_to_add],
                                       factor);
        add_simplex_coboundary(column_to_add, dim, working_reduction_column,
                               working_coboundary, add_diagonal);

        if (!reduction_column_to_add)
            return;
        for (diameter_entry_t simplex : *reduction_column_to_add) {
            set_coefficient(simplex,
                            get_coefficient(simplex) * factor % modulus);
            add_simplex_coboundary(simplex, dim, working_reduction_column,
                                   working_coboundary);
        }
    }

    MatrixColumn* generate_column(WorkingColumn&& working_reduction_column)
    {
        if (working_reduction_column.empty())
            return nullptr;

        MatrixColumn column;
        while (true) {
            diameter_entry_t e = pop_pivot(working_reduction_column);
            if (get_index(e) == -1)
                break;
            assert(get_coefficient(e) > 0);
            column.push_back(e);
        }

        if (!column.size())
            return nullptr;
        return new MatrixColumn(std::move(column));
    }

    template <class F>
    void foreach (const std::vector<diameter_index_t>& columns_to_reduce,
                  const F& f)
    {
#if defined(INDICATE_PROGRESS)
        std::atomic<int> progress(0);
        std::cerr << clear_line << "Starting reduction of "
                  << columns_to_reduce.size() << " columns" << std::endl;
#endif
        std::atomic<size_t> achunk(0);
        size_t chunk_size = (columns_to_reduce.size() / num_threads) >> 2;
        chunk_size = (chunk_size) ? chunk_size : 1;
        mrzv::MemoryManager<MatrixColumn> memory_manager(num_threads);

        const size_t n_thr = (chunk_size < num_threads) ? 1 : num_threads;

#if defined(USE_THREAD_POOL)
        std::vector<std::future<void>> futures;
        futures.reserve(n_thr);
        for (unsigned i = 0; i < n_thr; ++i)
            futures.emplace_back(p.push([&](int t_idx) {
#else
        std::vector<std::thread> threads;
        threads.reserve(n_thr);
        for (unsigned t = 0; t < n_thr; ++t)
            threads.emplace_back([&]() {
#endif

#ifdef INDICATE_PROGRESS
                int indicate_progress = progress++;
                std::chrono::steady_clock::time_point next =
                    std::chrono::steady_clock::now() + time_step;
#endif

                for (size_t cur_chunk = achunk++;
                     cur_chunk * chunk_size < columns_to_reduce.size();
                     cur_chunk = achunk++) {
                    size_t from = cur_chunk * chunk_size;
                    const size_t to = std::min((cur_chunk + 1) * chunk_size,
                                               columns_to_reduce.size());
#ifdef INDICATE_PROGRESS
                    if (indicate_progress == 0) {
                        if (std::chrono::steady_clock::now() > next) {
                            std::cerr << clear_line << "reducing columns "
                                      << from << " - " << to << "/"
                                      << columns_to_reduce.size() << std::flush;
                            next = std::chrono::steady_clock::now() + time_step;
                        }
                    }
#endif
                    for (; from < to; ++from) {
                        size_t idx_col_to_reduce = from;
                        /* first run => true parameter */
                        size_t previous =
                            f(idx_col_to_reduce, true, memory_manager);
                        while (previous != idx_col_to_reduce) {
                            idx_col_to_reduce = previous;
                            /* not first run => false parameter */
                            previous =
                                f(idx_col_to_reduce, false, memory_manager);
                        }
                    }

                    memory_manager.quiescent();
                }
#if defined(USE_THREAD_POOL)
            }));
        for (auto& fut : futures)
            fut.get();
#else
            });

        for (auto& thread : threads)
            thread.join();
#endif
    }

#if !defined(NDEBUG)
    // debug only
    diameter_entry_t
    get_column_pivot(MatrixColumn* column,
                     const std::vector<diameter_index_t>& columns_to_reduce,
                     const index_t index, const coefficient_t factor,
                     const size_t& dim)
    {
        WorkingColumn tmp_working_reduction_column, tmp_working_coboundary;
        add_coboundary(column, columns_to_reduce, index, 1, dim,
                       tmp_working_reduction_column, tmp_working_coboundary);
        return get_pivot(tmp_working_coboundary);
    }
#endif

    void retire_column(const index_t& idx_col,
                       WorkingColumn& working_reduction_column, Matrix& mat,
                       const std::vector<diameter_index_t>& columns_to_reduce,
                       const index_t dim, const index_t& pivot_index,
                       mrzv::MemoryManager<MatrixColumn>& mem_manager)
    {
        MatrixColumn* new_column =
            generate_column(std::move(working_reduction_column));
        MatrixColumn* previous = mat.exchange(idx_col, new_column);
        mem_manager.retire(previous);

        assert(get_index(get_column_pivot(new_column, columns_to_reduce,
                                          idx_col, 1, dim)) == pivot_index);
    }

    void compute_pairs(const std::vector<diameter_index_t>& columns_to_reduce,
                       entry_hash_map& pivot_column_index, const index_t dim)
    {
        Matrix reduction_matrix(columns_to_reduce.size());

        // extra vector is a work-around inability to store floats in the
        // hash_map
        std::atomic<size_t> last_diameter_index{0}, idx_essential{0};
        entry_hash_map deaths;
        deaths.reserve(columns_to_reduce.size());
        std::vector<value_t> diameters(columns_to_reduce.size());
        std::vector<value_t> essential_pair;
        essential_pair.resize(columns_to_reduce.size());

        foreach (columns_to_reduce, [&](index_t index_column_to_reduce,
                                        bool first,
                                        mrzv::MemoryManager<MatrixColumn>&
                                            memory_manager) {
            const diameter_entry_t column_to_reduce(
                columns_to_reduce[index_column_to_reduce], 1);

            WorkingColumn working_reduction_column, working_coboundary;

            diameter_entry_t pivot;
            if (first) {
                bool emergent;
                std::tie(pivot, emergent) = init_coboundary_and_get_pivot(
                    column_to_reduce, working_coboundary, dim,
                    pivot_column_index, index_column_to_reduce);
                if (emergent)
                    return index_column_to_reduce;
            } else {
                MatrixColumn* reduction_column_to_reduce =
                    reduction_matrix.column(index_column_to_reduce);
                add_coboundary(reduction_column_to_reduce, columns_to_reduce,
                               index_column_to_reduce, 1, dim,
                               working_reduction_column, working_coboundary,
                               false);
                pivot = get_pivot(working_coboundary);
            }
            diameter_entry_t e;

            while (true) {
                if (get_index(pivot) != -1) {
                    auto pair =
                        pivot_column_index.find(get_index(get_entry(pivot)));
                    if (pair != pivot_column_index.end()) {
                        entry_t old_entry_column_to_add;
                        index_t index_column_to_add;
                        MatrixColumn* reduction_column_to_add;
                        entry_t entry_column_to_add =
                            pivot_column_index.value(pair);
                        do {
                            old_entry_column_to_add = entry_column_to_add;

                            index_column_to_add =
                                get_index(entry_column_to_add);

                            reduction_column_to_add =
                                reduction_matrix.column(index_column_to_add);

                            // this is a weaker check than in the original
                            // lockfree persistence paper (it would suffice
                            // that the pivot in reduction_column_to_add)
                            // hasn't changed, but given that matrix V is
                            // stored, rather than matrix R, it's easier to
                            // check that pivot_column_index entry we read
                            // hasn't changed
                            // TODO: think through memory orders, and
                            // whether we need to adjust anything
                            entry_column_to_add =
                                pivot_column_index.value(pair);
                        } while (old_entry_column_to_add !=
                                 entry_column_to_add);

                        if (index_column_to_add < index_column_to_reduce) {
                            // pivot to the left; usual reduction
                            coefficient_t factor =
                                modulus -
                                get_coefficient(pivot) *
                                    multiplicative_inverse[get_coefficient(
                                        entry_column_to_add)] %
                                    modulus;

                            add_coboundary(
                                reduction_column_to_add, columns_to_reduce,
                                index_column_to_add, factor, dim,
                                working_reduction_column, working_coboundary);

                            pivot = get_pivot(working_coboundary);
                        } else {
                            // pivot to the right
                            retire_column(index_column_to_reduce,
                                          working_reduction_column,
                                          reduction_matrix, columns_to_reduce,
                                          dim, get_index(pivot),
                                          memory_manager);

                            if (pivot_column_index.update(
                                    pair, entry_column_to_add,
                                    make_entry(index_column_to_reduce,
                                               get_coefficient(pivot)))) {
                                return index_column_to_add;
                            } else {
                                continue;  // re-read the pair
                            }
                        }
                    } else if (get_index(e = get_zero_apparent_facet(
                                             pivot, dim + 1)) != -1) {
                        set_coefficient(e, modulus - get_coefficient(e));

                        add_simplex_coboundary(e, dim, working_reduction_column,
                                               working_coboundary);

                        pivot = get_pivot(working_coboundary);
                    } else {
                        retire_column(index_column_to_reduce,
                                      working_reduction_column,
                                      reduction_matrix, columns_to_reduce, dim,
                                      get_index(pivot), memory_manager);

                        // equivalent to CAS in the original algorithm
                        auto insertion_result = pivot_column_index.insert(
                            {get_index(get_entry(pivot)),
                             make_entry(index_column_to_reduce,
                                        get_coefficient(pivot))});
                        if (!insertion_result
                                 .second)  // failed to insert, somebody
                                           // got there before us,
                                           // continue reduction
                            continue;      // TODO: insertion_result.first is
                                           // the new pair; could elide and
                                           // extra atomic load

                        /* Pairs should be extracted if insertation was
                         * first one ! */
                        size_t location = last_diameter_index++;
                        diameters[location] = get_diameter(pivot);
                        deaths.insert({get_index(get_entry(pivot)), location});
                        break;
                    }
                } else {
                    essential_pair[idx_essential++] =
                        get_diameter(column_to_reduce);
                    // TODO: these will need special attention, if output
                    // happens after the reduction, not during
                    break;
                }
            }
            return index_column_to_reduce;
        })
            ;
        pivot_column_index.quiescent();
        deaths.quiescent();
        /* persistence pairs */
#if defined(SORT_BARCODES)
        std::vector<std::pair<value_t, value_t>> persistence_pair;
#endif
        pivot_column_index.foreach (
            [&](const typename entry_hash_map::value_type& x) {
                auto it = deaths.find(x.first);
                if (it == deaths.end())
                    return;
                value_t death = diameters[get_index(it->second)];
                value_t birth =
                    get_diameter(columns_to_reduce[get_index(x.second)]);
                if (death > birth * ratio) {
#if defined(SORT_BARCODES)
                    persistence_pair.push_back({birth, death});
#else
                    births_and_deaths_by_dim[dim].push_back(birth);
                    births_and_deaths_by_dim[dim].push_back(death);
#endif
                }
            });
#if defined(SORT_BARCODES)
        if (persistence_pair.size()) {
            std::sort(persistence_pair.begin(), persistence_pair.end(),
                      std::greater<>());
            for (auto& pair : persistence_pair) {
                births_and_deaths_by_dim[dim].push_back(pair.first);
                births_and_deaths_by_dim[dim].push_back(pair.second);
            }
        }
#endif
        /* essential pairs */
        if (idx_essential) {
            essential_pair.resize(idx_essential);
#if defined(SORT_BARCODES)
            std::sort(essential_pair.begin(), essential_pair.end(),
                      std::greater<>());
#endif
            for (size_t i = 0; i < idx_essential; ++i) {
                if (!std::isinf(essential_pair[i])) {
                    births_and_deaths_by_dim[dim].push_back(essential_pair[i]);
                    births_and_deaths_by_dim[dim].push_back(
                        std::numeric_limits<value_t>::infinity());
                }
            }
        }
    }

    std::vector<diameter_index_t> get_edges();

    void compute_barcodes()
    {
        std::vector<diameter_index_t> simplices, columns_to_reduce;

        /* pre allocate Container for each dimension */
        births_and_deaths_by_dim.resize(dim_max + 1);

        compute_dim_0_pairs(simplices, columns_to_reduce);
        /* pre allocate Container for each dimension */
        entry_hash_map pivot_column_index(columns_to_reduce.size());

        for (index_t dim = 1; dim <= dim_max; ++dim) {
            pivot_column_index.reserve(columns_to_reduce.size());

            // Only try to compute pairs if any column needs to be
            // reduced
            if (columns_to_reduce.size())
                compute_pairs(columns_to_reduce, pivot_column_index, dim);

            if (dim < dim_max)
                assemble_columns_to_reduce(simplices, columns_to_reduce,
                                           pivot_column_index, dim + 1);
        }
    }
};

template <>
value_t ripser<compressed_lower_distance_matrix>::get_vertex_birth(index_t i)
{
    // TODO: Dummy for now; nonzero vertex births are only done through
    // sparse matrices at the moment
    return 0.0;
}

template <>
value_t ripser<sparse_distance_matrix>::get_vertex_birth(index_t i)
{
    return dist.vertex_births[i];
}

template <>
class ripser<compressed_lower_distance_matrix>::simplex_coboundary_enumerator
{
    const ripser<compressed_lower_distance_matrix>& parent;
    index_t idx_below, idx_above, v, k;
    std::vector<index_t> vertices;
    diameter_entry_t simplex;
    const coefficient_t modulus;
    const compressed_lower_distance_matrix& dist;
    const binomial_coeff_table& binomial_coeff;

public:
    simplex_coboundary_enumerator(
        const diameter_entry_t _simplex, const index_t _dim,
        const ripser<compressed_lower_distance_matrix>& parent)
        : parent(parent), idx_below(get_index(_simplex)), idx_above(0),
          v(parent.n - 1), k(_dim + 1), vertices(_dim + 1), simplex(_simplex),
          modulus(parent.modulus), dist(parent.dist),
          binomial_coeff(parent.binomial_coeff)
    {
        parent.get_simplex_vertices(get_index(_simplex), _dim, parent.n,
                                    vertices.rbegin());
    }

    simplex_coboundary_enumerator(
        const ripser<compressed_lower_distance_matrix>& _parent)
        : parent(_parent), modulus(_parent.modulus), dist(_parent.dist),
          binomial_coeff(_parent.binomial_coeff)
    {
    }

    void set_simplex(const diameter_entry_t _simplex, const index_t _dim)
    {
        idx_below = get_index(_simplex);
        idx_above = 0;
        v = parent.n - 1;
        k = _dim + 1;
        simplex = _simplex;
        vertices.resize(_dim + 1);
        parent.get_simplex_vertices(get_index(_simplex), _dim, parent.n,
                                    vertices.rbegin());
    }

    bool has_next(bool all_cofacets = true)
    {
        return (v >= k && (all_cofacets || binomial_coeff(v, k) > idx_below));
    }

    diameter_entry_t next()
    {
        while ((binomial_coeff(v, k) <= idx_below)) {
            idx_below -= binomial_coeff(v, k);
            idx_above += binomial_coeff(v, k + 1);
            --v;
            --k;
            assert(k != -1);
        }
        value_t cofacet_diameter = get_diameter(simplex);
        for (index_t w : vertices)
            cofacet_diameter = std::max(cofacet_diameter, dist(v, w));
        index_t cofacet_index =
            idx_above + binomial_coeff(v--, k + 1) + idx_below;
        coefficient_t cofacet_coefficient =
            (k & 1 ? modulus - 1 : 1) * get_coefficient(simplex) % modulus;
        return diameter_entry_t(cofacet_diameter, cofacet_index,
                                cofacet_coefficient);
    }
};

template <>
class ripser<sparse_distance_matrix>::simplex_coboundary_enumerator
{
    const ripser<sparse_distance_matrix>& parent;
    index_t idx_below, idx_above, k;
    std::vector<index_t> vertices;
    diameter_entry_t simplex;
    const coefficient_t modulus;
    const sparse_distance_matrix& dist;
    const binomial_coeff_table& binomial_coeff;
    std::vector<std::vector<index_diameter_t>::const_reverse_iterator>
        neighbor_it;
    std::vector<std::vector<index_diameter_t>::const_reverse_iterator>
        neighbor_end;
    index_diameter_t neighbor;

public:
    simplex_coboundary_enumerator(const diameter_entry_t _simplex,
                                  const index_t _dim,
                                  const ripser<sparse_distance_matrix>& _parent)
        : parent(_parent), idx_below(get_index(_simplex)), idx_above(0),
          k(_dim + 1), vertices(_dim + 1), simplex(_simplex),
          modulus(parent.modulus), dist(parent.dist),
          binomial_coeff(parent.binomial_coeff)
    {
        neighbor_it.clear();
        neighbor_end.clear();

        parent.get_simplex_vertices(idx_below, _dim, parent.n,
                                    vertices.rbegin());
        for (auto v : vertices) {
            neighbor_it.push_back(dist.neighbors[v].rbegin());
            neighbor_end.push_back(dist.neighbors[v].rend());
        }
    }

    simplex_coboundary_enumerator(const ripser<sparse_distance_matrix>& _parent)
        : parent(_parent), modulus(_parent.modulus), dist(_parent.dist),
          binomial_coeff(_parent.binomial_coeff)
    {
    }

    void set_simplex(const diameter_entry_t _simplex, const index_t _dim)
    {
        idx_below = get_index(_simplex);
        idx_above = 0;
        k = _dim + 1;
        simplex = _simplex;
        vertices.resize(_dim + 1);
        parent.get_simplex_vertices(idx_below, _dim, parent.n,
                                    vertices.rbegin());

        neighbor_it.resize(_dim + 1);
        neighbor_end.resize(_dim + 1);
        for (index_t i = 0; i <= _dim; ++i) {
            auto v = vertices[i];
            neighbor_it[i] = dist.neighbors[v].rbegin();
            neighbor_end[i] = dist.neighbors[v].rend();
        }
    }

    bool has_next(bool all_cofacets = true)
    {
        for (auto &it0 = neighbor_it[0], &end0 = neighbor_end[0]; it0 != end0;
             ++it0) {
            neighbor = *it0;
            for (size_t idx = 1; idx < neighbor_it.size(); ++idx) {
                auto &it = neighbor_it[idx], end = neighbor_end[idx];
                while (get_index(*it) > get_index(neighbor))
                    if (++it == end)
                        return false;
                if (get_index(*it) != get_index(neighbor))
                    goto continue_outer;
                else
                    neighbor = std::max(neighbor, *it);
            }
            while (k > 0 && vertices[k - 1] > get_index(neighbor)) {
                if (!all_cofacets)
                    return false;
                idx_below -= binomial_coeff(vertices[k - 1], k);
                idx_above += binomial_coeff(vertices[k - 1], k + 1);
                --k;
            }
            return true;
        continue_outer:;
        }
        return false;
    }

    diameter_entry_t next()
    {
        ++neighbor_it[0];
        value_t cofacet_diameter =
            std::max(get_diameter(simplex), get_diameter(neighbor));
        index_t cofacet_index =
            idx_above + binomial_coeff(get_index(neighbor), k + 1) + idx_below;
        coefficient_t cofacet_coefficient =
            (k & 1 ? modulus - 1 : 1) * get_coefficient(simplex) % modulus;
        return diameter_entry_t(cofacet_diameter, cofacet_index,
                                cofacet_coefficient);
    }
};

template <>
std::vector<diameter_index_t>
ripser<compressed_lower_distance_matrix>::get_edges()
{
    std::vector<index_t> vertices(2);
    std::vector<diameter_index_t> edges;
    edges.reserve(n);
    for (index_t index = binomial_coeff(n, 2); index-- > 0;) {
        get_simplex_vertices(index, 1, dist.size(), vertices.rbegin());
        value_t length = dist(vertices[0], vertices[1]);
        if (length <= threshold)
            edges.push_back({length, index});
    }
    return edges;
}

template <>
std::vector<diameter_index_t> ripser<sparse_distance_matrix>::get_edges()
{
    std::vector<diameter_index_t> edges;
    edges.reserve(n);
    for (index_t i = 0; i < n; ++i)
        for (auto n : dist.neighbors[i]) {
            index_t j = get_index(n);
            if (i > j)
                edges.push_back({get_diameter(n), get_edge_index(i, j)});
        }
    return edges;
}
