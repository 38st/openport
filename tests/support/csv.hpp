#pragma once

#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openport::test {

/// Minimal reader for the unquoted CSV fixtures under tests/data.
class CsvTable {
 public:
  explicit CsvTable(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("empty file " + path);
    const auto header = split(line);
    for (std::size_t i = 0; i < header.size(); ++i) columns_.emplace(header[i], i);
    while (std::getline(in, line)) {
      if (!line.empty()) rows_.push_back(split(line));
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

  [[nodiscard]] const std::string& text(std::size_t row, std::string_view column) const {
    const auto it = columns_.find(std::string(column));
    if (it == columns_.end()) throw std::out_of_range("no column " + std::string(column));
    return rows_.at(row).at(it->second);
  }

  [[nodiscard]] double number(std::size_t row, std::string_view column) const {
    return std::stod(text(row, column));
  }

 private:
  static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
  }

  std::unordered_map<std::string, std::size_t> columns_;
  std::vector<std::vector<std::string>> rows_;
};

}  // namespace openport::test
