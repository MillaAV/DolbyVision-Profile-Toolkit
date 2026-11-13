#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <utility>
#include <limits>

namespace dvprobe {

struct Result {
    bool dolbyVision = false;
    unsigned profile = 0;
    unsigned configurationRecords = 0;
    unsigned rpuUnits = 0;
    unsigned enhancementUnits = 0;
    std::size_t scannedBytes = 0;
    std::vector<unsigned char> firstRpu;
};

namespace detail {

inline std::uint32_t Be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

inline bool ReadVint(const unsigned char* data, std::size_t size, std::size_t at,
                     std::uint64_t& value, std::size_t& length, bool elementId = false) {
    if (at >= size) return false;
    const unsigned char initial = data[at];
    unsigned mask = 0x80;
    length = 1;
    while (length <= 8 && !(initial & mask)) {
        mask >>= 1;
        ++length;
    }
    if (length > (elementId ? 4u : 8u) || length > size - at) return false;
    value = elementId ? initial : static_cast<unsigned>(initial & (mask - 1u));
    for (std::size_t i = 1; i < length; ++i)
        value = (value << 8) | data[at + i];
    return true;
}

inline bool EbmlElement(const unsigned char* p, std::size_t size, std::size_t& pos,
                        std::uint32_t& id, std::size_t& begin, std::size_t& end,
                        bool& unknown) {
    std::uint64_t identifier = 0, encodedSize = 0;
    std::size_t idLength = 0, sizeLength = 0;
    if (!ReadVint(p, size, pos, identifier, idLength, true)) return false;
    if (!ReadVint(p, size, pos + idLength, encodedSize, sizeLength)) return false;
    begin = pos + idLength + sizeLength;
    if (begin > size) return false;
    const std::uint64_t maxValue = (std::uint64_t(1) << (7 * sizeLength)) - 1;
    unknown = encodedSize == maxValue;
    if (unknown) end = size;
    else {
        if (encodedSize > size - begin) {
            if (identifier != 0x18538067 && identifier != 0x1F43B675 &&
                identifier != 0x1654AE6B) return false;
            end = size;
            unknown = true;
        } else end = begin + static_cast<std::size_t>(encodedSize);
    }
    id = static_cast<std::uint32_t>(identifier);
    pos = end;
    return true;
}

inline void ScanConfig(const unsigned char* bytes, std::size_t size, Result& result) {
    if (!bytes || size < 12) return;
    const std::size_t limit = size < 1024 * 1024 ? size : 1024 * 1024;
    for (std::size_t i = 4; i + 8 < limit; ++i) {
        const unsigned char* p = bytes + i;
        if (std::memcmp(p, "dvcC", 4) != 0 &&
            std::memcmp(p, "dvvC", 4) != 0 &&
            std::memcmp(p, "dvwC", 4) != 0) continue;
        const std::uint32_t boxSize = Be32(p - 4);
        const bool boxed = boxSize >= 12 && boxSize <= 512 &&
                           static_cast<std::size_t>(boxSize) <= limit - (i - 4);
        if (!boxed) continue;
        const unsigned char* config = p + 4;
        if (config[0] != 1 || config[1] > 2) continue;
        const unsigned profile = config[2] >> 1;
        if (profile != 5 && profile != 7 && profile != 8 && profile != 9) continue;
        ++result.configurationRecords;
        if (!result.profile) result.profile = profile;
    }
}

inline bool IsHevcHeader(const unsigned char* nal, std::size_t size) {
    return size >= 3 && (nal[0] & 0x80u) == 0 && (nal[1] & 7u) != 0;
}

inline void ScanNal(const unsigned char* nal, std::size_t size, Result& result) {
    if (!IsHevcHeader(nal, size)) return;
    const unsigned type = (nal[0] >> 1) & 63u;
    if (type == 62 && size >= 8) {
        ++result.rpuUnits;
        if (result.firstRpu.empty() && size <= 32768)
            result.firstRpu.assign(nal, nal + size);
    } else if (type == 63 && size >= 4) {
        ++result.enhancementUnits;
    }
}

inline bool ScanLengthPrefixed(const unsigned char* p, std::size_t n, unsigned lengthBytes,
                               Result& result) {
    if (lengthBytes < 1 || lengthBytes > 4) return false;
    std::size_t offset = 0;
    unsigned units = 0;
    Result observed{};
    while (offset < n) {
        if (n - offset < lengthBytes) return false;
        std::uint32_t length = 0;
        for (unsigned i = 0; i < lengthBytes; ++i) length = (length << 8) | p[offset + i];
        offset += lengthBytes;
        if (length < 3 || length > n - offset || !IsHevcHeader(p + offset, length)) return false;
        ScanNal(p + offset, length, observed);
        offset += length;
        if (++units >= 4096) return false;
    }
    if (units == 0) return false;
    result.rpuUnits += observed.rpuUnits;
    result.enhancementUnits += observed.enhancementUnits;
    if (result.firstRpu.empty()) result.firstRpu = std::move(observed.firstRpu);
    return true;
}

inline bool ScanAnnexB(const unsigned char* p, std::size_t n, Result& result) {
    std::size_t pos = 0, previous = 0;
    unsigned count = 0;
    Result observed{};
    while (pos + 3 < n) {
        const std::size_t startCode = p[pos] == 0 && p[pos + 1] == 0 && p[pos + 2] == 1
            ? 3 : (p[pos] == 0 && p[pos + 1] == 0 && p[pos + 2] == 0 && p[pos + 3] == 1 ? 4 : 0);
        if (!startCode) { ++pos; continue; }
        if (count && pos > previous) ScanNal(p + previous, pos - previous, observed);
        previous = pos + startCode;
        ++count;
        pos = previous;
    }
    if (!count) return false;
    if (previous < n) ScanNal(p + previous, n - previous, observed);
    if (count < 2) return false;
    result.rpuUnits += observed.rpuUnits;
    result.enhancementUnits += observed.enhancementUnits;
    if (result.firstRpu.empty()) result.firstRpu = std::move(observed.firstRpu);
    return true;
}

struct MatroskaTrack {
    std::uint64_t number = 0;
    bool video = false;
    bool hevc = false;
    unsigned lengthBytes = 4;
};

inline void ReadTrack(const unsigned char* p, std::size_t n, std::size_t start,
                      std::size_t end, std::vector<MatroskaTrack>& tracks) {
    MatroskaTrack track{};
    for (std::size_t pos = start; pos < end;) {
        std::uint32_t id = 0;
        std::size_t begin = 0, finish = 0;
        bool unknown = false;
        if (!EbmlElement(p, n, pos, id, begin, finish, unknown) || finish > end) break;
        const std::size_t length = finish - begin;
        if (id == 0xD7 && length && length <= 8) {
            track.number = 0;
            for (std::size_t i = begin; i < finish; ++i) track.number = (track.number << 8) | p[i];
        } else if (id == 0x83 && length == 1) {
            track.video = p[begin] == 1;
        } else if (id == 0x86 && length == 16) {
            track.hevc = std::memcmp(p + begin, "V_MPEGH/ISO/HEVC", 16) == 0;
        } else if (id == 0x63A2 && length >= 22 && p[begin] == 1) {
            track.lengthBytes = (p[begin + 21] & 3u) + 1u;
        }
    }
    if (track.number && track.video && track.hevc) tracks.push_back(track);
}

inline void ReadTracks(const unsigned char* p, std::size_t n, std::size_t start,
                       std::size_t end, std::vector<MatroskaTrack>& tracks) {
    for (std::size_t pos = start; pos < end;) {
        std::uint32_t id = 0;
        std::size_t begin = 0, finish = 0;
        bool unknown = false;
        if (!EbmlElement(p, n, pos, id, begin, finish, unknown) || finish > end) break;
        if (id == 0xAE) ReadTrack(p, n, begin, finish, tracks);
    }
}

inline void ReadBlock(const unsigned char* p, std::size_t n, std::size_t start,
                      std::size_t end, const std::vector<MatroskaTrack>& tracks,
                      Result& result) {
    if (end - start < 9) return;
    std::uint64_t trackId = 0;
    std::size_t bytes = 0;
    if (!ReadVint(p, n, start, trackId, bytes)) return;
    const MatroskaTrack* track = nullptr;
    for (const auto& item : tracks) if (item.number == trackId) { track = &item; break; }
    if (!track || start + bytes + 3 > end) return;
    const std::size_t payload = start + bytes + 3;
    const unsigned char flags = p[start + bytes + 2];
    if ((flags & 0x06u) != 0 || payload >= end) return;
    if (!ScanLengthPrefixed(p + payload, end - payload, track->lengthBytes, result))
        ScanAnnexB(p + payload, end - payload, result);
}

inline void ReadCluster(const unsigned char* p, std::size_t n, std::size_t start,
                        std::size_t end, const std::vector<MatroskaTrack>& tracks,
                        Result& result) {
    for (std::size_t pos = start; pos < end;) {
        std::uint32_t id = 0;
        std::size_t begin = 0, finish = 0;
        bool unknown = false;
        if (!EbmlElement(p, n, pos, id, begin, finish, unknown) || finish > end) break;
        if (id == 0xA3) ReadBlock(p, n, begin, finish, tracks, result);
        if (id == 0xA0) {
            for (std::size_t inner = begin; inner < finish;) {
                std::uint32_t partId = 0;
                std::size_t partBegin = 0, partEnd = 0;
                bool partUnknown = false;
                if (!EbmlElement(p, n, inner, partId, partBegin, partEnd, partUnknown) ||
                    partEnd > finish) break;
                if (partId == 0xA1) ReadBlock(p, n, partBegin, partEnd, tracks, result);
            }
        }
        if (result.rpuUnits >= 24 && result.enhancementUnits >= 16) break;
    }
}

inline bool ScanMatroska(const unsigned char* data, std::size_t size, Result& result) {
    if (size < 16 || std::memcmp(data, "\x1a\x45\xdf\xa3", 4) != 0) return false;
    std::vector<MatroskaTrack> tracks;
    for (std::size_t pos = 0; pos < size;) {
        std::uint32_t id = 0;
        std::size_t begin = 0, finish = 0;
        bool unknown = false;
        if (!EbmlElement(data, size, pos, id, begin, finish, unknown)) break;
        if (id != 0x18538067) continue;
        for (std::size_t segmentPos = begin; segmentPos < finish;) {
            std::uint32_t childId = 0;
            std::size_t childBegin = 0, childEnd = 0;
            bool childUnknown = false;
            if (!EbmlElement(data, size, segmentPos, childId, childBegin, childEnd, childUnknown) ||
                childEnd > finish) break;
            if (childId == 0x1654AE6B) ReadTracks(data, size, childBegin, childEnd, tracks);
            if (childId == 0x1F43B675 && !tracks.empty())
                ReadCluster(data, size, childBegin, childEnd, tracks, result);
            if (result.rpuUnits >= 24 && result.enhancementUnits >= 16) break;
        }
        return true;
    }
    return true;
}

}

inline Result Inspect(const unsigned char* bytes, std::size_t size) {
    Result result{};
    result.scannedBytes = size;
    if (!bytes || size < 12) return result;
    detail::ScanConfig(bytes, size, result);
    detail::ScanMatroska(bytes, size, result);
    if (!result.profile && result.rpuUnits >= 2 && result.enhancementUnits >= 1)
        result.profile = 7;
    result.dolbyVision = result.configurationRecords != 0 || result.rpuUnits >= 2;
    return result;
}

}
