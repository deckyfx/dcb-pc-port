#pragma once
// Single-file build: the game binary with its bundle (assets/, cheats/, README.txt) appended.
//
// Layout of such a program (all integers little-endian):
//   [the executable, unchanged]
//   [a .pak archive (pak.hpp) of the bundle tree; its offsets are from the archive start]
//   [trailer, 32 bytes, the last in the file:
//      u64 archive offset, u64 archive size, u64 content id (PakWriter::content_id),
//      char[8] magic "DCBEXE01"]
// Neither ELF nor PE loaders look past their own sections, so the program runs as before; a
// program without the trailer is just the plain binary.
//
// At startup the game unpacks the archive next to itself once (unpack_payload) and then boots
// from the loose files exactly like the zip release. assets/.payload-id records which payload
// is there (its content id, then every file it wrote), so a newer program replaces it and an
// unchanged one starts at once.

#include "vfs/pak.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace vfs {

inline constexpr char kPayloadMagic[8] = {'D', 'C', 'B', 'E', 'X', 'E', '0', '1'};
inline constexpr uint64_t kPayloadTrailerSize = 32;
/// The unpack stamp, relative to the program's directory.
inline constexpr const char* kPayloadStamp = "assets/.payload-id";

struct PayloadTrailer {
    uint64_t offset = 0, size = 0, id = 0;
};

/// Write `out`: the bytes of `program`, then `pak`, then the trailer. False (and `error` says
/// why) on I/O errors; `out` is then removed.
bool write_payload_program(const std::filesystem::path& out, const std::filesystem::path& program,
                           const PakWriter& pak, std::string& error);

/// The trailer of `program`, if it carries a payload (magic and bounds checked).
std::optional<PayloadTrailer> find_payload(const std::filesystem::path& program);

/// True for bundle files the player owns once unpacked: never overwritten or removed
/// (cheats/<serial>.txt: the trainer saves into it).
bool payload_keeps_existing(const std::string& name);

enum class PayloadStatus {
    None,       ///< no payload: a plain binary
    UpToDate,   ///< this payload is already unpacked
    NotOurs,    ///< assets/ exists but was not unpacked from a payload: left alone
    Needed,     ///< (check_payload only) not unpacked yet, or another payload is there
    Unpacked,   ///< unpacked now
    Failed,     ///< could not unpack; `error` says why
};

struct PayloadResult {
    PayloadStatus status = PayloadStatus::None;
    PayloadTrailer trailer;
    size_t files = 0;      ///< files written
    size_t kept = 0;       ///< existing player files left in place
    size_t removed = 0;    ///< files of a previous payload that this one no longer has
    uint64_t bytes = 0;    ///< bytes written
    std::string error;
};

/// Called with (bytes done, bytes total) as the unpack goes.
using PayloadProgress = std::function<void(uint64_t, uint64_t)>;

/// What unpack_payload would do, without writing anything: None, UpToDate, NotOurs or Needed.
/// `trailer` receives the payload's trailer when there is one.
PayloadStatus check_payload(const std::filesystem::path& program, const std::filesystem::path& dest,
                            PayloadTrailer* trailer = nullptr);

/// Unpack `program`'s payload into `dest` when needed (see the statuses). A previous payload's
/// files that this one lacks are removed first; the stamp is written last, so an interrupted
/// unpack is redone on the next start.
PayloadResult unpack_payload(const std::filesystem::path& program, const std::filesystem::path& dest,
                             const PayloadProgress& progress = {});

}  // namespace vfs
