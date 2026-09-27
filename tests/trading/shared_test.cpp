#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "openport/trading/shared.hpp"

namespace {
using namespace openport::trading;
using nlohmann::json;
using Ints = SharedVector<int, 8>;
using Counts = SharedMap<std::string, int, 4>;

TEST(SharedVector, CopiesShareUntilWrittenAndNeverSeeEachOthersWrites) {
  SharedVector<std::string, 4> a;
  for (int i = 0; i < 10; ++i) a.push_back("v" + std::to_string(i));
  auto b = a;
  EXPECT_TRUE(a.same(b));
  b.mut(5) = "changed";
  b.push_back("tail");
  EXPECT_EQ(a.size(), 10U);
  EXPECT_EQ(a[5], "v5");
  EXPECT_EQ(b[5], "changed");
  EXPECT_EQ(b.back(), "tail");
  // Untouched chunks stay shared; the written one does not.
  EXPECT_TRUE(a.same_chunk(b, 0));
  EXPECT_FALSE(a.same_chunk(b, 5));
  EXPECT_FALSE(a == b);
  b.mut(5) = "v5";
  b.truncate(10);
  EXPECT_TRUE(a == b);
  EXPECT_THROW((void)a.at(10), std::out_of_range);
  EXPECT_THROW((void)b.mut(10), std::out_of_range);
}

TEST(SharedVector, MatchesAVectorUnderRandomEditsAndSerializesTheSame) {
  std::mt19937 random(7);
  Ints shared;
  std::vector<int> plain;
  std::vector<std::pair<Ints, std::vector<int>>> copies;
  for (int step = 0; step < 4000; ++step) {
    const auto choice = random() % 10;
    if (choice < 5 || plain.empty()) {
      const int value = static_cast<int>(random() % 1000);
      shared.push_back(value);
      plain.push_back(value);
    } else if (choice < 8) {
      const auto i = random() % plain.size();
      const int value = static_cast<int>(random() % 1000);
      shared.mut(i) = value;
      plain[i] = value;
    } else if (choice < 9) {
      copies.emplace_back(shared, plain);
    } else {
      const auto count = random() % (plain.size() + 1);
      shared.truncate(count);
      plain.resize(std::min(plain.size(), static_cast<std::size_t>(count)));
    }
    ASSERT_EQ(shared.size(), plain.size());
  }
  EXPECT_EQ(shared.to_vector(), plain);
  EXPECT_EQ(json(shared).dump(), json(plain).dump());
  for (const auto& [copy, expected] : copies) EXPECT_EQ(copy.to_vector(), expected);
  EXPECT_EQ(json(shared).get<Ints>().to_vector(), plain);
  EXPECT_EQ(std::vector<int>(shared.rbegin(), shared.rend()), std::vector<int>(plain.rbegin(), plain.rend()));
}

TEST(SharedMap, MatchesAMapUnderRandomEditsAndSerializesTheSame) {
  std::mt19937 random(11);
  Counts shared;
  std::map<std::string, int> plain;
  std::vector<std::pair<Counts, std::map<std::string, int>>> copies;
  const auto key = [&] { return "k" + std::to_string(random() % 300); };
  for (int step = 0; step < 6000; ++step) {
    const auto choice = random() % 10;
    const auto k = key();
    if (choice < 5) {
      const int value = static_cast<int>(random() % 1000);
      shared[k] = value;
      plain[k] = value;
    } else if (choice < 7) {
      EXPECT_EQ(shared.erase(k), plain.erase(k));
    } else if (choice < 8 && plain.contains(k)) {
      shared.mut(k) += 1;
      plain[k] += 1;
    } else if (choice < 9) {
      EXPECT_EQ(shared.emplace(k, 5), plain.emplace(k, 5).second);
    } else {
      copies.emplace_back(shared, plain);
    }
    ASSERT_EQ(shared.size(), plain.size());
    ASSERT_EQ(shared.contains(k), plain.contains(k));
    if (plain.contains(k)) {
      ASSERT_EQ(shared.at(k), plain.at(k));
    }
  }
  const auto entries = [](const auto& map) {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& [k, v] : map) out.emplace_back(k, v);
    return out;
  };
  EXPECT_EQ(entries(shared), entries(plain));
  EXPECT_EQ(json(shared).dump(), json(plain).dump());
  for (const auto& [copy, expected] : copies) EXPECT_EQ(entries(copy), entries(expected));
  EXPECT_EQ(entries(json(shared).get<Counts>()), entries(plain));
  // Erasing by iterator returns the next entry, across chunk boundaries.
  for (auto it = shared.begin(); it != shared.end();) {
    if (it->second % 2 == 0) it = shared.erase(it); else ++it;
  }
  std::erase_if(plain, [](const auto& entry) { return entry.second % 2 == 0; });
  EXPECT_EQ(entries(shared), entries(plain));
  EXPECT_EQ(shared.find("absent"), shared.end());
  EXPECT_THROW((void)shared.at("absent"), std::out_of_range);
}

TEST(SharedSet, MatchesASetAndSerializesTheSame) {
  SharedSet<std::string> shared{"b", "a"};
  std::set<std::string> plain{"b", "a"};
  auto copy = shared;
  EXPECT_TRUE(shared.insert("c"));
  EXPECT_FALSE(shared.insert("c"));
  plain.insert("c");
  EXPECT_TRUE(shared.contains("c"));
  EXPECT_FALSE(copy.contains("c"));
  EXPECT_EQ(json(shared).dump(), json(plain).dump());
  EXPECT_EQ(std::vector<std::string>(shared.begin(), shared.end()), std::vector<std::string>(plain.begin(), plain.end()));
  EXPECT_EQ(json(shared).get<SharedSet<std::string>>(), shared);
}

template <std::size_t Chunk>
void vector_edits(std::size_t initial, int steps) {
  using Vector = SharedVector<int, Chunk>;
  std::mt19937 random(23);
  Vector shared;
  std::vector<int> plain;
  for (std::size_t i = 0; i < initial; ++i) {
    shared.push_back(static_cast<int>(i));
    plain.push_back(static_cast<int>(i));
  }
  std::vector<std::pair<Vector, std::vector<int>>> copies{{shared, plain}};
  for (int step = 0; step < steps; ++step) {
    const auto before = shared;
    const auto expected_before = plain;
    const auto choice = random() % 12;
    if (choice < 4 || plain.empty()) {
      const auto value = static_cast<int>(random() % 10000);
      shared.push_back(value);
      plain.push_back(value);
    } else if (choice < 8) {
      const auto index = random() % plain.size();
      shared.mut(index) = -step;
      plain[index] = -step;
    } else if (choice == 8) {
      shared.mut_back() = step;
      plain.back() = step;
    } else if (choice == 9) {
      if (copies.size() < 20) copies.emplace_back(shared, plain);
    } else if (choice == 10) {
      const auto count = plain.size() - random() % std::min<std::size_t>(plain.size(), 100);
      shared.truncate(count);
      plain.resize(count);
    } else if (step > steps / 2 && random() % 20 == 0) {
      shared.clear();
      plain.clear();
    }
    std::vector<std::size_t> changed, expected;
    Vector::compare(before, shared, [&](std::size_t index, int old, int now) {
      EXPECT_EQ(old, expected_before[index]);
      EXPECT_EQ(now, plain[index]);
      if (old != now) changed.push_back(index);
    });
    for (std::size_t i = 0; i < std::min(plain.size(), expected_before.size()); ++i) {
      if (plain[i] != expected_before[i]) expected.push_back(i);
    }
    ASSERT_EQ(changed, expected);
    ASSERT_EQ(shared.to_vector(), plain);
  }
  for (const auto& [copy, expected] : copies) {
    EXPECT_EQ(copy.to_vector(), expected);
    EXPECT_EQ(json(copy).dump(), json(expected).dump());
    EXPECT_EQ(json(copy).template get<Vector>(), copy);
  }
  EXPECT_EQ(std::vector<int>(shared.rbegin(), shared.rend()), std::vector<int>(plain.rbegin(), plain.rend()));
  auto moved = std::move(shared);
  EXPECT_TRUE(shared.empty());
  shared.push_back(7);
  EXPECT_EQ(shared.front(), 7);
  EXPECT_EQ(moved.to_vector(), plain);
}
TEST(SharedVector, DeepRandomEditsWithTwoEntryChunks) { vector_edits<2>(5000, 1500); }
TEST(SharedVector, DeepRandomEditsWithThreeEntryChunks) { vector_edits<3>(5000, 1500); }
TEST(SharedVector, DeepRandomEditsWithFourEntryChunks) { vector_edits<4>(5000, 1500); }
TEST(SharedVector, FiftyThousandEntriesAndRetainedSnapshots) { vector_edits<32>(50000, 200); }

TEST(SharedVector, ComparisonSkipsSharedSubtreesAcrossGrowthAndTruncation) {
  SharedVector<int, 2> before;
  for (int i = 0; i < 65536; ++i) before.push_back(i);
  auto after = before;
  after.push_back(65536);  // Grows the root beyond the unrolled read depths.
  EXPECT_EQ(after.back(), 65536);
  std::size_t visited = 0;
  const auto visit = [&](std::size_t, int, int) { ++visited; };
  SharedVector<int, 2>::compare(before, after, visit);
  EXPECT_EQ(visited, 0U);
  after.mut(71) = -1;
  SharedVector<int, 2>::compare(before, after, visit);
  EXPECT_EQ(visited, 2U);
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before.same_chunk(after, i), i / 2 != 71 / 2);
  }
  after.truncate(64);  // Collapses root levels, leaving shared complete chunks.
  visited = 0;
  SharedVector<int, 2>::compare(before, after, visit);
  EXPECT_EQ(visited, 0U);
  after.truncate(63);
  SharedVector<int, 2>::compare(before, after, visit);
  EXPECT_EQ(visited, 1U);
  after.push_back(-2);
  EXPECT_EQ(before[63], 63);
  EXPECT_EQ(after[63], -2);
  after.truncate(100);
  EXPECT_EQ(after.size(), 64U);
  after.truncate(0);
  EXPECT_TRUE(after.same(SharedVector<int, 2>{}));
}

template <class Map>
void check_map(const Map& shared, const std::map<std::string, int>& plain) {
  ASSERT_EQ(shared.size(), plain.size());
  auto expected = plain.begin();
  for (const auto& entry : shared) {
    ASSERT_NE(expected, plain.end());
    ASSERT_EQ(entry.first, expected->first);
    ASSERT_EQ(entry.second, expected->second);
    ++expected;
  }
  auto reverse = plain.rbegin();
  for (auto it = shared.rbegin(); it != shared.rend(); ++it, ++reverse) {
    ASSERT_EQ(it->first, reverse->first);
    ASSERT_EQ(it->second, reverse->second);
  }
}

template <class Map>
void check_map_change(const Map& before, const Map& after, const std::map<std::string, int>& old,
                      const std::map<std::string, int>& now) {
  std::vector<std::pair<std::string, char>> actual, expected;
  Map::compare(before, after,
      [&](const std::string& key, int value) {
        EXPECT_FALSE(old.contains(key));
        EXPECT_EQ(value, now.at(key));
        actual.emplace_back(key, '+');
      },
      [&](const std::string& key) {
        EXPECT_TRUE(old.contains(key));
        EXPECT_FALSE(now.contains(key));
        actual.emplace_back(key, '-');
      },
      [&](const std::string& key, int was, int value) {
        EXPECT_EQ(was, old.at(key));
        EXPECT_EQ(value, now.at(key));
        if (was != value) actual.emplace_back(key, '=');
      });
  for (const auto& [key, value] : old) {
    if (!now.contains(key)) expected.emplace_back(key, '-');
    else if (now.at(key) != value) expected.emplace_back(key, '=');
  }
  for (const auto& [key, value] : now) {
    (void)value;
    if (!old.contains(key)) expected.emplace_back(key, '+');
  }
  std::sort(expected.begin(), expected.end());
  ASSERT_EQ(actual, expected);
}

template <std::size_t Leaf>
void map_edits(int initial, int steps) {
  using Map = SharedMap<std::string, int, Leaf>;
  const auto key = [](int i) { return "k" + std::to_string(1000000 + i); };
  std::mt19937 random(71);
  Map shared;
  std::map<std::string, int> plain;
  std::vector<int> order;
  for (int i = 0; i < initial; ++i) order.push_back(i);
  std::shuffle(order.begin(), order.end(), random);
  for (const auto i : order) { shared.emplace(key(i), i); plain.emplace(key(i), i); }
  std::vector<std::pair<Map, std::map<std::string, int>>> copies{{shared, plain}};
  for (int step = 0; step < steps; ++step) {
    const auto before = shared;
    const auto old = plain;
    const auto k = key(static_cast<int>(random() % static_cast<unsigned>(initial + 100)));
    const auto choice = random() % 8;
    if (choice < 2) { shared[k] = step; plain[k] = step; }
    else if (choice == 2) { EXPECT_EQ(shared.emplace(k, step), plain.emplace(k, step).second); }
    else if (choice == 3 && plain.contains(k)) { shared.mut(k) = step; plain[k] = step; }
    else if (choice == 4) { EXPECT_EQ(shared.erase(k), plain.erase(k)); }
    else if (choice == 5 && plain.contains(k)) {
      const auto it = shared.erase(shared.find(k));
      const auto next = plain.erase(plain.find(k));
      ASSERT_EQ(it == shared.end(), next == plain.end());
      if (next != plain.end()) { EXPECT_EQ(it->first, next->first); }
    } else if (choice == 6 && copies.size() < 12) copies.emplace_back(shared, plain);
    EXPECT_EQ(shared.count(k), plain.count(k));
    check_map_change(before, shared, old, plain);
    check_map(shared, plain);
  }
  // Repeated deletion forces borrowing, merging and root collapse at every level.
  for (auto it = shared.begin(); it != shared.end();) {
    const auto before = shared;
    // Every check copies and walks the whole map: keep a large one to a few.
    const bool check = plain.size() <= 100 || (plain.size() - 1) % std::max<std::size_t>(997, static_cast<std::size_t>(initial) / 10) == 0;
    const auto old = check ? plain : std::map<std::string, int>{};
    const auto k = it->first;
    it = shared.erase(it);
    plain.erase(k);
    if (check) {
      check_map_change(before, shared, old, plain);
      check_map(shared, plain);
    }
  }
  EXPECT_TRUE(shared.same(Map{}));
  shared["new"] = 1;
  const auto copy = shared;
  shared.clear();
  check_map_change(copy, shared, {{"new", 1}}, {});
  check_map_change(shared, copy, {}, {{"new", 1}});
  EXPECT_THROW((void)shared.mut("absent"), std::out_of_range);
  for (const auto& [saved, expected] : copies) {
    check_map(saved, expected);
    EXPECT_EQ(json(saved).dump(), json(expected).dump());
    EXPECT_EQ(json(saved).template get<Map>(), saved);
  }
}
TEST(SharedMap, DeepRandomEditsWithTwoEntryLeaves) { map_edits<2>(700, 500); }
TEST(SharedMap, DeepRandomEditsWithThreeEntryLeaves) { map_edits<3>(700, 500); }
TEST(SharedMap, DeepRandomEditsWithFourEntryLeaves) { map_edits<4>(700, 500); }
TEST(SharedMap, FiftyThousandEntriesAndRetainedSnapshots) { map_edits<32>(50000, 20); }

TEST(SharedMap, ComparisonSkipsSharedSubtreesAndHandlesDifferentShapes) {
  using Map = SharedMap<std::string, int, 3>;
  Map original;
  std::map<std::string, int> plain;
  for (int i = 0; i < 3000; ++i) { original[std::to_string(i)] = i; plain[std::to_string(i)] = i; }
  auto edited = original;
  edited.mut("900") = -1;
  std::size_t visited = 0;
  Map::compare(original, edited, [](const auto&, int) { FAIL(); }, [](const auto&) { FAIL(); },
      [&](const auto&, int, int) { ++visited; });
  EXPECT_LE(visited, 3U);
  auto expected = plain;
  expected["900"] = -1;
  for (int i = 0; i < 3000; i += 2) { edited.erase(std::to_string(i)); expected.erase(std::to_string(i)); }
  for (int i = 3000; i < 4000; ++i) { edited[std::to_string(i)] = i; expected[std::to_string(i)] = i; }
  check_map_change(original, edited, plain, expected);
  check_map_change(edited, original, expected, plain);
  Map rebuilt;
  for (auto it = expected.rbegin(); it != expected.rend(); ++it) rebuilt.emplace(it->first, it->second);
  check_map_change(edited, rebuilt, expected, expected);
}

TEST(SharedMap, ComparisonAcrossEverySplitAndMerge) {
  using Map = SharedMap<std::string, int, 2>;
  Map shared;
  std::map<std::string, int> plain;
  for (int i = 0; i < 200; ++i) {
    const auto before = shared;
    const auto old = plain;
    const auto key = std::to_string(i);
    shared[key] = i;
    plain[key] = i;
    check_map_change(before, shared, old, plain);
    check_map_change(shared, before, plain, old);
  }
  bool front = true;
  while (!shared.empty()) {
    const auto before = shared;
    const auto old = plain;
    const auto it = front ? plain.begin() : std::prev(plain.end());
    shared.erase(it->first);
    plain.erase(it);
    front = !front;
    check_map_change(before, shared, old, plain);
    check_map_change(shared, before, plain, old);
  }
}

TEST(SharedSet, RandomEditsCopiesAndPositionalComparison) {
  std::mt19937 random(13);
  SharedSet<int> shared;
  std::set<int> plain;
  for (int i = 0; i < 50000; ++i) { shared.insert(i); plain.insert(i); }
  std::vector<std::pair<SharedSet<int>, std::set<int>>> copies{{shared, plain}};
  for (int step = 0; step < 150; ++step) {
    const auto before = shared;
    const std::vector<int> old(plain.begin(), plain.end());
    const int key = static_cast<int>(random() % 51000);
    if (random() % 2 == 0) { EXPECT_EQ(shared.insert(key), plain.insert(key).second); }
    else { EXPECT_EQ(shared.erase(key), plain.erase(key)); }
    if (step % 30 == 0) copies.emplace_back(shared, plain);
    const std::vector<int> now(plain.begin(), plain.end());
    std::vector<std::size_t> changed, expected;
    SharedSet<int>::compare(before, shared,
        [&](std::size_t i, int was, int value) {
          EXPECT_EQ(was, old[i]);
          EXPECT_EQ(value, now[i]);
          if (was != value) changed.push_back(i);
        },
        [&](std::size_t i, int value) { EXPECT_EQ(value, now[i]); changed.push_back(i); });
    for (std::size_t i = 0; i < now.size(); ++i) {
      if (i >= old.size() || old[i] != now[i]) expected.push_back(i);
    }
    EXPECT_EQ(changed, expected);
    EXPECT_EQ(std::vector<int>(shared.begin(), shared.end()), now);
  }
  for (auto it = shared.begin(); it != shared.end();) it = shared.erase(it);
  EXPECT_TRUE(shared.empty());
  for (const auto& [copy, expected] : copies) {
    EXPECT_EQ(json(copy).dump(), json(expected).dump());
    EXPECT_EQ(json(copy).get<SharedSet<int>>(), copy);
    EXPECT_EQ(std::vector<int>(copy.rbegin(), copy.rend()), std::vector<int>(expected.rbegin(), expected.rend()));
  }
  shared.insert(3);
  shared.clear();
  EXPECT_TRUE(shared.same(SharedSet<int>{}));
}

TEST(SharedContainers, IteratorsReadThroughTheOwnerAfterWritesAndAssignment) {
  SharedVector<int, 2> values{0, 1, 2, 3};
  auto vi = values.begin() + 2;
  const auto old_values = values;
  values.mut(2) = 20;
  EXPECT_EQ(*vi, 20);
  values = old_values;
  EXPECT_EQ(*vi, 2);
  values.push_back(4);
  EXPECT_EQ(vi[1], 3);
  EXPECT_EQ(values.end() - values.begin(), 5);
  SharedMap<int, int, 2> map{{0, 0}, {1, 1}, {2, 2}, {3, 3}};
  auto mi = map.find(2);
  const auto old_map = map;
  map.mut(2) = 20;
  EXPECT_EQ(mi->second, 20);
  map = old_map;
  EXPECT_EQ(mi->second, 2);
  map[4] = 4;
  EXPECT_EQ(mi->first, 2);
  EXPECT_EQ((--map.end())->first, 4);
  // A cached find must also survive writes to unique storage, root splits and
  // an assignment with the same number of writes on the other container.
  auto cached = map.find(1);
  map[5] = 5;
  EXPECT_EQ(cached->second, 1);
  SharedMap<int, int, 2> replacement{{0, 10}, {1, 11}, {2, 12}, {3, 13}, {4, 14}, {5, 15}};
  map = replacement;
  EXPECT_EQ(cached->second, 11);
  map = std::move(replacement);
  EXPECT_EQ(cached->second, 11);
}

TEST(SharedContainers, PublishedCopiesRemainReadableDuringWrites) {
  SharedVector<int, 2> values;
  SharedMap<int, int, 3> map;
  for (int i = 0; i < 10000; ++i) { values.push_back(i); map[i] = i; }
  const auto published_values = values;
  const auto published_map = map;
  std::atomic<bool> ready = false;
  std::thread reader([&] {
    ready.store(true, std::memory_order_release);
    for (int pass = 0; pass < 4; ++pass) {
      for (int i = 0; i < 10000; ++i) {
        EXPECT_EQ(published_values[static_cast<std::size_t>(i)], i);
        EXPECT_EQ(published_map.at(i), i);
      }
    }
  });
  while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
  for (int i = 0; i < 10000; ++i) {
    values.mut(static_cast<std::size_t>(i)) = -i;
    map.erase(i);
  }
  reader.join();
  EXPECT_TRUE(map.empty());
}
}  // namespace
