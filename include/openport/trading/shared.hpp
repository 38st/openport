#pragma once

#include <algorithm>
#include <atomic>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace openport::trading {

/// Containers for account history whose copies share storage. A transaction
/// works on a copy of the whole account and publishes another, so copying must
/// not cost the length of the history: a copy shares the chunks, and a write
/// copies only the chunk it changes (and the chunk index, once per copy).
///
/// Reads use the usual const interface, so reading never copies. Writes are
/// explicit: `mut()`, `push_back()` and a map's `operator[]`. A reference from a
/// write stays valid until the container is next copied, or copied from: after
/// that, the chunk it points into may belong to the copy. Do not write through a
/// reference held across a copy of the container (or of a struct holding it).
namespace shared_detail {
template <class Node>
bool unique(const std::shared_ptr<Node>& node) {
  if (node.use_count() != 1) return false;
  // Pairs with the release in another owner's last decrement, so its reads of
  // the chunk happen before this owner's writes.
  std::atomic_thread_fence(std::memory_order_acquire);
  return true;
}
}  // namespace shared_detail

template <class T, std::size_t Chunk = 32>
class SharedVector {
  using Block = std::vector<T>;
  struct Spine {
    std::vector<std::shared_ptr<Block>> blocks;
    std::size_t size = 0;
  };

 public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using const_reference = const T&;
  using reference = const T&;

  /// Reads through its container, so a write elsewhere never leaves it stale.
  class const_iterator {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;
    const_iterator() = default;
    const_iterator(const SharedVector* owner, std::size_t index) : owner_(owner), index_(index) {}
    reference operator*() const { return (*owner_)[index_]; }
    pointer operator->() const { return &(*owner_)[index_]; }
    reference operator[](difference_type n) const { return (*owner_)[static_cast<std::size_t>(static_cast<difference_type>(index_) + n)]; }
    const_iterator& operator++() { ++index_; return *this; }
    const_iterator operator++(int) { auto copy = *this; ++index_; return copy; }
    const_iterator& operator--() { --index_; return *this; }
    const_iterator operator--(int) { auto copy = *this; --index_; return copy; }
    const_iterator& operator+=(difference_type n) { index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + n); return *this; }
    const_iterator& operator-=(difference_type n) { return *this += -n; }
    friend const_iterator operator+(const_iterator it, difference_type n) { return it += n; }
    friend const_iterator operator+(difference_type n, const_iterator it) { return it += n; }
    friend const_iterator operator-(const_iterator it, difference_type n) { return it -= n; }
    friend difference_type operator-(const const_iterator& a, const const_iterator& b) {
      return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
    }
    friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.index_ == b.index_; }
    friend auto operator<=>(const const_iterator& a, const const_iterator& b) { return a.index_ <=> b.index_; }
    [[nodiscard]] std::size_t index() const { return index_; }

   private:
    const SharedVector* owner_ = nullptr;
    std::size_t index_ = 0;
  };
  using iterator = const_iterator;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  SharedVector() = default;
  SharedVector(std::initializer_list<T> items) { for (const auto& item : items) push_back(item); }

  [[nodiscard]] std::size_t size() const { return spine_ ? spine_->size : 0; }
  [[nodiscard]] bool empty() const { return size() == 0; }
  const T& operator[](std::size_t i) const { return (*spine_->blocks[i / Chunk])[i % Chunk]; }
  const T& at(std::size_t i) const {
    if (i >= size()) throw std::out_of_range("SharedVector::at");
    return (*this)[i];
  }
  const T& front() const { return (*this)[0]; }
  const T& back() const { return (*this)[size() - 1]; }
  const_iterator begin() const { return {this, 0}; }
  const_iterator end() const { return {this, size()}; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }

  /// The element at `i`, for writing.
  T& mut(std::size_t i) {
    if (i >= size()) throw std::out_of_range("SharedVector::mut");
    auto& spine = own();
    return own_block(spine, i / Chunk)[i % Chunk];
  }
  T& mut_back() { return mut(size() - 1); }
  void push_back(T value) {
    auto& spine = own();
    if (spine.size % Chunk == 0) {
      auto block = std::make_shared<Block>();
      block->reserve(Chunk);
      spine.blocks.push_back(std::move(block));
    }
    own_block(spine, spine.blocks.size() - 1).push_back(std::move(value));
    ++spine.size;
  }
  void clear() { spine_.reset(); }
  /// Keeps the first `count` elements.
  void truncate(std::size_t count) {
    if (count >= size()) return;
    if (count == 0) { clear(); return; }
    auto& spine = own();
    spine.blocks.resize((count + Chunk - 1) / Chunk);
    auto& last = own_block(spine, spine.blocks.size() - 1);
    last.erase(last.begin() + static_cast<std::ptrdiff_t>(count - (spine.blocks.size() - 1) * Chunk), last.end());
    spine.size = count;
  }
  [[nodiscard]] std::vector<T> to_vector() const { return {begin(), end()}; }
  /// Whether the two share every element's storage (and so hold equal values).
  [[nodiscard]] bool same(const SharedVector& other) const { return spine_ == other.spine_; }
  /// Whether element `i` is stored in the same chunk as `other`'s element `i`.
  [[nodiscard]] bool same_chunk(const SharedVector& other, std::size_t i) const {
    return spine_ && other.spine_ && i / Chunk < spine_->blocks.size() && i / Chunk < other.spine_->blocks.size() &&
           spine_->blocks[i / Chunk] == other.spine_->blocks[i / Chunk];
  }

  friend bool operator==(const SharedVector& a, const SharedVector& b) {
    if (a.spine_ == b.spine_) return true;
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (i % Chunk == 0 && a.same_chunk(b, i)) { i += Chunk - 1; continue; }
      if (!(a[i] == b[i])) return false;
    }
    return true;
  }

 private:
  Spine& own() {
    if (!spine_) spine_ = std::make_shared<Spine>();
    else if (!shared_detail::unique(spine_)) spine_ = std::make_shared<Spine>(*spine_);
    return *spine_;
  }
  static Block& own_block(Spine& spine, std::size_t index) {
    auto& block = spine.blocks[index];
    if (!shared_detail::unique(block)) {
      auto copy = std::make_shared<Block>();
      copy->reserve(Chunk);
      copy->insert(copy->end(), block->begin(), block->end());
      block = std::move(copy);
    }
    return *block;
  }
  std::shared_ptr<Spine> spine_;
};

/// A sorted map with SharedVector's sharing: entries sit in sorted chunks.
template <class K, class V, std::size_t Leaf = 32>
class SharedMap {
  using Entry = std::pair<K, V>;
  using Block = std::vector<Entry>;
  struct Spine {
    std::vector<std::shared_ptr<Block>> blocks;
    std::size_t size = 0;
  };

 public:
  using key_type = K;
  using mapped_type = V;
  using value_type = Entry;
  using size_type = std::size_t;

  class const_iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Entry;
    using difference_type = std::ptrdiff_t;
    using pointer = const Entry*;
    using reference = const Entry&;
    const_iterator() = default;
    const_iterator(const SharedMap* owner, std::size_t block, std::size_t offset) : owner_(owner), block_(block), offset_(offset) {}
    reference operator*() const { return (*owner_->spine_->blocks[block_])[offset_]; }
    pointer operator->() const { return &**this; }
    const_iterator& operator++() {
      if (++offset_ == owner_->spine_->blocks[block_]->size()) { ++block_; offset_ = 0; }
      return *this;
    }
    const_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
    const_iterator& operator--() {
      if (offset_ == 0) { --block_; offset_ = owner_->spine_->blocks[block_]->size() - 1; }
      else --offset_;
      return *this;
    }
    const_iterator operator--(int) { auto copy = *this; --*this; return copy; }
    friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.block_ == b.block_ && a.offset_ == b.offset_; }

   private:
    friend class SharedMap;
    const SharedMap* owner_ = nullptr;
    std::size_t block_ = 0;
    std::size_t offset_ = 0;
  };
  using iterator = const_iterator;

  SharedMap() = default;
  SharedMap(std::initializer_list<Entry> items) { for (const auto& [key, value] : items) (*this)[key] = value; }

  [[nodiscard]] std::size_t size() const { return spine_ ? spine_->size : 0; }
  [[nodiscard]] bool empty() const { return size() == 0; }
  const_iterator begin() const { return {this, 0, 0}; }
  const_iterator end() const { return {this, spine_ ? spine_->blocks.size() : 0, 0}; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  const_iterator find(const K& key) const {
    const auto [block, offset, found] = locate(key);
    return found ? const_iterator{this, block, offset} : end();
  }
  [[nodiscard]] bool contains(const K& key) const { return std::get<2>(locate(key)); }
  [[nodiscard]] std::size_t count(const K& key) const { return contains(key) ? 1 : 0; }
  const V& at(const K& key) const {
    const auto it = find(key);
    if (it == end()) throw std::out_of_range("SharedMap::at");
    return it->second;
  }

  /// The value for `key`, default-constructed if absent, for writing.
  V& operator[](const K& key) {
    auto [block, offset, found] = locate(key);
    auto& spine = own();
    if (!found) return insert_at(spine, block, offset, key, V{});
    return own_block(spine, block)[offset].second;
  }
  /// The existing value for `key`, for writing.
  V& mut(const K& key) {
    const auto [block, offset, found] = locate(key);
    if (!found) throw std::out_of_range("SharedMap::mut");
    return own_block(own(), block)[offset].second;
  }
  /// Inserts when absent; true if it did.
  bool emplace(const K& key, V value) {
    auto [block, offset, found] = locate(key);
    if (found) return false;
    insert_at(own(), block, offset, key, std::move(value));
    return true;
  }
  std::size_t erase(const K& key) {
    const auto [block, offset, found] = locate(key);
    if (!found) return 0;
    erase_at(block, offset);
    return 1;
  }
  /// Erases the entry at `it`; the entry after it.
  const_iterator erase(const_iterator it) {
    const auto block = it.block_, offset = it.offset_;
    erase_at(block, offset);
    if (!spine_ || block >= spine_->blocks.size()) return end();
    if (offset >= spine_->blocks[block]->size()) return {this, block + 1, 0};
    return {this, block, offset};
  }
  void clear() { spine_.reset(); }
  /// Whether the two share every entry's storage (and so hold equal values).
  [[nodiscard]] bool same(const SharedMap& other) const { return spine_ == other.spine_; }
  /// Walks two maps' differences in key order, skipping the chunks they share:
  /// `added(key, value)` for a key only `after` has, `removed(key)` for one only
  /// `before` has, and `both(key, before_value, after_value)` for a key in both
  /// that is not stored in a shared chunk (its values may still be equal).
  template <class Added, class Removed, class Both>
  static void compare(const SharedMap& before, const SharedMap& after, Added&& added, Removed&& removed, Both&& both) {
    static const std::vector<std::shared_ptr<Block>> none;
    const auto& b = before.spine_ ? before.spine_->blocks : none;
    const auto& a = after.spine_ ? after.spine_->blocks : none;
    std::size_t i = 0, io = 0, j = 0, jo = 0;
    const auto step = [](const std::vector<std::shared_ptr<Block>>& blocks, std::size_t& block, std::size_t& offset) {
      if (++offset == blocks[block]->size()) { ++block; offset = 0; }
    };
    while (i < b.size() || j < a.size()) {
      if (i < b.size() && j < a.size() && io == 0 && jo == 0 && b[i] == a[j]) { ++i; ++j; continue; }
      if (j == a.size() || (i < b.size() && (*b[i])[io].first < (*a[j])[jo].first)) {
        removed((*b[i])[io].first);
        step(b, i, io);
      } else if (i == b.size() || (*a[j])[jo].first < (*b[i])[io].first) {
        added((*a[j])[jo].first, (*a[j])[jo].second);
        step(a, j, jo);
      } else {
        both((*a[j])[jo].first, (*b[i])[io].second, (*a[j])[jo].second);
        step(b, i, io);
        step(a, j, jo);
      }
    }
  }

  friend bool operator==(const SharedMap& a, const SharedMap& b) {
    if (a.spine_ == b.spine_) return true;
    if (a.size() != b.size()) return false;
    auto x = a.begin();
    for (auto y = b.begin(); y != b.end(); ++x, ++y)
      if (!(x->first == y->first) || !(x->second == y->second)) return false;
    return true;
  }

 private:
  /// The block and offset where `key` is or would go, and whether it is there.
  std::tuple<std::size_t, std::size_t, bool> locate(const K& key) const {
    if (!spine_ || spine_->blocks.empty()) return {0, 0, false};
    const auto& blocks = spine_->blocks;
    // The last block whose first key is not after `key`.
    auto it = std::upper_bound(blocks.begin(), blocks.end(), key,
        [](const K& k, const std::shared_ptr<Block>& block) { return k < block->front().first; });
    const std::size_t index = it == blocks.begin() ? 0 : static_cast<std::size_t>(it - blocks.begin()) - 1;
    const auto& entries = *blocks[index];
    const auto at = std::lower_bound(entries.begin(), entries.end(), key,
        [](const Entry& entry, const K& k) { return entry.first < k; });
    const auto offset = static_cast<std::size_t>(at - entries.begin());
    return {index, offset, at != entries.end() && !(key < at->first)};
  }
  V& insert_at(Spine& spine, std::size_t block, std::size_t offset, const K& key, V value) {
    if (spine.blocks.empty()) {
      spine.blocks.push_back(std::make_shared<Block>());
      block = 0;
      offset = 0;
    }
    auto& entries = own_block(spine, block);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(offset), Entry{key, std::move(value)});
    ++spine.size;
    if (entries.size() <= 2 * Leaf) return entries[offset].second;
    // Split a full block in two; the key lands in one of them.
    auto upper = std::make_shared<Block>(std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(Leaf)),
                                         std::make_move_iterator(entries.end()));
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(Leaf), entries.end());
    spine.blocks.insert(spine.blocks.begin() + static_cast<std::ptrdiff_t>(block + 1), std::move(upper));
    return offset < Leaf ? (*spine.blocks[block])[offset].second : (*spine.blocks[block + 1])[offset - Leaf].second;
  }
  void erase_at(std::size_t block, std::size_t offset) {
    auto& spine = own();
    auto& entries = own_block(spine, block);
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(offset));
    --spine.size;
    if (entries.empty()) spine.blocks.erase(spine.blocks.begin() + static_cast<std::ptrdiff_t>(block));
  }
  Spine& own() {
    if (!spine_) spine_ = std::make_shared<Spine>();
    else if (!shared_detail::unique(spine_)) spine_ = std::make_shared<Spine>(*spine_);
    return *spine_;
  }
  static Block& own_block(Spine& spine, std::size_t index) {
    auto& block = spine.blocks[index];
    if (!shared_detail::unique(block)) block = std::make_shared<Block>(*block);
    return *block;
  }
  std::shared_ptr<Spine> spine_;
};

/// A sorted set with SharedMap's sharing.
template <class K>
class SharedSet {
  struct Present {
    bool operator==(const Present&) const = default;
  };
  using Map = SharedMap<K, Present>;

 public:
  using value_type = K;
  class const_iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = K;
    using difference_type = std::ptrdiff_t;
    using pointer = const K*;
    using reference = const K&;
    const_iterator() = default;
    explicit const_iterator(typename Map::const_iterator it) : it_(it) {}
    reference operator*() const { return it_->first; }
    pointer operator->() const { return &it_->first; }
    const_iterator& operator++() { ++it_; return *this; }
    const_iterator operator++(int) { auto copy = *this; ++it_; return copy; }
    const_iterator& operator--() { --it_; return *this; }
    const_iterator operator--(int) { auto copy = *this; --it_; return copy; }
    friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.it_ == b.it_; }

   private:
    typename Map::const_iterator it_;
  };
  using iterator = const_iterator;

  SharedSet() = default;
  SharedSet(std::initializer_list<K> items) { for (const auto& item : items) insert(item); }
  [[nodiscard]] std::size_t size() const { return map_.size(); }
  [[nodiscard]] bool empty() const { return map_.empty(); }
  [[nodiscard]] bool contains(const K& key) const { return map_.contains(key); }
  [[nodiscard]] std::size_t count(const K& key) const { return map_.count(key); }
  bool insert(const K& key) { return map_.emplace(key, Present{}); }
  std::size_t erase(const K& key) { return map_.erase(key); }
  void clear() { map_.clear(); }
  const_iterator begin() const { return const_iterator(map_.begin()); }
  const_iterator end() const { return const_iterator(map_.end()); }
  /// Whether the two share every key's storage (and so hold the same keys).
  [[nodiscard]] bool same(const SharedSet& other) const { return map_.same(other.map_); }
  friend bool operator==(const SharedSet& a, const SharedSet& b) { return a.map_ == b.map_; }

 private:
  Map map_;
};

// JSON as the standard containers write it: an array, an object by key, an array.
template <class Json, class T, std::size_t N>
void to_json(Json& j, const SharedVector<T, N>& items) {
  j = Json::array();
  for (const auto& item : items) j.push_back(item);
}
template <class Json, class T, std::size_t N>
void from_json(const Json& j, SharedVector<T, N>& items) {
  if (!j.is_array()) throw std::invalid_argument("Expected an array");
  items.clear();
  for (const auto& item : j) items.push_back(item.template get<T>());
}
template <class Json, class V, std::size_t N>
void to_json(Json& j, const SharedMap<std::string, V, N>& items) {
  j = Json::object();
  for (const auto& [key, value] : items) j[key] = value;
}
template <class Json, class V, std::size_t N>
void from_json(const Json& j, SharedMap<std::string, V, N>& items) {
  if (!j.is_object()) throw std::invalid_argument("Expected an object");
  items.clear();
  for (auto it = j.begin(); it != j.end(); ++it) items.emplace(it.key(), it.value().template get<V>());
}
template <class Json, class K>
void to_json(Json& j, const SharedSet<K>& items) {
  j = Json::array();
  for (const auto& item : items) j.push_back(item);
}
template <class Json, class K>
void from_json(const Json& j, SharedSet<K>& items) {
  if (!j.is_array()) throw std::invalid_argument("Expected an array");
  items.clear();
  for (const auto& item : j) items.insert(item.template get<K>());
}

}  // namespace openport::trading
