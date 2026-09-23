#pragma once

#include <functional>
#include <string>
#include <vector>

#include "openport/net/http.hpp"

namespace openport::test {

class HttpStub final : public net::HttpClient {
 public:
  net::HttpResponse get(std::string_view url, const net::Headers&, std::chrono::seconds) override {
    urls.emplace_back(url);
    return respond(url);
  }

  std::function<net::HttpResponse(std::string_view)> respond;
  std::vector<std::string> urls;
};

}  // namespace openport::test
