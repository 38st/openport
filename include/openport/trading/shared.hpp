#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace openport::trading {

/// Containers for account history whose copies share storage. A transaction
/// works on a copy of the whole account and publishes another, so copying must
/// not cost the length of the history. Vectors use a 32-way radix tree over
/// chunks; maps use a balanced B+ tree with up to Leaf entries or children per
/// node. Copies share a root. Writes copy only a root-to-leaf path, plus a
/// neighbour when an erase rebalances a map: O(log n) pointers and one chunk.
/// Comparisons skip shared subtrees. Tree height also bounds destruction's stack;
/// dropping unshared entries still takes time proportional to those entries.
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
  static_assert(Chunk > 0);
  struct Node {
    std::vector<T> entries;
  };
  struct Branch : Node {
    std::array<std::shared_ptr<Node>, 32> children;
    std::size_t count = 0;
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
  SharedVector(const SharedVector&) = default;
  SharedVector& operator=(const SharedVector&) = default;
  SharedVector(SharedVector&& other) noexcept
      : root_(std::move(other.root_)), size_(std::exchange(other.size_, 0)), shift_(std::exchange(other.shift_, 0)) {}
  SharedVector& operator=(SharedVector&& other) noexcept {
    if (this != &other) {
      root_ = std::move(other.root_);
      size_ = std::exchange(other.size_, 0);
      shift_ = std::exchange(other.shift_, 0);
    }
    return *this;
  }
  SharedVector(std::initializer_list<T> items) { for (const auto& item : items) push_back(item); }

  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size() == 0; }
  const T& operator[](std::size_t i) const { return block(i)->entries[i % Chunk]; }
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
    return own_leaf(root_, shift_, i / Chunk).entries[i % Chunk];
  }
  T& mut_back() { return mut(size() - 1); }
  void push_back(T value) {
    const auto index = size_ / Chunk;
    if (root_ && (index >> shift_) != 0) {
      auto parent = std::make_shared<Branch>();
      parent->children[0] = root_;
      parent->count = 1;
      root_ = std::move(parent);
      shift_ += 5;
    }
    own_leaf(root_, shift_, index).entries.push_back(std::move(value));
    ++size_;
  }
  void clear() { root_.reset(); size_ = 0; shift_ = 0; }
  /// Keeps the first `count` elements.
  void truncate(std::size_t count) {
    if (count >= size()) return;
    if (count == 0) { clear(); return; }
    trim(root_, shift_, count);
    size_ = count;
    while (shift_ != 0 && static_cast<const Branch&>(*root_).count == 1) {
      auto child = static_cast<const Branch&>(*root_).children[0];
      root_ = std::move(child);
      shift_ -= 5;
    }
  }
  [[nodiscard]] std::vector<T> to_vector() const { return {begin(), end()}; }
  /// Whether the two share every element's storage (and so hold equal values).
  [[nodiscard]] bool same(const SharedVector& other) const { return root_ == other.root_; }
  /// Whether element `i` is stored in the same chunk as `other`'s element `i`.
  [[nodiscard]] bool same_chunk(const SharedVector& other, std::size_t i) const {
    return i < size_ && i < other.size_ && block(i) == other.block(i);
  }
  /// Visits common indices in unshared chunks; values may still be equal.
  /// Appended and removed elements are described by the two sizes.
  template <class Both>
  static void compare(const SharedVector& before, const SharedVector& after, Both&& both) {
    const auto common = std::min(before.size(), after.size());
    if (common != 0) compare_nodes(before.root_.get(), before.shift_, after.root_.get(), after.shift_, 0, common, both);
  }

  friend bool operator==(const SharedVector& a, const SharedVector& b) {
    if (a.size() != b.size()) return false;
    bool equal = true;
    compare(a, b, [&](std::size_t, const T& old, const T& now) { if (!(old == now)) equal = false; });
    return equal;
  }

 private:
  const Node* block(std::size_t i) const {
    auto* node = root_.get();
    const auto index = i / Chunk;
    // Unroll the usual depths so indexed reads do not branch at each level.
    switch (shift_) {
      default:
        for (auto shift = shift_; shift > 15; shift -= 5)
          node = static_cast<const Branch*>(node)->children[(index >> (shift - 5)) & 31].get();
        [[fallthrough]];
      case 15: node = static_cast<const Branch*>(node)->children[(index >> 10) & 31].get(); [[fallthrough]];
      case 10: node = static_cast<const Branch*>(node)->children[(index >> 5) & 31].get(); [[fallthrough]];
      case 5: node = static_cast<const Branch*>(node)->children[index & 31].get(); [[fallthrough]];
      case 0: return node;
    }
  }
  static Node& own(std::shared_ptr<Node>& node, std::size_t shift) {
    if (!node) {
      if (shift == 0) node = std::make_shared<Node>();
      else node = std::make_shared<Branch>();
    } else if (!shared_detail::unique(node)) {
      if (shift == 0) node = std::make_shared<Node>(*node);
      else node = std::make_shared<Branch>(static_cast<const Branch&>(*node));
    }
    return *node;
  }
  static Node& own_leaf(std::shared_ptr<Node>& node, std::size_t shift, std::size_t index) {
    auto& current = own(node, shift);
    if (shift == 0) {
      current.entries.reserve(Chunk);
      return current;
    }
    const auto child = (index >> (shift - 5)) & 31;
    auto& branch = static_cast<Branch&>(current);
    if (child == branch.count) ++branch.count;
    return own_leaf(branch.children[child], shift - 5, index);
  }
  static void trim(std::shared_ptr<Node>& node, std::size_t shift, std::size_t count) {
    auto& current = own(node, shift);
    if (shift == 0) {
      current.entries.erase(current.entries.begin() + static_cast<std::ptrdiff_t>(count), current.entries.end());
      return;
    }
    const auto span = Chunk * (std::size_t{1} << (shift - 5));
    const auto last = (count - 1) / span;
    auto& branch = static_cast<Branch&>(current);
    for (std::size_t i = last + 1; i < branch.count; ++i) branch.children[i].reset();
    branch.count = last + 1;
    const auto kept = count - last * span;
    if (kept < span) trim(branch.children[last], shift - 5, kept);
  }
  template <class Both>
  static void compare_nodes(const Node* before, std::size_t before_shift, const Node* after,
                            std::size_t after_shift, std::size_t base, std::size_t count, Both& both) {
    if (before == after) return;
    const auto shift = std::max(before_shift, after_shift);
    if (shift == 0) {
      for (std::size_t i = 0; i < count; ++i) both(base + i, before->entries[i], after->entries[i]);
      return;
    }
    const auto span = Chunk * (std::size_t{1} << (shift - 5));
    for (std::size_t offset = 0, child = 0; offset < count; offset += span, ++child) {
      const auto* old = before_shift == shift ? static_cast<const Branch*>(before)->children[child].get() : before;
      const auto* now = after_shift == shift ? static_cast<const Branch*>(after)->children[child].get() : after;
      compare_nodes(old, std::min(before_shift, shift - 5), now, std::min(after_shift, shift - 5),
                    base + offset, std::min(span, count - offset), both);
    }
  }
  std::shared_ptr<Node> root_;
  std::size_t size_ = 0;
  std::size_t shift_ = 0;
};

/// A sorted B+ tree with SharedVector's sharing. Leaves and branches are bounded
/// by Leaf (at least four children); non-root nodes stay at least half full.
template <class K, class V, std::size_t Leaf = 32>
class SharedMap {
  static_assert(Leaf >= 2);
  using Entry = std::pair<K, V>;
  struct Node;
  struct Child {
    K first;
    std::shared_ptr<Node> node;
    std::size_t end = 0;
  };
  struct Node {
    std::vector<Entry> entries;
    std::vector<Child> children;
    std::size_t size = 0;
    std::size_t height = 0;
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
    const_iterator(const SharedMap* owner, std::size_t index, const Entry* found = nullptr)
        : owner_(owner), index_(index), found_(found), revision_(owner->revision_) {}
    reference operator*() const {
      return found_ && revision_ == owner_->revision_ ? *found_ : owner_->entry(index_);
    }
    pointer operator->() const { return &**this; }
    const_iterator& operator++() { ++index_; found_ = nullptr; return *this; }
    const_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
    const_iterator& operator--() { --index_; found_ = nullptr; return *this; }
    const_iterator operator--(int) { auto copy = *this; --*this; return copy; }
    friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.index_ == b.index_; }

   private:
    friend class SharedMap;
    const SharedMap* owner_ = nullptr;
    std::size_t index_ = 0;
    const Entry* found_ = nullptr;
    std::size_t revision_ = 0;
  };
  using iterator = const_iterator;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  SharedMap() = default;
  SharedMap(const SharedMap& other) : root_(other.root_) {}
  SharedMap(SharedMap&& other) noexcept : root_(std::move(other.root_)) { ++other.revision_; }
  SharedMap& operator=(const SharedMap& other) {
    root_ = other.root_;
    ++revision_;
    return *this;
  }
  SharedMap& operator=(SharedMap&& other) noexcept {
    if (this != &other) {
      root_ = std::move(other.root_);
      ++revision_;
      ++other.revision_;
    }
    return *this;
  }
  SharedMap(std::initializer_list<Entry> items) { for (const auto& [key, value] : items) (*this)[key] = value; }

  [[nodiscard]] std::size_t size() const { return root_ ? root_->size : 0; }
  [[nodiscard]] bool empty() const { return size() == 0; }
  const_iterator begin() const { return {this, 0}; }
  const_iterator end() const { return {this, size()}; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }

  const_iterator find(const K& key) const {
    const auto [index, found] = locate(key);
    return found ? const_iterator{this, index, found} : end();
  }
  [[nodiscard]] bool contains(const K& key) const { return locate(key).second != nullptr; }
  [[nodiscard]] std::size_t count(const K& key) const { return contains(key) ? 1 : 0; }
  const V& at(const K& key) const {
    const auto found = locate(key).second;
    if (!found) throw std::out_of_range("SharedMap::at");
    return found->second;
  }

  /// The value for `key`, default-constructed if absent, for writing.
  V& operator[](const K& key) {
    const auto [index, found] = locate(key);
    ++revision_;
    if (found) return own_entry(root_, index).second;
    return insert_at(index, key, V{});
  }
  /// The existing value for `key`, for writing.
  V& mut(const K& key) {
    const auto [index, found] = locate(key);
    if (!found) throw std::out_of_range("SharedMap::mut");
    ++revision_;
    return own_entry(root_, index).second;
  }
  /// Inserts when absent; true if it did.
  bool emplace(const K& key, V value) {
    const auto [index, found] = locate(key);
    if (found) return false;
    ++revision_;
    insert_at(index, key, std::move(value));
    return true;
  }
  std::size_t erase(const K& key) {
    const auto [index, found] = locate(key);
    if (!found) return 0;
    erase_at(index);
    return 1;
  }
  /// Erases the entry at `it`; the entry after it.
  const_iterator erase(const_iterator it) {
    erase_at(it.index_);
    return {this, it.index_};
  }
  void clear() { ++revision_; root_.reset(); }
  /// Whether the two share every entry's storage (and so hold equal values).
  [[nodiscard]] bool same(const SharedMap& other) const { return root_ == other.root_; }
  /// Walks two maps' differences in key order, skipping shared subtrees:
  /// `added(key, value)` for a key only `after` has, `removed(key)` for one only
  /// `before` has, and `both(key, before_value, after_value)` for a key in both
  /// that is not stored in a shared chunk (its values may still be equal).
  template <class Added, class Removed, class Both>
  static void compare(const SharedMap& before, const SharedMap& after, Added&& added, Removed&& removed, Both&& both) {
    if (before.same(after)) return;
    Cursor old(before.root_.get()), now(after.root_.get());
    while (!old.empty() || !now.empty()) {
      if (old.shared(now)) { old.skip(); now.skip(); continue; }
      if (!old.empty() && old.height() != 0 && (now.empty() || old.height() > now.height())) { old.descend(); continue; }
      if (!now.empty() && now.height() != 0 && (old.empty() || now.height() > old.height())) { now.descend(); continue; }
      if (!old.empty() && old.height() != 0) { old.descend(); now.descend(); continue; }
      if (now.empty() || (!old.empty() && old.value().first < now.value().first)) {
        removed(old.value().first);
        old.step();
      } else if (old.empty() || now.value().first < old.value().first) {
        added(now.value().first, now.value().second);
        now.step();
      } else {
        both(now.value().first, old.value().second, now.value().second);
        old.step();
        now.step();
      }
    }
  }
  /// Common positions, for the set's array encoding. A key insertion can shift
  /// the following positions, so only subtrees at the same position are skipped.
  template <class Both, class Added>
  static void compare_positions(const SharedMap& before, const SharedMap& after, Both&& both, Added&& added) {
    if (before.same(after)) return;
    Cursor old(before.root_.get()), now(after.root_.get());
    std::size_t index = 0;
    while (!old.empty() && !now.empty()) {
      if (old.shared(now)) {
        index += old.remaining();
        old.skip(); now.skip();
      } else if (old.height() > now.height()) old.descend();
      else if (now.height() > old.height()) now.descend();
      else if (old.height() != 0) { old.descend(); now.descend(); }
      else {
        both(index++, old.value().first, now.value().first);
        old.step(); now.step();
      }
    }
    while (!now.empty()) {
      if (now.height() != 0) now.descend();
      else { added(index++, now.value().first); now.step(); }
    }
  }

  friend bool operator==(const SharedMap& a, const SharedMap& b) {
    if (a.size() != b.size()) return false;
    bool equal = true;
    compare(a, b, [&](const K&, const V&) { equal = false; }, [&](const K&) { equal = false; },
        [&](const K&, const V& old, const V& now) { if (!(old == now)) equal = false; });
    return equal;
  }

 private:
  // A frontier of subtrees in key order. Raw pointers borrow the two roots for
  // the comparison; traversing or skipping it never increments a reference count.
  class Cursor {
   public:
    explicit Cursor(const Node* root) { if (root) pending_.push_back(root); }
    bool empty() const { return pending_.empty(); }
    std::size_t height() const { return pending_.back()->height; }
    std::size_t remaining() const { return pending_.back()->size - offset_; }
    const Entry& value() const { return pending_.back()->entries[offset_]; }
    bool shared(const Cursor& other) const {
      return !empty() && !other.empty() && pending_.back() == other.pending_.back() && offset_ == other.offset_;
    }
    void skip() { pending_.pop_back(); offset_ = 0; }
    void step() { if (++offset_ == pending_.back()->size) skip(); }
    void descend() {
      // An empty opposite cursor still needs leaves delivered to the caller.
      if (height() == 0) return;
      const auto* node = pending_.back();
      skip();
      for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) pending_.push_back(it->node.get());
    }
   private:
    std::vector<const Node*> pending_;
    std::size_t offset_ = 0;
  };
  static const K& first(const Node& node) {
    return node.height == 0 ? node.entries.front().first : node.children.front().first;
  }
  static void refresh(Node& node) {
    if (node.height == 0) { node.size = node.entries.size(); return; }
    node.size = 0;
    for (auto& child : node.children) {
      child.first = first(*child.node);
      node.size += child.node->size;
      child.end = node.size;
    }
  }
  static std::size_t child_at(const Node& node, std::size_t index) {
    const auto it = std::upper_bound(node.children.begin(), node.children.end(), index,
        [](std::size_t position, const Child& child) { return position < child.end; });
    return it == node.children.end() ? node.children.size() - 1 : static_cast<std::size_t>(it - node.children.begin());
  }
  const Entry& entry(std::size_t index) const {
    const auto* node = root_.get();
    while (node->height != 0) {
      const auto child = child_at(*node, index);
      if (child != 0) index -= node->children[child - 1].end;
      node = node->children[child].node.get();
    }
    return node->entries[index];
  }
  /// The insertion position and the entry if `key` is present.
  std::pair<std::size_t, const Entry*> locate(const K& key) const {
    if (!root_) return {0, nullptr};
    const auto* node = root_.get();
    std::size_t index = 0;
    while (node->height != 0) {
      auto it = std::upper_bound(node->children.begin(), node->children.end(), key,
          [](const K& sought, const Child& child) { return sought < child.first; });
      if (it != node->children.begin()) --it;
      if (it != node->children.begin()) index += (it - 1)->end;
      node = it->node.get();
    }
    const auto it = std::lower_bound(node->entries.begin(), node->entries.end(), key,
        [](const Entry& item, const K& sought) { return item.first < sought; });
    index += static_cast<std::size_t>(it - node->entries.begin());
    return {index, it != node->entries.end() && !(key < it->first) ? &*it : nullptr};
  }
  static Node& own(std::shared_ptr<Node>& node) {
    if (!node) node = std::make_shared<Node>();
    else if (!shared_detail::unique(node)) node = std::make_shared<Node>(*node);
    return *node;
  }
  static Entry& own_entry(std::shared_ptr<Node>& node, std::size_t index) {
    auto& current = own(node);
    if (current.height == 0) return current.entries[index];
    const auto child = child_at(current, index);
    if (child != 0) index -= current.children[child - 1].end;
    return own_entry(current.children[child].node, index);
  }
  struct Split {
    std::shared_ptr<Node> right;
    V* value;
  };
  V& insert_at(std::size_t index, const K& key, V value) {
    const auto result = insert(root_, index, key, std::move(value));
    if (result.right) {
      auto parent = std::make_shared<Node>();
      parent->height = root_->height + 1;
      parent->children.push_back({first(*root_), root_, 0});
      parent->children.push_back({first(*result.right), result.right, 0});
      refresh(*parent);
      root_ = std::move(parent);
    }
    return *result.value;
  }
  static Split insert(std::shared_ptr<Node>& node, std::size_t index, const K& key, V value) {
    auto& current = own(node);
    V* stored;
    if (current.height == 0) {
      current.entries.insert(current.entries.begin() + static_cast<std::ptrdiff_t>(index), Entry{key, std::move(value)});
      stored = &current.entries[index].second;
    } else {
      const auto child = child_at(current, index);
      const auto offset = index - (child == 0 ? 0 : current.children[child - 1].end);
      auto result = insert(current.children[child].node, offset, key, std::move(value));
      stored = result.value;
      if (result.right) current.children.insert(current.children.begin() + static_cast<std::ptrdiff_t>(child + 1),
                                                Child{first(*result.right), std::move(result.right), 0});
    }
    refresh(current);
    if (occupancy(current) <= capacity(current)) return {nullptr, stored};
    auto right = std::make_shared<Node>();
    right->height = current.height;
    const auto middle = occupancy(current) / 2;
    if (current.height == 0) {
      move_suffix(current.entries, right->entries, middle);
      stored = index < middle ? &current.entries[index].second : &right->entries[index - middle].second;
    } else move_suffix(current.children, right->children, middle);
    refresh(current);
    refresh(*right);
    return {std::move(right), stored};
  }
  template <class T>
  static void move_suffix(std::vector<T>& from, std::vector<T>& to, std::size_t start) {
    const auto it = from.begin() + static_cast<std::ptrdiff_t>(start);
    to.insert(to.end(), std::make_move_iterator(it), std::make_move_iterator(from.end()));
    from.erase(it, from.end());
  }
  static std::size_t capacity(const Node& node) { return node.height == 0 ? Leaf : std::max<std::size_t>(4, Leaf); }
  static std::size_t occupancy(const Node& node) { return node.height == 0 ? node.entries.size() : node.children.size(); }
  void erase_at(std::size_t index) {
    ++revision_;
    erase(root_, index);
    if (root_->size == 0) { clear(); return; }
    while (root_->height != 0 && root_->children.size() == 1) {
      auto child = root_->children.front().node;
      root_ = std::move(child);
    }
  }
  static void erase(std::shared_ptr<Node>& node, std::size_t index) {
    auto& current = own(node);
    if (current.height == 0) current.entries.erase(current.entries.begin() + static_cast<std::ptrdiff_t>(index));
    else {
      const auto child = child_at(current, index);
      const auto offset = index - (child == 0 ? 0 : current.children[child - 1].end);
      erase(current.children[child].node, offset);
      if (occupancy(*current.children[child].node) < (capacity(*current.children[child].node) + 1) / 2) rebalance(current, child);
    }
    refresh(current);
  }
  static void rebalance(Node& parent, std::size_t child) {
    const auto left_index = child == 0 ? 0 : child - 1;
    auto& left = own(parent.children[left_index].node);
    auto& right = own(parent.children[left_index + 1].node);
    if (occupancy(left) + occupancy(right) <= capacity(left)) {
      if (left.height == 0) move_suffix(right.entries, left.entries, 0);
      else move_suffix(right.children, left.children, 0);
      parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(left_index + 1));
    } else {
      if (left.height == 0) borrow(left.entries, right.entries, child != 0);
      else borrow(left.children, right.children, child != 0);
      refresh(right);
    }
    refresh(left);
  }
  template <class T>
  static void borrow(std::vector<T>& left, std::vector<T>& right, bool from_left) {
    if (from_left) {
      right.insert(right.begin(), std::move(left.back()));
      left.pop_back();
    } else {
      left.push_back(std::move(right.front()));
      right.erase(right.begin());
    }
  }
  std::shared_ptr<Node> root_;
  // A find iterator may reuse its lookup until any write or assignment. The
  // fallback still reads its position through the owner, including after COW.
  std::size_t revision_ = 0;
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
    friend class SharedSet;
    typename Map::const_iterator it_;
  };
  using iterator = const_iterator;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  SharedSet() = default;
  SharedSet(std::initializer_list<K> items) { for (const auto& item : items) insert(item); }
  [[nodiscard]] std::size_t size() const { return map_.size(); }
  [[nodiscard]] bool empty() const { return map_.empty(); }
  [[nodiscard]] bool contains(const K& key) const { return map_.contains(key); }
  [[nodiscard]] std::size_t count(const K& key) const { return map_.count(key); }
  bool insert(const K& key) { return map_.emplace(key, Present{}); }
  std::size_t erase(const K& key) { return map_.erase(key); }
  const_iterator erase(const_iterator it) { return const_iterator(map_.erase(it.it_)); }
  void clear() { map_.clear(); }
  const_iterator begin() const { return const_iterator(map_.begin()); }
  const_iterator end() const { return const_iterator(map_.end()); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  const_iterator find(const K& key) const { return const_iterator(map_.find(key)); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
  /// Common positions in unshared subtrees, then newly appended positions.
  template <class Both, class Added>
  static void compare(const SharedSet& before, const SharedSet& after, Both&& both, Added&& added) {
    Map::compare_positions(before.map_, after.map_, std::forward<Both>(both), std::forward<Added>(added));
  }
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
