#pragma once

#include <databento/enums.hpp>
#include <databento/timeseries.hpp>

#include "openport/providers/history.hpp"

namespace openport::providers {
struct DatabentoHistoryRequest {
  HistoryWindow window;
  databento::Schema schema;
  std::vector<std::string> symbols;
};
/// Saved SDK records enter through the same callback as TimeseriesGetRange.
using DatabentoHistoryFetch = std::function<void(const DatabentoHistoryRequest&,
                                                const databento::RecordCallback&)>;
[[nodiscard]] std::unique_ptr<HistorySource> databento_history(DatabentoHistoryFetch fetch);
}  // namespace openport::providers
