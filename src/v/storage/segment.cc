#include "storage/segment.h"

#include "storage/logger.h"
#include "storage/segment_appender_utils.h"
#include "vassert.h"
#include "vlog.h"

#include <seastar/core/future-util.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>

#include <stdexcept>

namespace storage {

segment::segment(
  segment::offset_tracker tkr,
  segment_reader r,
  segment_index i,
  std::optional<segment_appender> a,
  std::optional<batch_cache_index> c) noexcept
  : _tracker(tkr)
  , _reader(std::move(r))
  , _idx(std::move(i))
  , _appender(std::move(a))
  , _cache(std::move(c)) {}

void segment::check_segment_not_closed(const char* msg) {
    if (unlikely(_closed)) {
        throw std::runtime_error(fmt::format(
          "Attempted to perform operation: '{}' on a closed segment: {}",
          msg,
          *this));
    }
}

ss::future<> segment::close() {
    check_segment_not_closed("closed()");
    _closed = true;
    /**
     * close() is considered a destructive operation. All future IO on this
     * segment is unsafe. write_lock() ensures that we want for any active
     * readers and writers to finish before performing a destructive operation
     */
    return write_lock().then([this](ss::rwlock::holder h) {
        return do_close()
          .then([this] { return remove_thombsones(); })
          .finally([h = std::move(h)] {});
    });
}

ss::future<> segment::remove_thombsones() {
    if (!_tombstone) {
        return ss::make_ready_future<>();
    }
    std::vector<ss::sstring> rm;
    rm.push_back(reader().filename());
    rm.push_back(index().filename());
    vlog(stlog.info, "removing: {}", rm);
    return ss::do_with(std::move(rm), [](std::vector<ss::sstring>& to_remove) {
        return ss::do_for_each(to_remove, [](const ss::sstring& name) {
            return ss::remove_file(name).handle_exception(
              [](std::exception_ptr e) {
                  vlog(stlog.info, "error removing segment files: {}", e);
              });
        });
    });
}

ss::future<> segment::do_close() {
    auto f = _reader.close();
    if (_appender) {
        f = f.then([this] { return _appender->close(); });
    }
    // after appender flushes to make sure we make things visible
    // only after appender flush
    f = f.then([this] { return _idx.close(); });
    return f;
}

ss::future<> segment::release_appender() {
    vassert(_appender, "cannot release a null appender");
    return write_lock().then([this](ss::rwlock::holder h) {
        return do_flush()
          .then([this] { return _appender->close(); })
          .then([this] { return _idx.flush(); })
          .then([this] {
              _appender = std::nullopt;
              _cache = std::nullopt;
          })
          .finally([h = std::move(h)] {});
    });
}

ss::future<> segment::flush() {
    return read_lock().then([this](ss::rwlock::holder h) {
        return do_flush().finally([h = std::move(h)] {});
    });
}
ss::future<> segment::do_flush() {
    check_segment_not_closed("flush()");
    if (!_appender) {
        return ss::make_ready_future<>();
    }
    auto o = _tracker.dirty_offset;
    auto fsize = _appender->file_byte_offset();
    return _appender->flush().then([this, o, fsize] {
        _tracker.committed_offset = o;
        _reader.set_file_size(fsize);
    });
}

ss::future<>
segment::truncate(model::offset prev_last_offset, size_t physical) {
    check_segment_not_closed("truncate()");
    return write_lock().then(
      [this, prev_last_offset, physical](ss::rwlock::holder h) {
          return do_truncate(prev_last_offset, physical)
            .finally([h = std::move(h)] {});
      });
}

ss::future<>
segment::do_truncate(model::offset prev_last_offset, size_t physical) {
    _tracker.committed_offset = _tracker.dirty_offset = prev_last_offset;
    _reader.set_file_size(physical);
    cache_truncate(prev_last_offset + model::offset(1));
    auto f = _idx.truncate(prev_last_offset);
    // physical file only needs *one* truncation call
    if (_appender) {
        f = f.then([this, physical] { return _appender->truncate(physical); });
    } else {
        f = f.then([this, physical] { return _reader.truncate(physical); });
    }
    return f;
}

ss::future<bool> segment::materialize_index() {
    vassert(
      _tracker.base_offset == _tracker.dirty_offset,
      "Materializing the index must happen tracking any data. {}",
      *this);
    return _idx.materialize_index().then([this](bool yn) {
        if (yn) {
            _tracker.committed_offset = _idx.max_offset();
            _tracker.dirty_offset = _idx.max_offset();
        }
        return yn;
    });
}

void segment::cache_truncate(model::offset offset) {
    check_segment_not_closed("cache_truncate()");
    if (likely(bool(_cache))) {
        _cache->truncate(offset);
    }
}

ss::future<append_result> segment::append(const model::record_batch& b) {
    check_segment_not_closed("append()");
    const auto start_physical_offset = _appender->file_byte_offset();
    // proxy serialization to segment_appender_utils
    return write(*_appender, b).then([this, &b, start_physical_offset] {
        _tracker.dirty_offset = b.last_offset();
        const auto end_physical_offset = _appender->file_byte_offset();
        vassert(
          end_physical_offset == start_physical_offset + b.header().size_bytes,
          "size must be deterministic: end_offset:{}, expected:{}",
          end_physical_offset,
          start_physical_offset + b.header().size_bytes);
        // index the write
        _idx.maybe_track(b.header(), start_physical_offset);
        auto ret = append_result{.base_offset = b.base_offset(),
                                 .last_offset = b.last_offset(),
                                 .byte_size = (size_t)b.size_bytes()};
        vassert(
          b.header().ctx.owner_shard,
          "Shard not set when writing to: {} - header: {}",
          *this,
          b.header());
        // cache always copies the batch
        cache_put(b);
        return ret;
    });
}
ss::future<append_result> segment::append(model::record_batch&& b) {
    return ss::do_with(std::move(b), [this](model::record_batch& b) mutable {
        return append(b);
    });
}

ss::input_stream<char>
segment::offset_data_stream(model::offset o, ss::io_priority_class iopc) {
    check_segment_not_closed("offset_data_stream()");
    auto nearest = _idx.find_nearest(o);
    size_t position = 0;
    if (nearest) {
        position = nearest->filepos;
    }
    return _reader.data_stream(position, iopc);
}

std::ostream& operator<<(std::ostream& o, const segment::offset_tracker& t) {
    fmt::print(
      o,
      "{{term:{}, base_offset:{}, committed_offset:{}, dirty_offset:{}}}",
      t.term,
      t.base_offset,
      t.committed_offset,
      t.dirty_offset);
    return o;
}

std::ostream& operator<<(std::ostream& o, const segment& h) {
    o << "{offset_tracker:" << h._tracker << ", reader=" << h._reader
      << ", writer=";
    if (h.has_appender()) {
        o << *h._appender;
    } else {
        o << "nullptr";
    }
    o << ", cache=";
    if (h._cache) {
        o << *h._cache;
    } else {
        o << "nullptr";
    }
    return o << ", closed=" << h._closed << ", tombstone=" << h._tombstone
             << ", index=" << h.index() << "}";
}

} // namespace storage
