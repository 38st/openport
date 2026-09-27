#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "openport/net/http.hpp"

namespace openport::test {

class HttpStub final : public net::HttpClient {
 public:
  net::HttpResponse get(std::string_view url, const net::Headers& headers, std::chrono::seconds) override {
    urls.emplace_back(url);
    sent_headers.push_back(headers);
    return respond(url);
  }

  net::HttpResponse get_direct(std::string_view url, const net::Headers& headers,
                               std::chrono::seconds timeout, const std::atomic<bool>* cancel) override {
    if (cancel && cancel->load()) throw std::runtime_error("cancelled");
    return get(url, headers, timeout);
  }

  net::HttpResponse post(std::string_view url, std::string_view body, const net::Headers& headers,
                         std::chrono::seconds timeout, const std::atomic<bool>* cancel) override {
    bodies.emplace_back(body);
    return get_direct(url, headers, timeout, cancel);
  }

  std::function<net::HttpResponse(std::string_view)> respond;
  std::vector<std::string> urls;
  std::vector<net::Headers> sent_headers;
  std::vector<std::string> bodies;
};

}  // namespace openport::test
