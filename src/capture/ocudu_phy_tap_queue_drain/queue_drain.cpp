#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

struct shared_symbol_record_header
{
  uint32_t magic        = 0x50595450;
  uint16_t version      = 1;
  uint16_t reserved     = 0;
  uint32_t eaxc_id      = 0;
  uint32_t frame_id     = 0;
  uint32_t subframe_id  = 0;
  uint32_t slot_id      = 0;
  uint32_t symbol_id    = 0;
  uint32_t numerology   = 0;
  uint32_t stream_index = 0;
  uint32_t sample_count = 0;
  uint64_t sequence     = 0;
  uint64_t timestamp_ns = 0;
};

struct shared_state_header
{
  uint32_t magic               = 0x52534250;
  uint16_t version             = 1;
  uint16_t reserved            = 0;
  uint32_t capacity            = 0;
  uint32_t sample_count        = 0;
  std::atomic<uint64_t> write_sequence{0};
  std::atomic<uint64_t> read_sequence{0};
  std::atomic<uint64_t> dropped_count{0};
  std::atomic<uint32_t> init_done{0};
};

struct options
{
  std::string queue_name = "/ocudu_phy_tap_queue";
  std::string log_file;
  std::optional<std::size_t> max_records;
  unsigned poll_sleep_ms = 1;
};

std::size_t round_up(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1U) & ~(alignment - 1U);
}

std::string normalize_shared_memory_name(std::string_view raw_name)
{
  std::string result(raw_name);
  if (result.empty()) {
    result = "/ocudu_phy_tap_queue";
  }
  if (result.front() != '/') {
    result.insert(result.begin(), '/');
  }
  for (char& ch : result) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      ch = '_';
    }
  }
  return result;
}

options parse_options(int argc, char** argv)
{
  options result;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    auto equals_pos = arg.find('=');
    auto key = arg.substr(0, equals_pos);
    auto value = (equals_pos == std::string_view::npos) ? std::string_view{} : arg.substr(equals_pos + 1);

    if (key == "--queue-name") {
      result.queue_name = std::string(value);
    } else if (key == "--log-file") {
      result.log_file = std::string(value);
    } else if (key == "--max-records") {
      result.max_records = std::stoull(std::string(value));
    } else if (key == "--poll-sleep-ms") {
      result.poll_sleep_ms = static_cast<unsigned>(std::stoul(std::string(value)));
    } else if (key == "--help" || key == "-h") {
      std::cout << "Usage: queue_drain [--queue-name=/name] [--max-records=N] [--log-file=path] [--poll-sleep-ms=1]\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << '\n';
      std::exit(1);
    }
  }

  result.queue_name = normalize_shared_memory_name(result.queue_name);
  return result;
}

class mapped_queue
{
public:
  explicit mapped_queue(const std::string& queue_name)
  {
    fd = ::shm_open(queue_name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
      throw std::runtime_error("failed to open shared memory " + queue_name + ": " + std::string(std::strerror(errno)));
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      throw std::runtime_error("failed to stat shared memory: " + std::string(std::strerror(errno)));
    }

    mapped_bytes = static_cast<std::size_t>(st.st_size);
    mapping = ::mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      throw std::runtime_error("failed to map shared memory: " + std::string(std::strerror(errno)));
    }
  }

  ~mapped_queue()
  {
    if (mapping != nullptr && mapping != MAP_FAILED) {
      ::munmap(mapping, mapped_bytes);
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }

  shared_state_header* state()
  {
    return reinterpret_cast<shared_state_header*>(mapping);
  }

  const shared_state_header* state() const
  {
    return reinterpret_cast<const shared_state_header*>(mapping);
  }

  std::byte* records_base()
  {
    return reinterpret_cast<std::byte*>(mapping) + sizeof(shared_state_header);
  }

private:
  int fd = -1;
  std::size_t mapped_bytes = 0;
  void* mapping = nullptr;
};

void write_line(std::ostream& out, const shared_symbol_record_header& header, uint64_t dropped_count)
{
  out << "seq=" << header.sequence
      << " eAxC=" << header.eaxc_id
      << " frame=" << header.frame_id
      << " subframe=" << header.subframe_id
      << " slot=" << header.slot_id
      << " symbol=" << header.symbol_id
      << " numerology=" << header.numerology
      << " stream=" << header.stream_index
      << " samples=" << header.sample_count
      << " ts_ns=" << header.timestamp_ns
      << " dropped=" << dropped_count
      << '\n';
}

} // namespace

int main(int argc, char** argv)
{
  try {
    const options opts = parse_options(argc, argv);
    std::unique_ptr<std::ofstream> log_file;
    std::ostream* out = &std::cout;

    if (!opts.log_file.empty()) {
      log_file = std::make_unique<std::ofstream>(opts.log_file, std::ios::out | std::ios::app);
      if (!*log_file) {
        throw std::runtime_error("failed to open log file: " + opts.log_file);
      }
      out = log_file.get();
    }

    mapped_queue queue(opts.queue_name);
    auto* state = queue.state();

    if (state->magic != 0x52534250 || state->version != 1) {
      throw std::runtime_error("shared queue layout mismatch or uninitialized queue: " + opts.queue_name);
    }

    const std::size_t record_stride = round_up(sizeof(shared_symbol_record_header) +
                                                   static_cast<std::size_t>(state->sample_count) * sizeof(std::uint16_t) * 2,
                                               alignof(std::max_align_t));

    *out << "opened queue " << opts.queue_name
         << " capacity=" << state->capacity
         << " sample_count=" << state->sample_count
         << " record_stride=" << record_stride
         << '\n';

    std::size_t printed = 0;
    while (!opts.max_records.has_value() || printed < *opts.max_records) {
      const uint64_t read_seq = state->read_sequence.load(std::memory_order_acquire);
      const uint64_t write_seq = state->write_sequence.load(std::memory_order_acquire);

      if (read_seq == write_seq) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.poll_sleep_ms));
        continue;
      }

      const std::size_t slot_index = static_cast<std::size_t>(read_seq % state->capacity);
      std::byte* slot_address = queue.records_base() + slot_index * record_stride;
      auto* header = reinterpret_cast<shared_symbol_record_header*>(slot_address);

      if (header->magic != 0x50595450 || header->sample_count != state->sample_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.poll_sleep_ms));
        continue;
      }

      if (header->sequence != read_seq) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.poll_sleep_ms));
        continue;
      }

      write_line(*out, *header, state->dropped_count.load(std::memory_order_relaxed));
      out->flush();

      state->read_sequence.store(read_seq + 1, std::memory_order_release);
      ++printed;
    }

    if (opts.max_records.has_value()) {
      *out << "stopped after " << printed << " records\n";
    }
    *out << "dropped_total=" << state->dropped_count.load(std::memory_order_relaxed) << '\n';
    out->flush();
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "queue_drain: " << ex.what() << '\n';
    return 1;
  }
}
