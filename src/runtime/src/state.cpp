#include <psx/state.hpp>

namespace psx {

namespace {

std::string tag_name(uint32_t tag) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((tag >> (8 * i)) & 0xFFu);
        s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return "'" + s + "'";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// StateWriter

void StateWriter::begin(uint32_t tag, uint32_t version) {
    u32(tag);
    u32(version);
    open_.push_back(buf_.size());
    u64(0);  // size, patched by end()
}

void StateWriter::end() {
    if (open_.empty()) throw StateError("StateWriter::end without begin");
    const size_t at = open_.back();
    open_.pop_back();
    const uint64_t size = buf_.size() - at - sizeof(uint64_t);
    std::memcpy(buf_.data() + at, &size, sizeof size);
}

void StateWriter::bytes(const void* data, size_t size) {
    if (size == 0) return;
    const auto* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + size);
}

// ---------------------------------------------------------------------------------------------
// StateReader

void StateReader::fail(const std::string& why) const {
    std::string where;
    for (const Open& o : open_) where += (where.empty() ? "" : "/") + tag_name(o.tag);
    throw StateError("save state: " + why + (where.empty() ? "" : " (in " + where + ")"));
}

void StateReader::bytes(void* out, size_t size) {
    if (size > remaining()) fail("truncated data (" + std::to_string(size) + " bytes wanted, " +
                                 std::to_string(remaining()) + " left)");
    if (size) std::memcpy(out, data_.data() + pos_, size);
    pos_ += size;
}

bool StateReader::boolean() {
    const uint8_t v = u8();
    if (v > 1) fail("bad boolean " + std::to_string(v));
    return v != 0;
}

size_t StateReader::size(size_t max, size_t unit) {
    const uint64_t n = u64();
    if (n > max || (unit && n > remaining() / unit)) fail("count " + std::to_string(n) + " out of range");
    return static_cast<size_t>(n);
}

void StateReader::str(std::string& s, size_t max) {
    const size_t n = size(max);
    s.resize(n);
    if (n) bytes(s.data(), n);
}

uint32_t StateReader::begin(uint32_t tag, uint32_t max_version) {
    if (remaining() < 16) fail("missing chunk " + tag_name(tag));
    const uint32_t found = u32();
    const uint32_t version = u32();
    const uint64_t size = u64();
    if (found != tag) fail("expected chunk " + tag_name(tag) + ", found " + tag_name(found));
    if (version == 0 || version > max_version)
        fail("chunk " + tag_name(tag) + " version " + std::to_string(version) + " is not supported (max " +
             std::to_string(max_version) + ")");
    if (size > remaining()) fail("chunk " + tag_name(tag) + " is truncated");
    open_.push_back({tag, pos_ + static_cast<size_t>(size), limit_});
    limit_ = pos_ + static_cast<size_t>(size);
    return version;
}

void StateReader::end() {
    if (open_.empty()) fail("end() without begin()");
    if (pos_ != open_.back().end)
        fail(std::to_string(open_.back().end - pos_) + " unread bytes at the end of the chunk (size mismatch)");
    limit_ = open_.back().outer_limit;
    open_.pop_back();
}

}  // namespace psx
