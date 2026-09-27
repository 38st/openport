#include <gtest/gtest.h>

#include <map>
#include <random>
#include <set>
#include <string>
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
}  // namespace
