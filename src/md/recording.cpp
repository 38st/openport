#include "openport/md/recording.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <zstd.h>

#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace openport::md {
namespace {
constexpr std::array<char, 8> kMagic{'O', 'P', 'R', 'E', 'C', '\r', '\n', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxRecord = 1024 * 1024;
using Bytes = std::vector<char>;
static_assert(std::variant_size_v<Event> == 8, "update the recording codec for new event types");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

template <typename To, typename From>
To bits_of(const From& value) {
  static_assert(sizeof(To) == sizeof(From));
  To result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

[[noreturn]] void invalid(const std::string& reason) {
  throw std::runtime_error("recording: " + reason);
}

void check_zstd(std::size_t result) {
  if (ZSTD_isError(result)) invalid(std::string("zstd: ") + ZSTD_getErrorName(result));
}

/// Fixed-width little-endian fields avoid ABI padding and preserve floating bits.
class Encoder {
 public:
  explicit Encoder(Bytes& bytes) : bytes_(bytes), start_(bytes.size()) {}
  template <typename T>
  void number(T value) {
    if constexpr (std::is_same_v<T, double>) {
      number(bits_of<std::uint64_t>(value));
    } else {
      if (bytes_.size() - start_ + sizeof(T) > kMaxRecord + 4)
        invalid("record/header exceeds 1 MiB limit");
      std::uint64_t bits = bits_of<std::make_unsigned_t<T>>(value);
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes_.push_back(static_cast<char>(bits & 0xff));
        bits >>= 8;
      }
    }
  }
  void byte(std::uint8_t value) { number(value); }
  void string(const std::string& value) {
    if (value.size() > kMaxRecord || bytes_.size() - start_ + 4 + value.size() > kMaxRecord)
      invalid("string or record/header exceeds 1 MiB limit");
    number(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

 private:
  Bytes& bytes_;
  std::size_t start_;
};

class Decoder {
 public:
  explicit Decoder(std::span<const char> bytes) : bytes_(bytes) {}
  template <typename T>
  T number() {
    if constexpr (std::is_same_v<T, double>) {
      return bits_of<double>(number<std::uint64_t>());
    } else {
      if (bytes_.size() < sizeof(T)) invalid("incomplete field in record/header");
      std::make_unsigned_t<T> bits = 0;
      for (std::size_t i = 0; i < sizeof(T); ++i)
        bits |= static_cast<std::make_unsigned_t<T>>(static_cast<unsigned char>(bytes_[i]))
                << (8 * i);
      bytes_ = bytes_.subspan(sizeof(T));
      return bits_of<T>(bits);
    }
  }
  std::uint8_t byte(std::uint8_t maximum) {
    const auto value = number<std::uint8_t>();
    if (value > maximum) invalid("invalid boolean, enum or event tag");
    return value;
  }
  std::string string() {
    const auto length = number<std::uint32_t>();
    if (length > bytes_.size()) invalid("invalid string length");
    std::string value(bytes_.data(), length);
    bytes_ = bytes_.subspan(length);
    return value;
  }
  void finish() const {
    if (!bytes_.empty()) invalid("unexpected trailing fields");
  }

 private:
  std::span<const char> bytes_;
};

Bytes encode_header(const RecordingHeader& h) {
  Bytes data;
  Encoder e(data);
  e.string(h.provider);
  const auto& c = h.capabilities;
  e.byte(c.realtime);
  e.byte(c.realtime_plan_dependent);
  e.number<std::int64_t>(c.poll_interval.count());
  e.number<std::int64_t>(c.delay.count());
  e.byte(c.quotes);
  e.byte(c.trades);
  e.byte(c.open_interest);
  e.byte(c.vendor_greeks);
  e.byte(c.history);
  if (h.subscription.underlyings.size() > kMaxRecord / 4) invalid("too many subscribed symbols");
  e.number(static_cast<std::uint32_t>(h.subscription.underlyings.size()));
  for (const auto& symbol : h.subscription.underlyings) e.string(symbol);
  e.number<std::int32_t>(h.subscription.max_expiries);
  e.number(h.subscription.strike_window);
  e.number(h.started);
  if (data.size() > kMaxRecord) invalid("header exceeds 1 MiB limit");
  Bytes prefix(kMagic.begin(), kMagic.end());
  Encoder p(prefix);
  p.number(kVersion);
  p.number(static_cast<std::uint32_t>(data.size()));
  prefix.insert(prefix.end(), data.begin(), data.end());
  return prefix;
}

RecordingHeader decode_header(std::span<const char> data) {
  Decoder d(data);
  RecordingHeader h;
  h.provider = d.string();
  auto& c = h.capabilities;
  c.realtime = d.byte(1);
  c.realtime_plan_dependent = d.byte(1);
  c.poll_interval = std::chrono::seconds(d.number<std::int64_t>());
  c.delay = std::chrono::seconds(d.number<std::int64_t>());
  c.quotes = d.byte(1);
  c.trades = d.byte(1);
  c.open_interest = d.byte(1);
  c.vendor_greeks = d.byte(1);
  c.history = d.byte(1);
  const auto count = d.number<std::uint32_t>();
  if (count > kMaxRecord / 4) invalid("too many subscribed symbols");
  for (std::uint32_t i = 0; i < count; ++i) h.subscription.underlyings.push_back(d.string());
  h.subscription.max_expiries = d.number<std::int32_t>();
  h.subscription.strike_window = d.number<double>();
  h.started = d.number<Timestamp>();
  d.finish();
  return h;
}

void encode_event(Bytes& bytes, Timestamp received, const Event& event) {
  const auto start = bytes.size();
  Encoder e(bytes);
  e.number<std::uint32_t>(0);
  e.number(received);
  e.byte(static_cast<std::uint8_t>(event.index()));
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ContractDefinition>) {
          e.number(v.id);
          const auto& c = v.contract;
          e.string(c.root);
          e.string(c.underlying);
          e.number<std::int32_t>(c.expiry.year);
          e.number<std::int32_t>(c.expiry.month);
          e.number<std::int32_t>(c.expiry.day);
          e.number(c.strike);
          e.byte(static_cast<std::uint8_t>(c.type));
          e.byte(static_cast<std::uint8_t>(c.style));
          e.byte(static_cast<std::uint8_t>(c.settlement));
          e.number(c.multiplier);
          e.byte(c.standard);
        } else if constexpr (std::is_same_v<T, UnderlyingQuote>) {
          e.string(v.symbol);
          e.number(v.ts);
          e.number(v.bid);
          e.number(v.ask);
          e.number(v.last);
        } else if constexpr (std::is_same_v<T, ProviderStatus>) {
          e.number(v.ts);
          e.byte(static_cast<std::uint8_t>(v.state));
          e.string(v.message);
          e.string(v.underlying);
        } else if constexpr (std::is_same_v<T, UnderlyingClose>) {
          e.string(v.symbol);
          e.number(v.ts);
          e.number<std::int32_t>(v.date.year);
          e.number<std::int32_t>(v.date.month);
          e.number<std::int32_t>(v.date.day);
          e.number(v.price);
        } else {
          e.number(v.id);
          e.number(v.ts);
          if constexpr (std::is_same_v<T, OptionQuote>) {
            e.number(v.bid);
            e.number(v.ask);
            e.number(v.bid_size);
            e.number(v.ask_size);
          } else if constexpr (std::is_same_v<T, OptionTrade>) {
            e.number(v.price);
            e.number(v.size);
          } else if constexpr (std::is_same_v<T, OpenInterest>) {
            e.number(v.contracts);
          } else if constexpr (std::is_same_v<T, VendorGreeks>) {
            e.number(v.iv);
            e.number(v.delta);
            e.number(v.gamma);
            e.number(v.vega);
            e.number(v.theta);
            e.number(v.rho);
          }
        }
      },
      event);
  const auto length = bytes.size() - start - 4;
  if (length > kMaxRecord) invalid("event exceeds 1 MiB limit");
  for (int i = 0; i < 4; ++i) bytes[start + i] = static_cast<char>((length >> (8 * i)) & 0xff);
}

RecordedEvent decode_event(std::span<const char> bytes) {
  Decoder d(bytes);
  RecordedEvent out;
  out.received = d.number<Timestamp>();
  switch (d.byte(7)) {
    case 0: {
      ContractDefinition v;
      v.id = d.number<InstrumentId>();
      auto& c = v.contract;
      c.root = d.string();
      c.underlying = d.string();
      c.expiry = {d.number<std::int32_t>(), d.number<std::int32_t>(), d.number<std::int32_t>()};
      c.strike = d.number<double>();
      c.type = static_cast<pricing::OptionType>(d.byte(1));
      c.style = static_cast<pricing::ExerciseStyle>(d.byte(1));
      c.settlement = static_cast<Settlement>(d.byte(1));
      c.multiplier = d.number<double>();
      c.standard = d.byte(1);
      out.event = std::move(v);
      break;
    }
    case 1:
      out.event = OptionQuote{d.number<InstrumentId>(), d.number<Timestamp>(), d.number<double>(),
                              d.number<double>(),       d.number<double>(),    d.number<double>()};
      break;
    case 2:
      out.event = OptionTrade{d.number<InstrumentId>(), d.number<Timestamp>(), d.number<double>(),
                              d.number<double>()};
      break;
    case 3:
      out.event = OpenInterest{d.number<InstrumentId>(), d.number<Timestamp>(), d.number<double>()};
      break;
    case 4:
      out.event = VendorGreeks{d.number<InstrumentId>(), d.number<Timestamp>(), d.number<double>(),
                               d.number<double>(),       d.number<double>(),    d.number<double>(),
                               d.number<double>(),       d.number<double>()};
      break;
    case 5:
      out.event = UnderlyingQuote{d.string(), d.number<Timestamp>(), d.number<double>(),
                                  d.number<double>(), d.number<double>()};
      break;
    case 6:
      out.event = ProviderStatus{d.number<Timestamp>(), static_cast<FeedState>(d.byte(5)),
                                 d.string(), d.string()};
      break;
    case 7: {
      UnderlyingClose v;
      v.symbol = d.string();
      v.ts = d.number<Timestamp>();
      v.date = {d.number<std::int32_t>(), d.number<std::int32_t>(), d.number<std::int32_t>()};
      v.price = d.number<double>();
      out.event = std::move(v);
      break;
    }
  }
  d.finish();
  return out;
}

void write_all(int fd, std::span<const char> bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0)
      invalid(std::string("write failed: ") +
              (count < 0 ? std::strerror(errno) : "zero-byte write"));
    bytes = bytes.subspan(static_cast<std::size_t>(count));
  }
}

struct FileDescriptor {
  int fd = -1;
  ~FileDescriptor() {
    if (fd >= 0) ::close(fd);
  }
};
}  // namespace

struct RecordingSink::Impl {
  Impl(const std::filesystem::path& path, const RecordingHeader& header, EventSink& sink,
       Options opts)
      : downstream(sink), options(std::move(opts)) {
    if (options.frame_bytes == 0 || options.frame_bytes > 4 * 1024 * 1024 ||
        options.flush_interval <= std::chrono::milliseconds(0) ||
        options.flush_interval > std::chrono::seconds(1))
      invalid("frame size must be 1..4 MiB and flush interval 1..1000 ms");
    if (!options.write) options.write = write_all;
    const auto encoded = encode_header(header);
    file.fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file.fd < 0) invalid("cannot create " + path.string() + ": " + std::strerror(errno));
    options.write(file.fd, encoded);
    counters.bytes = encoded.size();
    pending.reserve(options.frame_bytes + kMaxRecord + 4);
    worker = std::thread([this] { run(); });
  }

  void fail(const std::string& reason) {
    if (!failure.empty()) return;
    failure = "recording stopped: " + reason;
    pending.clear();
    downstream.publish(ProviderStatus{options.clock(), FeedState::Error, failure, ""});
    ready.notify_all();
  }

  void run() {
    try {
      std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(),
                                                                   ZSTD_freeCCtx);
      if (!context) invalid("cannot allocate zstd compressor");
      check_zstd(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 1));
      check_zstd(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1));
      Bytes batch;
      batch.reserve(options.frame_bytes + kMaxRecord + 4);
      Bytes compressed(ZSTD_compressBound(options.frame_bytes + kMaxRecord + 8));
      auto deadline = std::chrono::steady_clock::now() + options.flush_interval;
      for (;;) {
        bool final;
        {
          std::unique_lock lock(mutex);
          ready.wait_until(lock, deadline, [&] {
            return closing || !failure.empty() || pending.size() >= options.frame_bytes;
          });
          if (!failure.empty()) break;
          final = closing;
          batch.swap(pending);
          // Admit no new publications until this frame is written: a process
          // crash can then lose at most one outstanding frame, not two buffers.
          writing = true;
        }
        if (final) Encoder(batch).number<std::uint32_t>(0);  // explicit clean EOF
        if (!batch.empty()) {
          const auto size = ZSTD_compress2(context.get(), compressed.data(), compressed.size(),
                                           batch.data(), batch.size());
          check_zstd(size);
          options.write(file.fd, std::span(compressed).first(size));
          const std::lock_guard lock(mutex);
          counters.bytes += size;
        }
        batch.clear();
        {
          const std::lock_guard lock(mutex);
          writing = false;
          ready.notify_all();
        }
        if (final) break;
        deadline = std::chrono::steady_clock::now() + options.flush_interval;
      }
      const int fd = file.fd;
      file.fd = -1;
      if (::close(fd) != 0) invalid(std::string("close failed: ") + std::strerror(errno));
    } catch (const std::exception& error) {
      const std::lock_guard lock(mutex);
      fail(error.what());
      // A failed recording must release its descriptor even while the live feed continues.
      if (file.fd >= 0) {
        ::close(file.fd);
        file.fd = -1;
      }
    }
  }

  EventSink& downstream;
  Options options;
  FileDescriptor file;
  mutable std::mutex mutex;
  std::condition_variable ready;
  Bytes pending;
  RecordingStats counters;
  std::string failure;
  bool closing = false;
  bool writing = false;
  std::thread worker;
};

RecordingSink::RecordingSink(const std::filesystem::path& path, const RecordingHeader& header,
                             EventSink& downstream, Options options)
    : impl_(std::make_unique<Impl>(path, header, downstream, std::move(options))) {}
RecordingSink::~RecordingSink() {
  close();
}
void RecordingSink::publish(Event event) {
  const auto started = std::chrono::steady_clock::now();
  auto& p = *impl_;
  std::unique_lock lock(p.mutex);
  p.ready.wait(lock, [&] {
    return p.closing || !p.failure.empty() ||
           (!p.writing && p.pending.size() < p.options.frame_bytes);
  });
  if (p.closing) throw std::logic_error("recording: publish after close");
  if (p.failure.empty()) {
    const auto previous = p.pending.size();
    try {
      encode_event(p.pending, p.options.clock(), event);
      ++p.counters.events;
    } catch (const std::exception& error) {
      p.pending.resize(previous);
      p.fail(error.what());
    }
  }
  p.downstream.publish(std::move(event));
  p.counters.publish_nanoseconds +=
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count());
  if (p.pending.size() >= p.options.frame_bytes) p.ready.notify_all();
}
void RecordingSink::close() {
  {
    const std::lock_guard lock(impl_->mutex);
    impl_->closing = true;
    impl_->ready.notify_all();
  }
  if (impl_->worker.joinable()) impl_->worker.join();
}
std::string RecordingSink::error() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->failure;
}
RecordingStats RecordingSink::stats() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->counters;
}

struct RecordingReader::Impl {
  explicit Impl(const std::filesystem::path& path) : file(path, std::ios::binary) {
    if (!file) invalid("cannot open " + path.string());
    std::array<char, 16> prefix{};
    file.read(prefix.data(), prefix.size());
    if (file.gcount() != static_cast<std::streamsize>(prefix.size())) invalid("truncated header");
    if (!std::equal(kMagic.begin(), kMagic.end(), prefix.begin())) invalid("invalid magic");
    Decoder d(std::span<const char>(prefix).subspan(8));
    const auto version = d.number<std::uint32_t>();
    if (version != kVersion) invalid("unsupported format version " + std::to_string(version));
    const auto length = d.number<std::uint32_t>();
    if (length > kMaxRecord) invalid("header exceeds 1 MiB limit");
    Bytes data(length);
    file.read(data.data(), length);
    if (file.gcount() != static_cast<std::streamsize>(length)) invalid("truncated header");
    header = decode_header(data);
    body = file.tellg();
    if (!context) invalid("cannot allocate zstd decoder");
    check_zstd(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 24));
  }

  /// The decompressor can return useful bytes before physical EOF within a frame.
  /// Only the incomplete last record is discarded, and absence of EOF is reported.
  bool fill(bool finish_frame = false) {
    while (out_pos == out_size) {
      if (finish_frame && frame_remaining == 0) return false;
      if (input.pos == input.size) {
        if (out_size == decoded.size() && frame_remaining != 0) {
          ZSTD_outBuffer tail{decoded.data(), decoded.size(), 0};
          frame_remaining = ZSTD_decompressStream(context.get(), &tail, &input);
          check_zstd(frame_remaining);
          out_pos = 0;
          out_size = tail.pos;
          if (out_size != 0) return true;
        }
        file.read(compressed.data(), compressed.size());
        if (file.bad()) invalid("read failed");
        input = {compressed.data(), static_cast<std::size_t>(file.gcount()), 0};
        if (input.size == 0) return false;
      }
      ZSTD_outBuffer output{decoded.data(), decoded.size(), 0};
      frame_remaining = ZSTD_decompressStream(context.get(), &output, &input);
      check_zstd(frame_remaining);
      out_pos = 0;
      out_size = output.pos;
    }
    return true;
  }

  bool read(Bytes& bytes, std::size_t length) {
    bytes.clear();
    while (bytes.size() < length) {
      if (!fill()) return false;
      const auto count = std::min(length - bytes.size(), out_size - out_pos);
      bytes.insert(bytes.end(), decoded.data() + out_pos, decoded.data() + out_pos + count);
      out_pos += count;
    }
    return true;
  }

  std::ifstream file;
  std::streampos body;
  RecordingHeader header;
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{ZSTD_createDCtx(), ZSTD_freeDCtx};
  std::array<char, 64 * 1024> compressed{};
  std::array<char, 128 * 1024> decoded{};
  ZSTD_inBuffer input{compressed.data(), 0, 0};
  std::size_t out_pos = 0, out_size = 0, frame_remaining = 0;
  Bytes record;
  std::string diagnostic;
  bool ended = false;
};

RecordingReader::RecordingReader(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}
RecordingReader::~RecordingReader() = default;
const RecordingHeader& RecordingReader::header() const {
  return impl_->header;
}
const std::string& RecordingReader::diagnostic() const {
  return impl_->diagnostic;
}
std::optional<RecordedEvent> RecordingReader::next() {
  auto& p = *impl_;
  if (p.ended) return std::nullopt;
  auto truncated = [&]() -> std::optional<RecordedEvent> {
    p.ended = true;
    p.diagnostic = "recording truncated: missing clean end; recovered all complete records";
    return std::nullopt;
  };
  if (!p.read(p.record, 4)) return truncated();
  const auto length = Decoder(p.record).number<std::uint32_t>();
  if (length == 0) {
    if (p.fill(true)) invalid("data after clean end marker");
    if (p.frame_remaining != 0) return truncated();
    if (p.input.pos != p.input.size || p.file.peek() != std::char_traits<char>::eof())
      invalid("data after clean end marker");
    if (p.file.bad()) invalid("read failed");
    p.ended = true;
    return std::nullopt;
  }
  if (length > kMaxRecord) invalid("record exceeds 1 MiB limit");
  if (!p.read(p.record, length)) return truncated();
  return decode_event(p.record);
}
void RecordingReader::rewind() {
  auto& p = *impl_;
  p.file.clear();
  p.file.seekg(p.body);
  if (!p.file) invalid("rewind failed");
  check_zstd(ZSTD_DCtx_reset(p.context.get(), ZSTD_reset_session_only));
  p.input = {p.compressed.data(), 0, 0};
  p.out_pos = p.out_size = p.frame_remaining = 0;
  p.ended = false;
  p.diagnostic.clear();
}

}  // namespace openport::md
