// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// WHAT CHANGED VS SUYU
//  * Common::FS::GetSuyuPath / SuyuPath -> GetEdenPath / EdenPath
//    (src/common/fs/path_util.h:16-37, :231).
//  * suyu's scan of sibling emulators' roaming directories for keys
//    ({suyu,yuzu,sudachi,citron,Ryujinx}, suyu retro_core.cpp:182-207) is DELETED,
//    not ported. Every iOS app has its own container and cannot read another's, so
//    every candidate path is guaranteed absent.
//  * Common::FS::CreateDir / CreateDirs are [[nodiscard]] in Eden
//    (src/common/fs/fs.h:157, :185); suyu calls them bare, which is a build failure
//    under Eden's -Werror=all. Wrapped as void(...) - the idiom Eden itself uses.
//  * NEW: the whole SetupUserPaths function. suyu never repoints its data root; it
//    relies on GetSuyuPath's OS default. That cannot work on iOS - see below.
//  * NEW: the status report (Describe/StatusToken/DescribeStatus) and the firmware
//    scan. suyu has neither; it fails at load with a bare number.
//  * NEW: key install, firmware install (ZIP or folder) and firmware verify, plus the
//    C entry points in src/ios/App/EdenContentBridge.h that a frontend calls. suyu has
//    none of this; it assumes a desktop where somebody already put the files in place.
//
// THE ZIP READER, AND WHY THERE IS ONE HERE
//   Installing firmware means getting NCA files out of an archive with their names
//   intact, because RegisteredCache matches on the name and a renamed NCA is invisible
//   to it with no error anywhere (registered_cache.cpp:56-63). On iOS there is no
//   system unzip an app can call and no zlib on this target: zlib exists in the tree
//   (CMakeLists.txt:467-471) but `core` does not link it and this file must not change
//   src/libretro_core/CMakeLists.txt. So the reader below is self-contained: a ZIP
//   central-directory parser (with Zip64), plus a DEFLATE decoder written from RFC 1951
//   section 3.2, plus CRC-32 from RFC 1952 section 8.
//
//   Every entry is checked against the CRC-32 in its own central-directory record
//   before it is renamed into place. That is not decoration: it is what makes a decoder
//   written for this port safe to trust. A bug in the decoder shows up as a REFUSED
//   file with a named reason, never as a corrupt NCA installed silently.
//
//   Memory is bounded and does not scale with entry size. The compressed side is read
//   through a 64 KiB window (ByteSource) and the decompressed side is written out
//   through a 128 KiB sliding window (OutputSink) that retains only the 32 KiB DEFLATE
//   can refer back to. A 400 MB firmware archive is installed in ~200 KiB of buffers.
//
// THIS FILE CONTAINS NO KEYS, NO FIRMWARE AND NO GAME DATA, AND FETCHES NONE.
// It only reports on, extracts, and copies files the user supplied themselves, and
// every destination is inside the app's own container. There is no network access
// anywhere in this translation unit.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/api_version.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "ios/App/EdenContentBridge.h"
#include "libretro.h"
#include "libretro_core/retro_content.h"
#include "libretro_core/retro_core_state.h"

namespace LibretroCore::Content {

const char* const KEY_FILE_NAMES[5] = {
    "prod.keys", "dev.keys", "title.keys", "console.keys", "key_retail.bin",
};

namespace {

/// The root that is actually installed in the Common::FS EdenPath table.
///
/// This replaces the old `bool g_paths_ready` latch. The latch was a real bug: it made
/// the SECOND call to SetupUserPaths() a no-op whatever the root had become, while
/// AdoptKeys() re-ran ResolveRoot() live. An app that called
/// eden_libretro_set_data_root() after retro_init therefore had its keys copied out of
/// the NEW root's keys_import and into the OLD root's keys directory - where Eden,
/// still pointed at the old root, would read them, so it half-worked, which is worse
/// than failing. Everything now resolves against this one variable.
std::filesystem::path g_applied_root;

/// Where the data tree should live. Preference order:
///   1. eden_libretro_set_data_root() - what the iOS app should call.
///   2. RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY + "/eden".
/// Empty if neither is available, in which case Eden's own default applies and, on
/// iOS, lands somewhere dot-hidden - see the header comment in SetupUserPaths.
std::filesystem::path ResolveRoot() {
    if (!g_data_root_override.empty()) {
        return std::filesystem::path{g_data_root_override};
    }
    if (g_environ_cb != nullptr) {
        const char* dir = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir != nullptr) {
            return std::filesystem::path{dir} / "eden";
        }
    }
    return {};
}

/// The base key file Eden will actually open. KeyManager::ReloadKeys
/// (key_manager.cpp:568-576) reads dev.keys when Settings::values.use_dev_keys
/// (settings.h:920) is set and prod.keys otherwise; KeyManager::KeyFileExists
/// (key_manager.cpp:847-853) applies the same rule. Reproduced rather than guessed so
/// the path shown to the user is the path Eden reads.
const char* BaseKeyFileName() {
    return Settings::values.use_dev_keys.GetValue() ? "dev.keys" : "prod.keys";
}

std::string ToLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

/// If `file_name` is one of the five names Eden opens out of the keys directory - in
/// ANY case - return the canonical lowercase spelling. Case matters: ReloadKeys opens
/// the exact lowercase names, so a "Prod.keys" copied off a Mac would sit in the keys
/// directory being read by nothing at all, with no diagnostic anywhere.
const char* CanonicalKeyName(std::string_view file_name) {
    const auto lowered = ToLowerAscii(file_name);
    for (const char* const name : KEY_FILE_NAMES) {
        if (lowered == name) {
            return name;
        }
    }
    return nullptr;
}

bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// The name test FileSys::RegisteredCache applies when it accumulates content
/// (registered_cache.cpp:56-63): 32 hex digits followed by ".nca", or by ".cnmt.nca".
/// Reimplemented without <regex> - the original compiles two static std::regex objects,
/// which is a lot of code size and a first-call cost for a string comparison.
bool FollowsNcaIdFormat(std::string_view name) {
    constexpr std::size_t kIdLen = 32;
    constexpr std::string_view kNca = ".nca";
    constexpr std::string_view kCnmtNca = ".cnmt.nca";

    const auto lowered = ToLowerAscii(name);
    const std::string_view view{lowered};
    const bool plain = view.size() == kIdLen + kNca.size() && view.substr(kIdLen) == kNca;
    const bool cnmt = view.size() == kIdLen + kCnmtNca.size() && view.substr(kIdLen) == kCnmtNca;
    if (!plain && !cnmt) {
        return false;
    }
    for (std::size_t i = 0; i < kIdLen; ++i) {
        if (!IsHexDigit(view[i])) {
            return false;
        }
    }
    return true;
}

/// The second layout the cache accepts: a "000000XX" bucket directory holding NCAs
/// (registered_cache.cpp:50-54).
bool FollowsTwoDigitDirFormat(std::string_view name) {
    constexpr std::string_view kPrefix = "000000";
    if (name.size() != kPrefix.size() + 2 || name.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    return IsHexDigit(name[6]) && IsHexDigit(name[7]);
}

/// Count NCA-shaped entries directly inside `dir`, without descending.
/// Both files and directories count: an installed NCA can be a plain
/// "<id>.nca" file or an "<id>.nca/" directory holding numbered chunks
/// (registered_cache.cpp:622-655 accepts both).
std::size_t CountNcasIn(const std::filesystem::path& dir) {
    if (!Common::FS::IsDir(dir)) {
        return 0;
    }
    std::size_t count = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (FollowsNcaIdFormat(it->path().filename().string())) {
            ++count;
        }
    }
    if (ec) {
        LOG_WARNING(Frontend, "libretro: could not read {}: {}",
                    Common::FS::PathToUTF8String(dir), ec.message());
    }
    return count;
}

// ---------------------------------------------------------------------------
// CRC-32 (RFC 1952 section 8) - the checksum every ZIP entry carries.
// ---------------------------------------------------------------------------

const std::array<u32, 256>& Crc32Table() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> built{};
        for (u32 i = 0; i < 256; ++i) {
            u32 value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            built[i] = value;
        }
        return built;
    }();
    return table;
}

/// Running value, pre-inversion. Start at 0xFFFFFFFF, finish by XORing 0xFFFFFFFF.
u32 Crc32Update(u32 running, u8 value) {
    return Crc32Table()[(running ^ value) & 0xFFu] ^ (running >> 8);
}

// ---------------------------------------------------------------------------
// Bounded-memory streaming plumbing.
// ---------------------------------------------------------------------------

/// A byte budget taken out of an already-positioned IOFile, read through a fixed
/// buffer. This is what keeps peak memory independent of how large an NCA is.
class ByteSource {
public:
    ByteSource(Common::FS::IOFile& source_file, u64 budget)
        : file{source_file}, remaining{budget}, buffer(64 * 1024, 0) {}

    bool Next(u8& value) {
        if (pos >= filled && !Refill()) {
            return false;
        }
        value = buffer[pos++];
        return true;
    }

private:
    bool Refill() {
        pos = 0;
        filled = 0;
        if (remaining == 0) {
            return false;
        }
        const auto want = static_cast<std::size_t>(
            std::min<u64>(remaining, static_cast<u64>(buffer.size())));
        const auto got = file.ReadSpan<u8>(std::span<u8>{buffer.data(), want});
        if (got == 0) {
            remaining = 0;
            return false;
        }
        filled = got;
        remaining -= static_cast<u64>(got);
        return true;
    }

    Common::FS::IOFile& file;
    u64 remaining;
    std::vector<u8> buffer;
    std::size_t pos = 0;
    std::size_t filled = 0;
};

/// LSB-first bit reader, the order RFC 1951 section 3.1.1 specifies for DEFLATE.
class BitReader {
public:
    explicit BitReader(ByteSource& source) : src{source} {}

    /// `count` must be <= 24, which is enough for every DEFLATE field (the widest is
    /// a 15-bit code plus 13 extra bits, never read in one call).
    u32 Bits(u32 count) {
        while (bit_count < count) {
            u8 byte = 0;
            if (!src.Next(byte)) {
                overrun = true;
                return 0;
            }
            bit_buf |= static_cast<u32>(byte) << bit_count;
            bit_count += 8;
        }
        const u32 mask = count == 32 ? ~0u : (1u << count) - 1u;
        const u32 value = bit_buf & mask;
        bit_buf >>= count;
        bit_count -= count;
        return value;
    }

    /// Drop the partial bits only. Whole bytes already pulled out of the source stay
    /// buffered - throwing them away would skip real compressed data.
    void AlignToByte() {
        const u32 partial = bit_count % 8u;
        bit_buf >>= partial;
        bit_count -= partial;
    }

    bool Overrun() const {
        return overrun;
    }

private:
    ByteSource& src;
    u32 bit_buf = 0;
    u32 bit_count = 0;
    bool overrun = false;
};

/// Decompressed output: CRC-checked, written straight to a file, keeping only the
/// 32 KiB of history DEFLATE back-references can reach.
class OutputSink {
public:
    explicit OutputSink(Common::FS::IOFile& destination) : out{destination} {
        window.reserve(kHigh);
    }

    void Put(u8 value) {
        window.push_back(value);
        crc = Crc32Update(crc, value);
        ++total;
        if (window.size() >= kHigh) {
            FlushDown(kKeep);
        }
    }

    bool CopyBack(std::size_t distance, std::size_t length) {
        if (distance == 0 || distance > window.size()) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            // Recomputed every iteration on purpose: DEFLATE copies may overlap the
            // bytes they are producing, which is how run-length encoding falls out of
            // a plain LZ77 back-reference.
            Put(window[window.size() - distance]);
        }
        return true;
    }

    /// Write everything still buffered. Must be called before the CRC is trusted.
    bool Finish() {
        FlushDown(0);
        return ok;
    }

    u32 Crc() const {
        return crc ^ 0xFFFFFFFFu;
    }

    u64 Total() const {
        return total;
    }

private:
    static constexpr std::size_t kKeep = 32 * 1024;  ///< max DEFLATE back-reference
    static constexpr std::size_t kHigh = 128 * 1024; ///< flush threshold

    void FlushDown(std::size_t keep) {
        if (window.size() <= keep) {
            return;
        }
        const std::size_t writable = window.size() - keep;
        if (out.WriteSpan<u8>(std::span<const u8>{window.data(), writable}) != writable) {
            ok = false;
        }
        window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(writable));
    }

    Common::FS::IOFile& out;
    std::vector<u8> window;
    u32 crc = 0xFFFFFFFFu;
    u64 total = 0;
    bool ok = true;
};

// ---------------------------------------------------------------------------
// DEFLATE (RFC 1951).
// ---------------------------------------------------------------------------

/// Canonical Huffman table: how many codes exist of each length, and the symbols in
/// canonical order. Decoding walks lengths 1..15 accumulating one bit at a time, which
/// needs no lookup table and no allocation per symbol.
struct HuffTable {
    std::array<u16, 16> count{};
    std::vector<u16> symbol;
};

/// Returns false only for an over-subscribed set (more codes of some length than the
/// tree can hold), which is corrupt data. An INCOMPLETE set is accepted: DEFLATE
/// legitimately produces one when a block has a single distance code, and a code that
/// is incomplete in a way that matters will simply fail to decode.
bool BuildHuffman(HuffTable& table, const u8* lengths, std::size_t count) {
    table.count.fill(0);
    for (std::size_t i = 0; i < count; ++i) {
        ++table.count[lengths[i]];
    }
    if (table.count[0] == count) {
        table.symbol.clear();
        return true; // no codes at all; any use of this table will fail to decode
    }

    int left = 1;
    for (std::size_t len = 1; len < 16; ++len) {
        left <<= 1;
        left -= static_cast<int>(table.count[len]);
        if (left < 0) {
            return false;
        }
    }

    std::array<u16, 16> offsets{};
    for (std::size_t len = 1; len < 15; ++len) {
        offsets[len + 1] = static_cast<u16>(offsets[len] + table.count[len]);
    }

    table.symbol.assign(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (lengths[i] != 0) {
            table.symbol[offsets[lengths[i]]++] = static_cast<u16>(i);
        }
    }
    return true;
}

/// -1 on a code that is not in the table, or on running out of input.
int DecodeSymbol(BitReader& reader, const HuffTable& table) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (std::size_t len = 1; len < 16; ++len) {
        code |= static_cast<int>(reader.Bits(1));
        if (reader.Overrun()) {
            return -1;
        }
        const int count = static_cast<int>(table.count[len]);
        if (code - first < count) {
            const auto slot = static_cast<std::size_t>(index + (code - first));
            if (slot >= table.symbol.size()) {
                return -1;
            }
            return table.symbol[slot];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

enum class InflateStatus {
    Ok,
    Truncated, ///< ran out of compressed input
    BadData,   ///< the stream is not valid DEFLATE
    WriteError,
};

// RFC 1951 section 3.2.5, tables 1 and 2.
constexpr std::array<u16, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                                             15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<u16, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<u16, 30> kDistanceBase = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,   33,   49,    65,    97,   129,
    193,  257,  385,  513,  769,  1025,  1537,  2049,  3073, 4097, 6145, 8193,  12289, 16385, 24577};
constexpr std::array<u16, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void BuildFixedTables(HuffTable& literals, HuffTable& distances) {
    // RFC 1951 section 3.2.6.
    std::array<u8, 288> literal_lengths{};
    for (std::size_t i = 0; i < 144; ++i) {
        literal_lengths[i] = 8;
    }
    for (std::size_t i = 144; i < 256; ++i) {
        literal_lengths[i] = 9;
    }
    for (std::size_t i = 256; i < 280; ++i) {
        literal_lengths[i] = 7;
    }
    for (std::size_t i = 280; i < 288; ++i) {
        literal_lengths[i] = 8;
    }
    static_cast<void>(BuildHuffman(literals, literal_lengths.data(), literal_lengths.size()));

    std::array<u8, 30> distance_lengths{};
    distance_lengths.fill(5);
    static_cast<void>(BuildHuffman(distances, distance_lengths.data(), distance_lengths.size()));
}

bool ReadDynamicTables(BitReader& reader, HuffTable& literals, HuffTable& distances) {
    // RFC 1951 section 3.2.7.
    static constexpr std::array<u8, 19> kCodeLengthOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                            11, 4,  12, 3, 13, 2, 14, 1, 15};

    const std::size_t literal_count = reader.Bits(5) + 257u;
    const std::size_t distance_count = reader.Bits(5) + 1u;
    const std::size_t code_length_count = reader.Bits(4) + 4u;
    if (reader.Overrun() || literal_count > 286 || distance_count > 30) {
        return false;
    }

    std::array<u8, 19> code_lengths{};
    for (std::size_t i = 0; i < code_length_count; ++i) {
        code_lengths[kCodeLengthOrder[i]] = static_cast<u8>(reader.Bits(3));
    }
    if (reader.Overrun()) {
        return false;
    }

    HuffTable code_length_table;
    if (!BuildHuffman(code_length_table, code_lengths.data(), code_lengths.size())) {
        return false;
    }

    std::array<u8, 320> lengths{};
    std::size_t index = 0;
    while (index < literal_count + distance_count) {
        const int symbol = DecodeSymbol(reader, code_length_table);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = static_cast<u8>(symbol);
            continue;
        }

        u8 value = 0;
        std::size_t repeat = 0;
        if (symbol == 16) {
            if (index == 0) {
                return false; // "copy the previous length" with no previous length
            }
            value = lengths[index - 1];
            repeat = reader.Bits(2) + 3u;
        } else if (symbol == 17) {
            repeat = reader.Bits(3) + 3u;
        } else {
            repeat = reader.Bits(7) + 11u;
        }
        if (reader.Overrun() || index + repeat > literal_count + distance_count) {
            return false;
        }
        for (std::size_t i = 0; i < repeat; ++i) {
            lengths[index++] = value;
        }
    }

    if (lengths[256] == 0) {
        return false; // no end-of-block code; the stream could never terminate
    }
    if (!BuildHuffman(literals, lengths.data(), literal_count)) {
        return false;
    }
    return BuildHuffman(distances, lengths.data() + literal_count, distance_count);
}

InflateStatus Inflate(BitReader& reader, OutputSink& sink) {
    bool last_block = false;
    while (!last_block) {
        last_block = reader.Bits(1) != 0;
        const u32 block_type = reader.Bits(2);
        if (reader.Overrun()) {
            return InflateStatus::Truncated;
        }

        if (block_type == 0) {
            // Stored. RFC 1951 section 3.2.4.
            reader.AlignToByte();
            const u32 length = reader.Bits(16);
            const u32 inverse = reader.Bits(16);
            if (reader.Overrun()) {
                return InflateStatus::Truncated;
            }
            if ((length ^ 0xFFFFu) != inverse) {
                return InflateStatus::BadData;
            }
            for (u32 i = 0; i < length; ++i) {
                const u32 byte = reader.Bits(8);
                if (reader.Overrun()) {
                    return InflateStatus::Truncated;
                }
                sink.Put(static_cast<u8>(byte));
            }
            continue;
        }

        if (block_type == 3) {
            return InflateStatus::BadData;
        }

        HuffTable literals;
        HuffTable distances;
        if (block_type == 1) {
            BuildFixedTables(literals, distances);
        } else if (!ReadDynamicTables(reader, literals, distances)) {
            return reader.Overrun() ? InflateStatus::Truncated : InflateStatus::BadData;
        }

        for (;;) {
            const int symbol = DecodeSymbol(reader, literals);
            if (symbol < 0) {
                return reader.Overrun() ? InflateStatus::Truncated : InflateStatus::BadData;
            }
            if (symbol < 256) {
                sink.Put(static_cast<u8>(symbol));
                continue;
            }
            if (symbol == 256) {
                break; // end of block
            }

            const auto length_index = static_cast<std::size_t>(symbol - 257);
            if (length_index >= kLengthBase.size()) {
                return InflateStatus::BadData;
            }
            const std::size_t length =
                kLengthBase[length_index] + reader.Bits(kLengthExtra[length_index]);

            const int distance_symbol = DecodeSymbol(reader, distances);
            if (distance_symbol < 0) {
                return reader.Overrun() ? InflateStatus::Truncated : InflateStatus::BadData;
            }
            const auto distance_index = static_cast<std::size_t>(distance_symbol);
            if (distance_index >= kDistanceBase.size()) {
                return InflateStatus::BadData;
            }
            const std::size_t distance =
                kDistanceBase[distance_index] + reader.Bits(kDistanceExtra[distance_index]);
            if (reader.Overrun()) {
                return InflateStatus::Truncated;
            }
            if (!sink.CopyBack(distance, length)) {
                return InflateStatus::BadData;
            }
        }
    }
    return InflateStatus::Ok;
}

// ---------------------------------------------------------------------------
// ZIP (APPNOTE.TXT 4.3 - central directory, local headers, Zip64).
// ---------------------------------------------------------------------------

constexpr u32 kSigEocd = 0x06054B50;
constexpr u32 kSigZip64Locator = 0x07064B50;
constexpr u32 kSigZip64Eocd = 0x06064B50;
constexpr u32 kSigCentralFile = 0x02014B50;
constexpr u32 kSigLocalFile = 0x04034B50;

u16 Le16(std::span<const u8> data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        return 0;
    }
    return static_cast<u16>(static_cast<u16>(data[offset]) |
                            static_cast<u16>(static_cast<u16>(data[offset + 1]) << 8));
}

u32 Le32(std::span<const u8> data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
           (static_cast<u32>(data[offset + 2]) << 16) | (static_cast<u32>(data[offset + 3]) << 24);
}

u64 Le64(std::span<const u8> data, std::size_t offset) {
    if (offset + 8 > data.size()) {
        return 0;
    }
    return static_cast<u64>(Le32(data, offset)) | (static_cast<u64>(Le32(data, offset + 4)) << 32);
}

struct ZipEntry {
    std::string name;
    u16 method = 0;
    u16 flags = 0;
    u32 crc = 0;
    u64 compressed_size = 0;
    u64 uncompressed_size = 0;
    u64 local_header_offset = 0;
};

/// Read the whole central directory. Returns false when the file is not a ZIP this can
/// read; `entries` is only meaningful on true.
bool ReadZipCentralDirectory(Common::FS::IOFile& file, std::vector<ZipEntry>& entries) {
    const u64 file_size = file.GetSize();
    if (file_size < 22) {
        return false;
    }

    // The end-of-central-directory record is last, but may be followed by up to 64 KiB
    // of archive comment, so it has to be searched for backwards.
    const u64 tail_size = std::min<u64>(file_size, 66u * 1024u);
    std::vector<u8> tail(static_cast<std::size_t>(tail_size));
    if (!file.Seek(static_cast<s64>(file_size - tail_size), Common::FS::SeekOrigin::SetOrigin)) {
        return false;
    }
    if (file.ReadSpan<u8>(tail) != tail.size()) {
        return false;
    }

    std::size_t eocd = tail.size();
    for (std::size_t probe = tail.size() >= 22 ? tail.size() - 22 : 0; ; --probe) {
        if (Le32(tail, probe) == kSigEocd) {
            eocd = probe;
            break;
        }
        if (probe == 0) {
            break;
        }
    }
    if (eocd == tail.size()) {
        return false;
    }

    u64 entry_count = Le16(tail, eocd + 10);
    u64 cd_size = Le32(tail, eocd + 12);
    u64 cd_offset = Le32(tail, eocd + 16);

    // Zip64. A firmware archive does not need it, but silently mis-reading a 0xFFFFFFFF
    // placeholder as a real offset is worse than not supporting it, so it is supported.
    if (entry_count == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        if (eocd < 20 || Le32(tail, eocd - 20) != kSigZip64Locator) {
            return false;
        }
        const u64 z64_offset = Le64(tail, eocd - 20 + 8);
        std::array<u8, 56> z64{};
        if (!file.Seek(static_cast<s64>(z64_offset), Common::FS::SeekOrigin::SetOrigin)) {
            return false;
        }
        if (file.ReadSpan<u8>(z64) != z64.size()) {
            return false;
        }
        if (Le32(z64, 0) != kSigZip64Eocd) {
            return false;
        }
        entry_count = Le64(z64, 32);
        cd_size = Le64(z64, 40);
        cd_offset = Le64(z64, 48);
    }

    if (cd_size == 0 || cd_size > 64u * 1024u * 1024u || cd_offset + cd_size > file_size) {
        return false;
    }

    std::vector<u8> directory(static_cast<std::size_t>(cd_size));
    if (!file.Seek(static_cast<s64>(cd_offset), Common::FS::SeekOrigin::SetOrigin)) {
        return false;
    }
    if (file.ReadSpan<u8>(directory) != directory.size()) {
        return false;
    }

    entries.clear();
    std::size_t cursor = 0;
    for (u64 i = 0; i < entry_count; ++i) {
        if (cursor + 46 > directory.size() || Le32(directory, cursor) != kSigCentralFile) {
            break;
        }
        ZipEntry entry;
        entry.flags = Le16(directory, cursor + 8);
        entry.method = Le16(directory, cursor + 10);
        entry.crc = Le32(directory, cursor + 16);
        entry.compressed_size = Le32(directory, cursor + 20);
        entry.uncompressed_size = Le32(directory, cursor + 24);
        const std::size_t name_len = Le16(directory, cursor + 28);
        const std::size_t extra_len = Le16(directory, cursor + 30);
        const std::size_t comment_len = Le16(directory, cursor + 32);
        entry.local_header_offset = Le32(directory, cursor + 42);

        const std::size_t name_at = cursor + 46;
        if (name_at + name_len + extra_len + comment_len > directory.size()) {
            break;
        }
        entry.name.assign(reinterpret_cast<const char*>(directory.data() + name_at), name_len);

        // Zip64 extended information extra field (header id 0x0001). The values appear
        // in a fixed order and ONLY for the fields that were written as placeholders.
        std::size_t extra_at = name_at + name_len;
        const std::size_t extra_end = extra_at + extra_len;
        while (extra_at + 4 <= extra_end) {
            const u16 header_id = Le16(directory, extra_at);
            const std::size_t data_size = Le16(directory, extra_at + 2);
            const std::size_t data_at = extra_at + 4;
            if (data_at + data_size > extra_end) {
                break;
            }
            if (header_id == 0x0001) {
                std::size_t field = data_at;
                if (entry.uncompressed_size == 0xFFFFFFFFu && field + 8 <= data_at + data_size) {
                    entry.uncompressed_size = Le64(directory, field);
                    field += 8;
                }
                if (entry.compressed_size == 0xFFFFFFFFu && field + 8 <= data_at + data_size) {
                    entry.compressed_size = Le64(directory, field);
                    field += 8;
                }
                if (entry.local_header_offset == 0xFFFFFFFFu && field + 8 <= data_at + data_size) {
                    entry.local_header_offset = Le64(directory, field);
                }
            }
            extra_at = data_at + data_size;
        }

        entries.push_back(std::move(entry));
        cursor = name_at + name_len + extra_len + comment_len;
    }

    return !entries.empty();
}

/// Decompress one entry to `destination`. Returns an empty string on success, or the
/// reason it failed.
std::string ExtractZipEntry(Common::FS::IOFile& archive, const ZipEntry& entry,
                            const std::filesystem::path& destination) {
    if ((entry.flags & 0x0001u) != 0) {
        return "the archive is encrypted";
    }
    if (entry.method != 0 && entry.method != 8) {
        return "unsupported compression method " + std::to_string(entry.method);
    }

    // The local header repeats the name and extra length, and they may DIFFER from the
    // central directory's. The data starts after the local copies, not the central ones.
    std::array<u8, 30> local{};
    if (!archive.Seek(static_cast<s64>(entry.local_header_offset),
                      Common::FS::SeekOrigin::SetOrigin)) {
        return "could not seek to its local header";
    }
    if (archive.ReadSpan<u8>(local) != local.size() || Le32(local, 0) != kSigLocalFile) {
        return "its local header is missing or malformed";
    }
    const u64 data_offset = entry.local_header_offset + 30u + Le16(local, 26) + Le16(local, 28);
    if (!archive.Seek(static_cast<s64>(data_offset), Common::FS::SeekOrigin::SetOrigin)) {
        return "could not seek to its data";
    }

    Common::FS::IOFile out{destination, Common::FS::FileAccessMode::Write};
    if (!out.IsOpen()) {
        return "could not create " + Common::FS::PathToUTF8String(destination);
    }

    ByteSource source{archive, entry.compressed_size};
    OutputSink sink{out};

    if (entry.method == 0) {
        for (u64 i = 0; i < entry.compressed_size; ++i) {
            u8 value = 0;
            if (!source.Next(value)) {
                return "the archive ends in the middle of it";
            }
            sink.Put(value);
        }
    } else {
        BitReader reader{source};
        switch (Inflate(reader, sink)) {
        case InflateStatus::Ok:
            break;
        case InflateStatus::Truncated:
            return "the archive ends in the middle of it";
        case InflateStatus::BadData:
            return "its compressed data is corrupt";
        case InflateStatus::WriteError:
            return "it could not be written";
        }
    }

    if (!sink.Finish()) {
        return "it could not be written - the device may be out of space";
    }
    if (entry.uncompressed_size != 0 && sink.Total() != entry.uncompressed_size) {
        return "it decompressed to the wrong size";
    }
    if (sink.Crc() != entry.crc) {
        return "it failed its CRC-32 check";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Firmware install helpers.
// ---------------------------------------------------------------------------

/// Delete NCA-named files (and "<id>.nca/" directories, and "000000XX" buckets) from
/// the firmware directory. Scoped as narrowly as it can be: it never recurses anywhere
/// else, and it only removes names the RegisteredCache would have read.
std::size_t ClearFirmwareDirectory(const std::filesystem::path& dir) {
    if (!Common::FS::IsDir(dir)) {
        return 0;
    }
    std::vector<std::filesystem::path> doomed;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (FollowsNcaIdFormat(name) || FollowsTwoDigitDirFormat(name)) {
            doomed.push_back(it->path());
        }
    }

    std::size_t removed = 0;
    for (const auto& victim : doomed) {
        std::error_code remove_ec;
        const auto count = std::filesystem::remove_all(victim, remove_ec);
        if (!remove_ec && count != static_cast<std::uintmax_t>(-1)) {
            removed += static_cast<std::size_t>(count);
        }
    }
    return removed;
}

void NoteProblem(FirmwareInstallReport& report, std::string text) {
    constexpr std::size_t kMaxProblems = 6;
    if (report.problems.size() < kMaxProblems) {
        report.problems.push_back(std::move(text));
    }
}

void InstallFirmwareFromZip(const std::filesystem::path& source,
                            const std::filesystem::path& firmware_dir,
                            FirmwareInstallReport& report) {
    Common::FS::IOFile archive{source, Common::FS::FileAccessMode::Read};
    if (!archive.IsOpen()) {
        report.result = FirmwareInstallResult::Unreadable;
        report.message = "Could not open " + Common::FS::PathToUTF8String(source) + ".";
        return;
    }

    std::vector<ZipEntry> entries;
    if (!ReadZipCentralDirectory(archive, entries)) {
        report.result = FirmwareInstallResult::Unreadable;
        report.message = "That file is not a ZIP archive this can read. If it is a .7z or a "
                         ".rar, unpack it first - only ZIP is supported here.";
        return;
    }

    for (const auto& entry : entries) {
        if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\')) {
            continue; // directory marker
        }
        // Match on the FILE NAME, not the path: firmware archives routinely wrap
        // everything in a "Firmware 19.0.1/" folder, and the RegisteredCache reads a
        // flat directory, so flattening is the correct thing to do rather than a
        // shortcut.
        const auto slash = entry.name.find_last_of("/\\");
        const std::string leaf =
            slash == std::string::npos ? entry.name : entry.name.substr(slash + 1);
        if (!FollowsNcaIdFormat(leaf)) {
            ++report.skipped;
            continue;
        }

        const auto final_path = firmware_dir / ToLowerAscii(leaf);
        const auto temp_path = firmware_dir / (ToLowerAscii(leaf) + ".part");
        const auto failure = ExtractZipEntry(archive, entry, temp_path);
        if (!failure.empty()) {
            ++report.failed;
            NoteProblem(report, leaf + ": " + failure);
            std::error_code cleanup_ec;
            std::filesystem::remove(temp_path, cleanup_ec);
            continue;
        }

        std::error_code rename_ec;
        std::filesystem::rename(temp_path, final_path, rename_ec);
        if (rename_ec) {
            ++report.failed;
            NoteProblem(report, leaf + ": " + rename_ec.message());
            std::error_code cleanup_ec;
            std::filesystem::remove(temp_path, cleanup_ec);
            continue;
        }
        ++report.installed;
    }
}

void InstallFirmwareFromDirectory(const std::filesystem::path& source,
                                  const std::filesystem::path& firmware_dir,
                                  FirmwareInstallReport& report) {
    std::error_code walk_ec;
    std::filesystem::recursive_directory_iterator it{
        source, std::filesystem::directory_options::skip_permission_denied, walk_ec};
    const std::filesystem::recursive_directory_iterator end;
    if (walk_ec) {
        report.result = FirmwareInstallResult::Unreadable;
        report.message = "Could not read that folder: " + walk_ec.message();
        return;
    }

    for (; it != end; it.increment(walk_ec)) {
        if (walk_ec) {
            break;
        }
        std::error_code kind_ec;
        if (!it->is_regular_file(kind_ec) || kind_ec) {
            continue;
        }
        const auto leaf = it->path().filename().string();
        if (!FollowsNcaIdFormat(leaf)) {
            ++report.skipped;
            continue;
        }

        const auto destination = firmware_dir / ToLowerAscii(leaf);
        // Already in the destination directory (the user picked the firmware folder
        // itself): nothing to do, and copying a file onto itself truncates it.
        std::error_code same_ec;
        if (std::filesystem::equivalent(it->path(), destination, same_ec) && !same_ec) {
            ++report.installed;
            continue;
        }

        std::error_code copy_ec;
        std::filesystem::copy_file(it->path(), destination,
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            ++report.failed;
            NoteProblem(report, leaf + ": " + copy_ec.message());
            continue;
        }
        ++report.installed;
    }
}

/// The version record FileSys stores in the SystemVersion title's RomFS, as "/file".
/// A byte-for-byte mirror of Service::Set::FirmwareVersionFormat
/// (core/hle/service/set/settings_types.h:436-449), reproduced here rather than
/// included: settings_types.h drags in service headers this target has no other reason
/// to compile, and the shape is fixed by the console, not by Eden. The static_assert
/// below is the guard - if the record ever stops being 0x100 bytes, this fails to
/// build rather than decoding garbage.
struct FirmwareVersionRecord {
    u8 major;
    u8 minor;
    u8 micro;
    u8 padding_1;
    u8 revision_major;
    u8 revision_minor;
    u8 padding_2[2];
    char platform[0x20];
    u8 version_hash[0x40];
    char display_version[0x18];
    char display_title[0x80];
};
static_assert(sizeof(FirmwareVersionRecord) == 0x100,
              "FirmwareVersionRecord must match Service::Set::FirmwareVersionFormat");

/// Fixed-width console strings are NOT guaranteed to be NUL-terminated.
std::string FromFixedString(const char* data, std::size_t capacity) {
    // Not strnlen: it is POSIX rather than C, so whether <cstring> declares it is a
    // platform question this file does not need to have an opinion about.
    std::size_t length = 0;
    while (length < capacity && data[length] != '\0') {
        ++length;
    }
    return std::string{data, length};
}

} // namespace

const std::filesystem::path& AppliedRoot() {
    return g_applied_root;
}

bool SetupUserPaths() {
    namespace FS = Common::FS;
    using FS::EdenPath;

    const auto root = ResolveRoot();

    if (root.empty()) {
        // Nothing new to apply. If a root was applied earlier the table is still valid;
        // only report failure when there has never been one.
        if (!g_applied_root.empty()) {
            return true;
        }
        LOG_WARNING(Frontend,
                    "libretro: no data root supplied; falling back to Eden's default. On iOS "
                    "that is $HOME/.local/share/eden inside the container - writable, but "
                    "dot-hidden and invisible to Files.app.");
        return false;
    }

    if (root == g_applied_root) {
        return true; // Idempotent: the table already points here.
    }

    // Common::FS::SetAppDirectory (path_util.h:222) does NOT work here.
    // PathManagerImpl::Reinitialize's non-Windows, non-Android branch
    // (src/common/fs/path_util.cpp:133-142) assigns over its own eden_path argument
    // with GetCurrentDir()/"user" and then $XDG_DATA_HOME/eden, so the argument is
    // discarded on every Apple platform; __ANDROID__ is the only branch that honours
    // it. Until path_util.cpp grows a TARGET_OS_IPHONE branch, each path is set by hand.
    //
    // Common::FS::SetEdenPath (path_util.cpp:302-309) logs an error and does nothing
    // if the new path is not ALREADY a directory, so every directory is created first.
    // Nothing else creates them: Common::FS::CreateEdenPaths() is called only from
    // src/qt_common/qt_common.cpp and src/yuzu/main_window.cpp, and path_util.cpp
    // explicitly defers creation.
    if (!FS::CreateDirs(root)) {
        LOG_CRITICAL(Frontend, "libretro: could not create Eden root at {}",
                     FS::PathToUTF8String(root));
        return false;
    }

    const auto set_path = [](EdenPath id, const std::filesystem::path& path) {
        void(FS::CreateDirs(path)); // [[nodiscard]] - src/common/fs/fs.h:185
        FS::SetEdenPath(id, path);
    };

    set_path(EdenPath::EdenDir, root);
    set_path(EdenPath::AmiiboDir, root / "amiibo");
    set_path(EdenPath::CacheDir, root / "cache");
    set_path(EdenPath::ConfigDir, root / "config");
    set_path(EdenPath::CrashDumpsDir, root / "crash_dumps");
    set_path(EdenPath::DumpDir, root / "dump");
    set_path(EdenPath::IconsDir, root / "icons");
    set_path(EdenPath::KeysDir, root / "keys");
    set_path(EdenPath::LoadDir, root / "load");
    set_path(EdenPath::LogDir, root / "log");
    set_path(EdenPath::LosslessDir, root / "lossless");
    set_path(EdenPath::NANDDir, root / "nand");
    set_path(EdenPath::PlayTimeDir, root / "play_time");
    set_path(EdenPath::PostPresetDir, root / "post_presets");
    set_path(EdenPath::PostShaderDir, root / "post_shaders");
    set_path(EdenPath::SaveDir, root / "nand"); // SaveDir aliases NANDDir upstream
    set_path(EdenPath::SDMCDir, root / "sdmc");
    set_path(EdenPath::ScreenshotsDir, root / "screenshots");
    set_path(EdenPath::ShaderDir, root / "cache" / "shader");
    set_path(EdenPath::TASDir, root / "tas");

    // The drop-off the app writes an imported prod.keys into.
    void(FS::CreateDirs(root / "keys_import"));

    // The firmware drop-off. FileSys::BISFactory creates this itself
    // (bis_factory.cpp:19-20, GetOrCreateDirectoryRelative(nand_root,
    // "/system/Contents/registered")) - but only once CreateFactories runs, which is
    // during a game load. Creating it up front is what lets the user find the folder in
    // Files.app BEFORE the first launch, which is the only moment they need it.
    void(FS::CreateDirs(root / "nand" / "system" / "Contents" / "registered"));

    if (!g_applied_root.empty()) {
        LOG_INFO(Frontend, "libretro: Eden data root moved {} -> {}",
                 FS::PathToUTF8String(g_applied_root), FS::PathToUTF8String(root));
    } else {
        LOG_INFO(Frontend, "libretro: Eden data root = {}", FS::PathToUTF8String(root));
    }
    g_applied_root = root;
    return true;
}

Paths GetPaths() {
    namespace FS = Common::FS;

    Paths paths{};
    if (g_applied_root.empty()) {
        paths.valid = false;
        return paths;
    }

    paths.valid = true;
    paths.root = g_applied_root;
    // Read back out of the EdenPath table rather than re-deriving root/"keys": the
    // table is what Eden itself will use, and if anything ever re-points it behind our
    // back the user must be told the truth, not our assumption.
    paths.keys_dir = FS::GetEdenPath(FS::EdenPath::KeysDir);          // path_util.h:231
    paths.keys_import_dir = g_applied_root / "keys_import";
    paths.firmware_dir =
        FS::GetEdenPath(FS::EdenPath::NANDDir) / "system" / "Contents" / "registered";
    paths.base_key_file = paths.keys_dir / BaseKeyFileName();
    return paths;
}

void AdoptKeys() {
    namespace FS = Common::FS;

    // static_cast, not void(...): `void(Qualified::Name())` on a no-argument call is
    // the vexing parse and is ill-formed inside a function body.
    // Ordering matters and is enforced here rather than documented: AdoptKeys must
    // never resolve a root that SetupUserPaths has not applied to the EdenPath table.
    static_cast<void>(SetupUserPaths());
    if (g_applied_root.empty()) {
        return;
    }

    const auto src_dir = g_applied_root / "keys_import";
    const auto dst_dir = FS::GetEdenPath(FS::EdenPath::KeysDir);
    if (FS::IsDir(src_dir)) {
        void(FS::CreateDirs(dst_dir));
        for (const char* const name : KEY_FILE_NAMES) {
            const auto src = src_dir / name;
            const auto dst = dst_dir / name;
            if (!FS::Exists(src)) {
                continue;
            }

            // The old rule was "skip if the destination exists", which silently ignored
            // a NEWER prod.keys dropped into keys_import - exactly what a user does
            // when their keys turn out to be for an older firmware generation. Copy
            // when the destination is missing, or when the source differs from it.
            // std::error_code overloads throughout: a filesystem_error escaping into
            // retro_load_game would abort the app.
            bool replace = true;
            if (FS::Exists(dst)) {
                // One error_code per query: the std::filesystem overloads CLEAR the code
                // on success, so sharing one between four calls lets a later success
                // erase an earlier failure.
                std::error_code src_size_ec;
                std::error_code dst_size_ec;
                std::error_code src_time_ec;
                std::error_code dst_time_ec;
                const auto src_size = std::filesystem::file_size(src, src_size_ec);
                const auto dst_size = std::filesystem::file_size(dst, dst_size_ec);
                const auto src_time = std::filesystem::last_write_time(src, src_time_ec);
                const auto dst_time = std::filesystem::last_write_time(dst, dst_time_ec);
                if (src_size_ec || dst_size_ec || src_time_ec || dst_time_ec) {
                    replace = true; // Could not compare: prefer the drop-off.
                } else {
                    replace = src_size != dst_size || src_time > dst_time;
                }
            }
            if (!replace) {
                continue;
            }

            std::error_code copy_ec;
            std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing,
                                       copy_ec);
            if (copy_ec) {
                LOG_ERROR(Frontend, "libretro: failed to install {}: {}", name, copy_ec.message());
            } else {
                LOG_INFO(Frontend, "libretro: installed {} into {}", name,
                         FS::PathToUTF8String(dst_dir));
            }
        }
    }

    // Core::Crypto::KeyManager is a function-local static whose CONSTRUCTOR calls
    // ReloadKeys() (src/core/crypto/key_manager.cpp:559-561). Anything that touched
    // KeyManager::Instance() before the EdenPath table was repointed cached keys read
    // from the wrong directory for the whole session, so reload unconditionally here.
    // The keys directory must also stay WRITABLE: KeyManager writes derived keys back
    // as *.keys_autogenerated into it and reads them on the next boot.
    Core::Crypto::KeyManager::Instance().ReloadKeys(); // key_manager.h:298
}

bool KeysPresent() {
    // Equivalent to frontend_common's ContentManager::AreKeysPresent()
    // (content_manager.h:379-381), reached directly so this target does not have to
    // link frontend_common. BaseDeriveNecessary (key_manager.cpp:692-710) returns true
    // - i.e. keys are NOT usable - unless the S256 header key is loaded AND, for every
    // crypto revision below CURRENT_CRYPTO_REVISION (key_manager.cpp:36, currently 5),
    // the master key, all three key-area keys (application/ocean/system) and the
    // titlekek are loaded. A prod.keys from an older firmware generation satisfies the
    // header check and fails the loop: that is the "stale keys" case, and it is why
    // "the file exists" is not the same question as "the keys work".
    return !Core::Crypto::KeyManager::Instance().BaseDeriveNecessary(); // key_manager.h:285
}

KeyInstallReport InstallKeysFrom(const std::filesystem::path& source) {
    namespace FS = Common::FS;

    KeyInstallReport report{};
    static_cast<void>(SetupUserPaths());
    const auto paths = GetPaths();
    if (!paths.valid) {
        report.result = KeyInstallResult::NoDataRoot;
        report.message = "Eden has nowhere to store its data yet, so keys cannot be installed.";
        return report;
    }
    if (!FS::Exists(source)) {
        report.result = KeyInstallResult::SourceMissing;
        report.message = "There is nothing at " + FS::PathToUTF8String(source) + ".";
        return report;
    }

    void(FS::CreateDirs(paths.keys_dir));

    // Collect candidates: a single file, or every key-named file under a folder. The
    // first match for each canonical name wins, so a folder holding both keys/prod.keys
    // and a stray copy deeper down installs the shallower one.
    std::vector<std::pair<const char*, std::filesystem::path>> candidates;
    const auto consider = [&candidates](const std::filesystem::path& file) {
        const char* const canonical = CanonicalKeyName(file.filename().string());
        if (canonical == nullptr) {
            return false;
        }
        for (const auto& already : candidates) {
            if (already.first == canonical) {
                return true;
            }
        }
        candidates.emplace_back(canonical, file);
        return true;
    };

    if (FS::IsDir(source)) {
        std::error_code walk_ec;
        std::filesystem::recursive_directory_iterator it{
            source, std::filesystem::directory_options::skip_permission_denied, walk_ec};
        const std::filesystem::recursive_directory_iterator end;
        for (; !walk_ec && it != end; it.increment(walk_ec)) {
            std::error_code kind_ec;
            if (!it->is_regular_file(kind_ec) || kind_ec) {
                continue;
            }
            // Capped: a user can point this at a folder holding thousands of files and
            // nobody reads a list that long. The message below says what IS read.
            if (!consider(it->path()) && report.ignored.size() < 8) {
                report.ignored.push_back(it->path().filename().string());
            }
        }
    } else if (!consider(source)) {
        report.ignored.push_back(source.filename().string());
    }

    if (candidates.empty()) {
        report.result = KeyInstallResult::WrongName;
        report.message =
            "That is not a key file Eden reads. It opens exactly these names, and ignores "
            "everything else without saying so: prod.keys, dev.keys, title.keys, console.keys, "
            "key_retail.bin. Pick the file itself, or the folder holding it.";
        return report;
    }

    bool any_copy_failed = false;
    for (const auto& [canonical, file] : candidates) {
        const auto destination = paths.keys_dir / canonical;
        std::error_code same_ec;
        if (std::filesystem::equivalent(file, destination, same_ec) && !same_ec) {
            report.installed.emplace_back(canonical);
            continue;
        }
        std::error_code copy_ec;
        std::filesystem::copy_file(file, destination,
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            any_copy_failed = true;
            report.ignored.push_back(std::string{canonical} + " (" + copy_ec.message() + ")");
            LOG_ERROR(Frontend, "libretro: failed to install {}: {}", canonical,
                      copy_ec.message());
            continue;
        }
        report.installed.emplace_back(canonical);
        LOG_INFO(Frontend, "libretro: installed {} into {}", canonical,
                 FS::PathToUTF8String(paths.keys_dir));
    }

    if (report.installed.empty()) {
        report.result = KeyInstallResult::CopyFailed;
        report.message = "The key files could not be copied into " +
                         FS::PathToUTF8String(paths.keys_dir) + ".";
        return report;
    }

    // Immediate, not deferred. This is the whole difference between this function and
    // the keys_import drop-off: the user sees the result now instead of at the next
    // load, and a wrong file is diagnosed while they still remember picking it.
    Core::Crypto::KeyManager::Instance().ReloadKeys(); // key_manager.h:298

    std::string installed_list;
    for (const auto& name : report.installed) {
        if (!installed_list.empty()) {
            installed_list += ", ";
        }
        installed_list += name;
    }

    if (!KeysPresent()) {
        report.result = KeyInstallResult::StillUnusable;
        report.message =
            "Installed " + installed_list +
            ", but Eden still cannot derive a complete keyring from it. The file is either "
            "truncated or from an older system version than the games you want to run. A "
            "prod.keys that opens older titles is not enough for newer ones.";
        return report;
    }

    report.result = any_copy_failed ? KeyInstallResult::CopyFailed : KeyInstallResult::Ok;
    report.message = "Installed " + installed_list + " into " +
                     FS::PathToUTF8String(paths.keys_dir) + ". Keys are complete.";
    return report;
}

FirmwareInstallReport InstallFirmwareFrom(const std::filesystem::path& source,
                                          bool replace_existing) {
    namespace FS = Common::FS;

    FirmwareInstallReport report{};
    static_cast<void>(SetupUserPaths());
    const auto paths = GetPaths();
    if (!paths.valid) {
        report.result = FirmwareInstallResult::NoDataRoot;
        report.message = "Eden has nowhere to store its data yet, so firmware cannot be installed.";
        return report;
    }
    if (!FS::Exists(source)) {
        report.result = FirmwareInstallResult::SourceMissing;
        report.message = "There is nothing at " + FS::PathToUTF8String(source) + ".";
        return report;
    }
    if (!FS::CreateDirs(paths.firmware_dir)) {
        report.result = FirmwareInstallResult::WriteFailed;
        report.message =
            "Could not create " + FS::PathToUTF8String(paths.firmware_dir) + ".";
        return report;
    }

    // Clearing first would delete the very files about to be installed if the user
    // picked the firmware directory itself - a completely reasonable thing to do from
    // Files.app, and silently destructive without this guard.
    std::error_code same_ec;
    const bool source_is_destination =
        std::filesystem::equivalent(source, paths.firmware_dir, same_ec) && !same_ec;

    std::size_t cleared = 0;
    if (replace_existing && !source_is_destination) {
        cleared = ClearFirmwareDirectory(paths.firmware_dir);
    }

    if (FS::IsDir(source)) {
        InstallFirmwareFromDirectory(source, paths.firmware_dir, report);
    } else {
        InstallFirmwareFromZip(source, paths.firmware_dir, report);
    }

    // An early failure inside the two helpers sets result and message itself.
    if (report.result == FirmwareInstallResult::Unreadable) {
        return report;
    }

    if (report.installed == 0 && report.failed == 0) {
        report.result = FirmwareInstallResult::NothingFound;
        report.message =
            "Nothing in there is a firmware file. Eden reads only files named as 32 "
            "hexadecimal characters followed by .nca or .cnmt.nca - that is what its content "
            "index matches on, so a renamed file is invisible to it. Point this at the "
            "firmware archive as downloaded, without renaming anything inside it.";
        return report;
    }
    if (report.installed == 0) {
        report.result = FirmwareInstallResult::WriteFailed;
    } else if (report.failed != 0) {
        report.result = FirmwareInstallResult::Partial;
    } else {
        report.result = FirmwareInstallResult::Ok;
    }

    report.message = "Installed " + std::to_string(report.installed) + " firmware file" +
                     (report.installed == 1 ? "" : "s") + " into " +
                     FS::PathToUTF8String(paths.firmware_dir) + ".";
    if (cleared != 0) {
        report.message += " Replaced " + std::to_string(cleared) + " existing file" +
                          (cleared == 1 ? "" : "s") + ".";
    }
    if (report.skipped != 0) {
        report.message += " Ignored " + std::to_string(report.skipped) +
                          " other file" + (report.skipped == 1 ? "" : "s") +
                          " that are not firmware.";
    }
    if (report.failed != 0) {
        report.message += " " + std::to_string(report.failed) + " failed:";
        for (const auto& problem : report.problems) {
            report.message += "\n- " + problem;
        }
    }
    report.message += "\n\nFirmware is picked up the next time a game starts.";
    return report;
}

std::size_t CountFirmwareNcas() {
    const auto paths = GetPaths();
    if (!paths.valid) {
        return 0;
    }

    std::size_t count = CountNcasIn(paths.firmware_dir);

    // Plus the bucketed layout: <registered>/000000XX/<id>.nca.
    if (Common::FS::IsDir(paths.firmware_dir)) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it{paths.firmware_dir, ec}, end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec) || ec) {
                ec.clear();
                continue;
            }
            if (FollowsTwoDigitDirFormat(it->path().filename().string())) {
                count += CountNcasIn(it->path());
            }
        }
    }
    return count;
}

bool FirmwarePresent() {
    return CountFirmwareNcas() != 0;
}

FirmwareReport VerifyFirmware() {
    FirmwareReport report{};
    report.verdict = FirmwareVerdict::NotInstalled;

    static_cast<void>(SetupUserPaths());
    const auto paths = GetPaths();
    if (!paths.valid) {
        report.verdict = FirmwareVerdict::NoDataRoot;
        report.detail = "No data root, so there is nowhere for firmware to be.";
        return report;
    }

    report.nca_count = CountFirmwareNcas();
    if (report.nca_count == 0) {
        report.verdict = FirmwareVerdict::NotInstalled;
        report.detail = "No firmware files in " +
                        Common::FS::PathToUTF8String(paths.firmware_dir) + ".";
        return report;
    }

    if (!KeysPresent()) {
        report.verdict = FirmwareVerdict::KeysMissing;
        report.detail = std::to_string(report.nca_count) +
                        " firmware files are installed, but they cannot be read without a "
                        "complete keyring. Fix the keys first; this says nothing about "
                        "whether the firmware itself is good.";
        return report;
    }

    // Everything below parses real NCA headers. FileSys::NCA and the RomFS extractor
    // are given a user-supplied file and are not written to be hostile-input-safe, so
    // a corrupt dump is contained here rather than allowed to reach a frontend.
    try {
        auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
        auto dir = vfs->OpenDirectory(Common::FS::PathToUTF8String(paths.firmware_dir),
                                      FileSys::OpenMode::Read);
        if (dir == nullptr) {
            report.verdict = FirmwareVerdict::Unreadable;
            report.detail = "Could not open " +
                            Common::FS::PathToUTF8String(paths.firmware_dir) + ".";
            return report;
        }

        // The same index Eden's own BISFactory builds over this directory
        // (bis_factory.cpp:19-20), constructed here so no Core::System is needed.
        const FileSys::RegisteredCache cache{std::move(dir)};

        // FirmwareManager::CheckFirmwarePresence's predicate, verbatim in effect:
        // Service::AM::AppletProgramId::MiiEdit is 0x0100000000001009 (am_types.h:110).
        constexpr u64 kMiiEditProgramId = static_cast<u64>(Service::AM::AppletProgramId::MiiEdit);
        report.has_system_applet =
            cache.HasEntry(kMiiEditProgramId, FileSys::ContentRecordType::Program);

        // The SystemVersion system-data title. Its RomFS holds a single 0x100-byte
        // "/file" - system_settings_server.cpp:66, :94-106.
        constexpr u64 kSystemVersionTitleId = 0x0100000000000809ULL;
        auto version_nca = cache.GetEntry(kSystemVersionTitleId, FileSys::ContentRecordType::Data);
        if (version_nca != nullptr) {
            if (auto raw_romfs = version_nca->GetRomFS(); raw_romfs != nullptr) {
                if (auto romfs = FileSys::ExtractRomFS(raw_romfs); romfs != nullptr) {
                    if (const auto version_file = romfs->GetFile("file");
                        version_file != nullptr) {
                        const auto bytes = version_file->ReadAllBytes();
                        if (bytes.size() == sizeof(FirmwareVersionRecord)) {
                            FirmwareVersionRecord record{};
                            std::memcpy(&record, bytes.data(), sizeof(record));
                            report.version_known = true;
                            report.major = record.major;
                            report.minor = record.minor;
                            report.micro = record.micro;
                            report.display_version = FromFixedString(record.display_version,
                                                                     sizeof(record.display_version));
                            report.display_title =
                                FromFixedString(record.display_title, sizeof(record.display_title));
                        }
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        report.verdict = FirmwareVerdict::Unreadable;
        report.detail = std::string{"Reading the installed firmware failed: "} + error.what();
        return report;
    } catch (...) {
        report.verdict = FirmwareVerdict::Unreadable;
        report.detail = "Reading the installed firmware failed.";
        return report;
    }

    if (!report.version_known && !report.has_system_applet) {
        report.verdict = FirmwareVerdict::Unreadable;
        report.detail =
            std::to_string(report.nca_count) +
            " firmware files are installed and none of them could be parsed. The usual cause is "
            "a partial extraction, or an archive whose files were renamed on the way in.";
        return report;
    }
    if (!report.has_system_applet || !report.version_known) {
        report.verdict = FirmwareVerdict::Incomplete;
        report.detail = std::to_string(report.nca_count) +
                        " firmware files are installed, but the set is incomplete - " +
                        (report.version_known ? "the system applets are missing"
                                              : "the system version title is missing") +
                        ". Install a full firmware archive rather than a subset.";
        return report;
    }

    // display_version is a fixed-width console field and can legitimately be blank in a
    // dump; the numeric triple always decodes, so fall back to it rather than printing
    // "Firmware  is installed".
    const std::string version_text =
        report.display_version.empty()
            ? std::to_string(report.major) + "." + std::to_string(report.minor) + "." +
                  std::to_string(report.micro)
            : report.display_version;

    if (report.major > HLE::ApiVersion::HOS_VERSION_MAJOR) {
        report.verdict = FirmwareVerdict::WrongVersion;
        report.detail = "Firmware " + version_text + " is installed, but this build " +
                        "emulates Horizon OS " + HLE::ApiVersion::DISPLAY_VERSION +
                        ". Games can misbehave when the installed firmware is newer than the "
                        "system the emulator reports. Install firmware " +
                        HLE::ApiVersion::DISPLAY_VERSION + " or older.";
        return report;
    }

    report.verdict = FirmwareVerdict::Good;
    report.detail = "Firmware " + version_text + " is installed (" +
                    std::to_string(report.nca_count) + " files). This build emulates Horizon OS " +
                    HLE::ApiVersion::DISPLAY_VERSION + ".";
    return report;
}

bool FirmwareRegistered(Core::System& system) {
    // FirmwareManager::CheckFirmwarePresence (frontend_common/firmware_manager.h:66-82)
    // asks the system-NAND RegisteredCache for the Mii Edit applet's Program NCA. The
    // same predicate, without linking frontend_common - see the header for why that is
    // an API decision and not a build one.
    //
    // HasEntry (registered_cache.cpp:716-718) is `GetEntryRaw(...) != nullptr`, and
    // FirmwareManager's GetEntry (registered_cache.cpp:744-750) returns nullptr on
    // exactly that condition, so the two agree while costing one fewer NCA parse.
    constexpr u64 kMiiEditProgramId = static_cast<u64>(Service::AM::AppletProgramId::MiiEdit);

    const auto* cache = system.GetFileSystemController().GetSystemNANDContents(); // core.h:364
    if (cache == nullptr) {
        // bis_factory is null until FileSystemController::CreateFactories runs
        // (filesystem.cpp:517-523), i.e. before the first load. Not an error; the
        // caller wanted VerifyFirmware().
        return false;
    }
    return cache->HasEntry(kMiiEditProgramId, FileSys::ContentRecordType::Program);
}

namespace {

/// The cheap half: everything that is a file-existence test or one directory scan.
Report DescribeFromDisk() {
    Report report{};

    // Resolve paths first: a status computed against a half-initialised EdenPath table
    // would name directories Eden is not using. Idempotent once a root is applied.
    static_cast<void>(SetupUserPaths());
    report.paths = GetPaths();

    if (!report.paths.valid) {
        report.status = Status::NoDataRoot;
        return report;
    }

    namespace FS = Common::FS;
    report.base_key_file_present = FS::Exists(report.paths.base_key_file);
    report.title_key_file_present = FS::Exists(report.paths.keys_dir / "title.keys");
    report.console_key_file_present = FS::Exists(report.paths.keys_dir / "console.keys");
    report.amiibo_key_file_present = FS::Exists(report.paths.keys_dir / "key_retail.bin");
    report.keys_usable = KeysPresent();

    // The one case where re-reading from disk can change the answer from wrong to
    // right: a key file is sitting there that the in-memory keyring has not been told
    // about, because the user dropped it in through Files.app after the KeyManager
    // singleton was first constructed. Costs one small file read, and only in the
    // failure state the user is actively trying to get out of.
    if (!report.keys_usable && report.base_key_file_present) {
        Core::Crypto::KeyManager::Instance().ReloadKeys(); // key_manager.h:298
        report.keys_usable = KeysPresent();
    }

    report.firmware_nca_count = CountFirmwareNcas();

    if (!report.keys_usable) {
        report.status = report.base_key_file_present ? Status::BadKeys : Status::NoKeys;
    } else if (report.firmware_nca_count == 0) {
        report.status = Status::NoFirmware;
    } else {
        report.status = Status::Ready;
    }
    return report;
}

/// Fold a firmware verification into a disk-only report.
///
/// Only a Ready report can be changed. A report that already says NoKeys, BadKeys or
/// NoFirmware describes a problem the user has to fix BEFORE the firmware question is
/// even meaningful, and replacing it with a firmware complaint would send them at the
/// wrong thing.
Report FoldFirmware(Report report, const FirmwareReport& firmware) {
    report.firmware_verified = true;
    report.firmware = firmware;
    if (report.status != Status::Ready) {
        return report;
    }

    switch (firmware.verdict) {
    case FirmwareVerdict::Good:
        report.status = Status::Ready;
        break;
    case FirmwareVerdict::WrongVersion:
        report.status = Status::FirmwareWrongVersion;
        break;
    case FirmwareVerdict::NotInstalled:
        // Reachable when the directory emptied between the count and the parse, which a
        // Files.app delete can genuinely do.
        report.status = Status::NoFirmware;
        break;
    case FirmwareVerdict::NoDataRoot:
        report.status = Status::NoDataRoot;
        break;
    case FirmwareVerdict::KeysMissing:
        report.status = Status::BadKeys;
        break;
    case FirmwareVerdict::Unreadable:
    case FirmwareVerdict::Incomplete:
        report.status = Status::FirmwareUnreadable;
        break;
    }
    return report;
}

} // namespace

Report Describe(bool verify_firmware) {
    Report report = DescribeFromDisk();
    if (!verify_firmware || report.status != Status::Ready) {
        // Nothing to verify, or a keys problem that has to be fixed first. Skipping the
        // parse here is not just an optimisation: VerifyFirmware cannot decrypt an NCA
        // header without a complete keyring, so it would spend seconds to report
        // KeysMissing, which DescribeFromDisk already knows.
        return report;
    }
    return FoldFirmware(std::move(report), VerifyFirmware());
}

Report DescribeUsing(const FirmwareReport& verified) {
    return FoldFirmware(DescribeFromDisk(), verified);
}

const char* StatusToken(Status status) {
    switch (status) {
    case Status::NoDataRoot:
        return "no_data_root";
    case Status::NoKeys:
        return "no_keys";
    case Status::BadKeys:
        return "bad_keys";
    case Status::NoFirmware:
        return "no_firmware";
    case Status::FirmwareUnreadable:
        return "firmware_unreadable";
    case Status::FirmwareWrongVersion:
        return "firmware_wrong_version";
    case Status::Ready:
        return "ready";
    }
    return "no_data_root";
}

std::string DescribeStatus(const Report& report) {
    const auto keys_path = Common::FS::PathToUTF8String(report.paths.keys_dir);
    const auto import_path = Common::FS::PathToUTF8String(report.paths.keys_import_dir);
    const auto firmware_path = Common::FS::PathToUTF8String(report.paths.firmware_dir);
    const std::string key_file{BaseKeyFileName()};

    switch (report.status) {
    case Status::NoDataRoot:
        return "Eden has nowhere to store its data. The app must call "
               "eden_libretro_set_data_root() before anything else.";
    case Status::NoKeys:
        return "No " + key_file + " found. Install your own " + key_file +
               ", or copy it into " + import_path +
               " from the Files app. Nothing encrypted can be opened without it, and Eden "
               "neither supplies keys nor can obtain them for you.";
    case Status::BadKeys:
        return key_file +
               " is there but Eden could not derive a complete keyring from it. It is usually "
               "truncated, or from an older system version than the games you want to run. "
               "Replace it in " + keys_path + " with a complete, current one.";
    case Status::NoFirmware:
        return "Keys are good. No firmware is installed - install a firmware archive, or put "
               "its .nca files in " + firmware_path +
               " yourself. Most games run without it; install it if a game asks for system "
               "files, a shared font, or a system applet.";
    case Status::FirmwareUnreadable:
        return report.firmware.detail.empty()
                   ? "Firmware files are installed but could not be read."
                   : report.firmware.detail;
    case Status::FirmwareWrongVersion:
        return report.firmware.detail.empty()
                   ? "The installed firmware is newer than this build emulates."
                   : report.firmware.detail;
    case Status::Ready:
        if (report.firmware_verified && report.firmware.version_known) {
            return report.firmware.detail;
        }
        return "Keys are good and firmware is installed (" +
               std::to_string(report.firmware_nca_count) + " files in " + firmware_path + ").";
    }
    return "Unknown content status.";
}

unsigned RetroRegion() {
    // Switch content is region free, so there is no per-ROM region to report; suyu
    // hardcodes NTSC. Eden does model the emulated console's region
    // (Settings::values.region_index, src/common/settings.h:765; enum
    // Settings::Region at src/common/settings_enums.h:126), so report that instead.
    switch (Settings::values.region_index.GetValue()) {
    case Settings::Region::Europe:
    case Settings::Region::Australia:
        return RETRO_REGION_PAL;
    default:
        return RETRO_REGION_NTSC;
    }
}

std::string DescribeLoadFailure(Core::SystemResultStatus status) {
    // Mirrors the decode Eden's own headless frontend does in src/yuzu_cmd/yuzu.cpp:
    // anything at or above ErrorLoader is ErrorLoader + (u16)Loader::ResultStatus.
    // suyu only logged the raw u32, which is how a missing prod.keys becomes an
    // opaque number instead of a sentence.
    switch (status) {
    case Core::SystemResultStatus::ErrorGetLoader:
        return "no loader could handle this file";
    case Core::SystemResultStatus::ErrorNotInitialized:
        return "CPU core not initialized";
    // ErrorSystemFiles (core.h:137) and ErrorSharedFont (core.h:138) are declared but
    // returned by nothing in src/ - a grep finds them only in core.h itself and in the
    // Android frontend's Kotlin mirror (NativeLibrary.kt:279, :382-383). Missing
    // firmware therefore does NOT surface as a distinct load status; it surfaces later,
    // as a service failure inside the guest. Mapped anyway in case that changes, but do
    // not build a firmware check on top of it - Content::VerifyFirmware() is the check.
    case Core::SystemResultStatus::ErrorSystemFiles:
        return "missing system files - install firmware";
    case Core::SystemResultStatus::ErrorSharedFont:
        return "missing shared font - install firmware";
    case Core::SystemResultStatus::ErrorVideoCore:
        return "video core failed to initialize";
    case Core::SystemResultStatus::ErrorUnknown:
        return "unknown error";
    default:
        break;
    }

    const auto loader_base = static_cast<u32>(Core::SystemResultStatus::ErrorLoader);
    const auto raw = static_cast<u32>(status);
    if (raw < loader_base) {
        return "unknown error";
    }
    switch (static_cast<Loader::ResultStatus>(raw - loader_base)) {
    // loader.h:103-113. These are the statuses that mean "the key situation is wrong",
    // as opposed to "this particular title needs a title key you do not have":
    // ErrorMissingProductionKeyFile is raised by xci.cpp:77, nsp.cpp:116 and nax.cpp:55
    // when KeyManager::KeyFileExists(false) is false at open time.
    case Loader::ResultStatus::ErrorMissingProductionKeyFile:
    case Loader::ResultStatus::ErrorMissingHeaderKey:
    case Loader::ResultStatus::ErrorIncorrectHeaderKey:
    case Loader::ResultStatus::ErrorMissingKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectKeyAreaKey:
        return "decryption keys are missing or wrong - install a current prod.keys";
    // These mean the keyring is fine but this specific title's key is absent, which is
    // a different sentence: a complete prod.keys will not fix it, title.keys might.
    case Loader::ResultStatus::ErrorMissingTitlekey:
    case Loader::ResultStatus::ErrorMissingTitlekek:
    case Loader::ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
        return "this title's key is missing or wrong - it needs a matching title.keys entry";
    default:
        return "loader error " + std::to_string(raw - loader_base);
    }
}

} // namespace LibretroCore::Content

// ===========================================================================
// The C surface the iOS app calls. Declared in src/ios/App/EdenContentBridge.h,
// which is pure C so Clang's Swift importer can read it; this file includes that same
// header, so there is exactly one declaration and the two cannot drift.
// ===========================================================================

namespace {

/// The last snapshot eden_content_snapshot() computed, so the token and detail
/// accessors describe the same moment the caller was told about rather than re-running
/// the disk scan and possibly answering about a different one.
LibretroCore::Content::Report g_last_report;
bool g_have_last_report = false;

/// The last firmware verification, remembered so a later cheap snapshot does not lose
/// it. Invalidated by the NCA COUNT changing, which is the one cheap signal that the
/// firmware directory was touched since - installing, or a delete from Files.app. It is
/// not a perfect test (same count, different files) and does not need to be: the user
/// can press the check again, and the install path clears it outright.
LibretroCore::Content::FirmwareReport g_last_firmware;
bool g_have_last_firmware = false;
std::size_t g_last_firmware_count = 0;

/// Copy `text` into a caller buffer, NUL-terminated, truncating if it does not fit.
/// Returns the length the text would have needed, excluding the terminator, so a caller
/// can detect truncation. A null buffer or zero length writes nothing.
std::size_t CopyOut(const std::string& text, char* buf, std::size_t len) {
    if (buf != nullptr && len != 0) {
        const std::size_t copied = std::min(text.size(), len - 1);
        std::memcpy(buf, text.data(), copied);
        buf[copied] = '\0';
    }
    return text.size();
}

} // namespace

extern "C" {

void eden_content_snapshot(EdenContentSnapshot* out) {
    auto report = LibretroCore::Content::Describe(false);

    // Re-apply a verification that is still current, so the headline cannot drift back
    // to "Ready" while the firmware row still says the version is wrong.
    if (g_have_last_firmware && g_last_firmware_count == report.firmware_nca_count) {
        report = LibretroCore::Content::DescribeUsing(g_last_firmware);
    }

    g_last_report = report;
    g_have_last_report = true;

    if (out == nullptr) {
        return;
    }
    *out = EdenContentSnapshot{};
    out->status = static_cast<int>(report.status);
    out->base_key_file_present = report.base_key_file_present;
    out->title_key_file_present = report.title_key_file_present;
    out->console_key_file_present = report.console_key_file_present;
    out->amiibo_key_file_present = report.amiibo_key_file_present;
    out->keys_usable = report.keys_usable;
    out->firmware_nca_count = static_cast<unsigned long long>(report.firmware_nca_count);
    out->firmware_version_known = report.firmware.version_known;
    out->firmware_major = report.firmware.major;
    out->firmware_minor = report.firmware.minor;
    out->firmware_micro = report.firmware.micro;
    out->target_major = HLE::ApiVersion::HOS_VERSION_MAJOR;
    out->target_minor = HLE::ApiVersion::HOS_VERSION_MINOR;
    out->target_micro = HLE::ApiVersion::HOS_VERSION_MICRO;
}

std::size_t eden_content_status_token(char* buf, std::size_t len) {
    if (!g_have_last_report) {
        eden_content_snapshot(nullptr);
    }
    return CopyOut(LibretroCore::Content::StatusToken(g_last_report.status), buf, len);
}

std::size_t eden_content_status_detail(char* buf, std::size_t len) {
    if (!g_have_last_report) {
        eden_content_snapshot(nullptr);
    }
    return CopyOut(LibretroCore::Content::DescribeStatus(g_last_report), buf, len);
}

std::size_t eden_content_path(EdenContentPathId which, char* buf, std::size_t len) {
    const auto paths = LibretroCore::Content::GetPaths();
    if (!paths.valid) {
        return CopyOut(std::string{}, buf, len);
    }
    switch (which) {
    case EdenContentPathRoot:
        return CopyOut(Common::FS::PathToUTF8String(paths.root), buf, len);
    case EdenContentPathKeysDir:
        return CopyOut(Common::FS::PathToUTF8String(paths.keys_dir), buf, len);
    case EdenContentPathKeysImportDir:
        return CopyOut(Common::FS::PathToUTF8String(paths.keys_import_dir), buf, len);
    case EdenContentPathFirmwareDir:
        return CopyOut(Common::FS::PathToUTF8String(paths.firmware_dir), buf, len);
    case EdenContentPathBaseKeyFile:
        return CopyOut(Common::FS::PathToUTF8String(paths.base_key_file), buf, len);
    }
    return CopyOut(std::string{}, buf, len);
}

int eden_content_install_keys(const char* source_path, char* message, std::size_t message_len) {
    if (source_path == nullptr) {
        CopyOut("No file was chosen.", message, message_len);
        return EdenKeyInstallSourceMissing;
    }
    const auto report = LibretroCore::Content::InstallKeysFrom(std::filesystem::path{source_path});
    CopyOut(report.message, message, message_len);
    return static_cast<int>(report.result);
}

int eden_content_install_firmware(const char* source_path, bool replace_existing, char* message,
                                  std::size_t message_len) {
    if (source_path == nullptr) {
        CopyOut("No file was chosen.", message, message_len);
        return EdenFirmwareInstallSourceMissing;
    }
    const auto report = LibretroCore::Content::InstallFirmwareFrom(
        std::filesystem::path{source_path}, replace_existing);
    // Whatever was verified before describes files that may no longer be there.
    g_have_last_firmware = false;
    g_last_firmware_count = 0;
    CopyOut(report.message, message, message_len);
    return static_cast<int>(report.result);
}

int eden_content_verify_firmware(EdenFirmwareCheck* out, char* version, std::size_t version_len,
                                 char* detail, std::size_t detail_len) {
    const auto report = LibretroCore::Content::VerifyFirmware();
    g_last_firmware = report;
    g_have_last_firmware = true;
    g_last_firmware_count = report.nca_count;
    if (out != nullptr) {
        *out = EdenFirmwareCheck{};
        out->verdict = static_cast<int>(report.verdict);
        out->nca_count = static_cast<unsigned long long>(report.nca_count);
        out->version_known = report.version_known;
        out->major = report.major;
        out->minor = report.minor;
        out->micro = report.micro;
        out->target_major = HLE::ApiVersion::HOS_VERSION_MAJOR;
        out->target_minor = HLE::ApiVersion::HOS_VERSION_MINOR;
        out->target_micro = HLE::ApiVersion::HOS_VERSION_MICRO;
    }
    CopyOut(report.display_version, version, version_len);
    CopyOut(report.detail, detail, detail_len);
    return static_cast<int>(report.verdict);
}

} // extern "C"
