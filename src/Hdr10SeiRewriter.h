#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Hdr10Sei {
struct Result {
    bool changed = false;
    unsigned originalMasterNits = 0;
    unsigned effectiveMasterNits = 0;
    unsigned maxCLL = 0;
};

struct NalUnit {
    size_t start = 0;
    size_t payload = 0;
    size_t end = 0;
    size_t prefix = 0;
};

inline uint32_t Read32(const uint8_t* ptr) {
    return (uint32_t(ptr[0]) << 24) | (uint32_t(ptr[1]) << 16) |
        (uint32_t(ptr[2]) << 8) | uint32_t(ptr[3]);
}
inline void Write32(uint8_t* ptr, uint32_t value) {
    ptr[0] = uint8_t(value >> 24);
    ptr[1] = uint8_t(value >> 16);
    ptr[2] = uint8_t(value >> 8);
    ptr[3] = uint8_t(value);
}
inline size_t StartCode(const uint8_t* bytes, size_t size, size_t offset, size_t& length) {
    for (size_t i = offset; i + 3 <= size; ++i) {
        if (i + 4 <= size && bytes[i] == 0 && bytes[i+1] == 0 &&
            bytes[i+2] == 0 && bytes[i+3] == 1) { length = 4; return i; }
        if (bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1) {
            length = 3; return i;
        }
    }
    length = 0;
    return size;
}
inline bool Split(const uint8_t* bytes, size_t size, std::vector<NalUnit>& nals, bool& annexB) {
    if (!bytes || size < 6) return false;
    size_t firstLen = 0;
    size_t first = StartCode(bytes, size, 0, firstLen);
    annexB = first <= 4 && firstLen > 0;
    if (annexB) {
        for (size_t pos = first; pos < size;) {
            size_t startLen = 0;
            size_t start = StartCode(bytes, size, pos, startLen);
            if (start == size) break;
            size_t payload = start + startLen;
            size_t ignored = 0;
            size_t end = StartCode(bytes, size, payload, ignored);
            if (end < payload + 2) return false;
            nals.push_back({start,payload,end,startLen});
            pos = end;
        }
    } else {
        for (size_t pos = 0; pos + 4 <= size;) {
            const uint32_t length = Read32(bytes + pos);
            if (length < 2 || length > size - (pos + 4)) return false;
            nals.push_back({pos,pos + 4,pos + 4 + length,4});
            pos += 4 + length;
        }
        if (nals.empty() || nals.back().end != size) return false;
    }
    return !nals.empty();
}
inline std::vector<uint8_t> Unescape(const uint8_t* nal, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    int zeroCount = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = nal[i];
        if (i >= 2 && zeroCount == 2 && b == 3 && i + 1 < size && nal[i+1] <= 3) {
            zeroCount = 0;
            continue;
        }
        out.push_back(b);
        zeroCount = b == 0 ? std::min(2, zeroCount+1) : 0;
    }
    return out;
}
inline std::vector<uint8_t> Escape(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    out.reserve(rbsp.size()+8);
    int zeros = 0;
    for (size_t i = 0; i < rbsp.size(); ++i) {
        const uint8_t b = rbsp[i];
        if (i >= 2 && zeros == 2 && b <= 3) { out.push_back(3); zeros = 0; }
        out.push_back(b);
        zeros = b == 0 ? std::min(2,zeros+1) : 0;
    }
    return out;
}
struct SeiFields {
    uint32_t maxMasterUnits = 0;
    uint16_t maxCLL = 0;
    bool hasMaster = false;
    bool hasCLL = false;
};
inline bool ParseSei(const std::vector<uint8_t>& rbsp, SeiFields& fields, size_t* masterOffset=nullptr) {
    if (rbsp.size() < 4) return false;
    size_t p = 2;
    while (p + 2 <= rbsp.size()) {
        if (p == rbsp.size()-1 && rbsp[p] == 0x80) break;
        unsigned type = 0;
        while (p < rbsp.size() && rbsp[p] == 0xff) { type += 255; ++p; }
        if (p >= rbsp.size()) return false;
        type += rbsp[p++];
        size_t length = 0;
        while (p < rbsp.size() && rbsp[p] == 0xff) { length += 255; ++p; }
        if (p >= rbsp.size()) return false;
        length += rbsp[p++];
        if (length > rbsp.size()-p) return false;
        if (type == 137 && length >= 24) {
            fields.maxMasterUnits = Read32(rbsp.data()+p+16);
            fields.hasMaster = true;
            if (masterOffset) *masterOffset = p+16;
        } else if (type == 144 && length >= 4) {
            fields.maxCLL = uint16_t((uint16_t(rbsp[p])<<8) | rbsp[p+1]);
            fields.hasCLL = true;
        }
        p += length;
    }
    return true;
}
inline bool Rewrite(const uint8_t* bytes, size_t size, unsigned displayPeakNits,
                    std::vector<uint8_t>& output, Result& result) {
    output.clear(); result = {};
    if (!bytes || size < 6 || displayPeakNits < 80 || displayPeakNits > 10000)
        return false;
    std::vector<NalUnit> nals;
    bool annexB = false;
    if (!Split(bytes, size, nals, annexB)) return false;
    SeiFields fields;
    struct MasterNal { size_t index; size_t offset; std::vector<uint8_t> rbsp; };
    std::vector<MasterNal> masters;
    for (size_t index=0; index<nals.size(); ++index) {
        const auto& nal = nals[index];
        const uint8_t* p = bytes + nal.payload;
        const uint8_t type = (p[0] >> 1) & 0x3f;
        if (type == 62 || type == 63) return false;
        if (type != 39 && type != 40) continue;
        if (nal.end-nal.payload > 131072) continue;
        auto rbsp = Unescape(p,nal.end-nal.payload);
        size_t offset = 0;
        SeiFields parsed;
        if (!ParseSei(rbsp,parsed,&offset)) return false;
        if (parsed.hasMaster) {
            fields.maxMasterUnits = parsed.maxMasterUnits;
            fields.hasMaster = true;
            masters.push_back({index,offset,std::move(rbsp)});
        }
        if (parsed.hasCLL) { fields.maxCLL = parsed.maxCLL; fields.hasCLL = true; }
    }
    if (!fields.hasMaster || !fields.hasCLL || masters.empty() ||
        !fields.maxMasterUnits || !fields.maxCLL) return false;
    const uint32_t sourceMaster = fields.maxMasterUnits/10000;
    if (sourceMaster <= displayPeakNits || fields.maxCLL < sourceMaster)
        return false;
    const uint32_t effectiveUnits = displayPeakNits * 10000u;
    if (effectiveUnits >= fields.maxMasterUnits) return false;
    for (auto& master : masters) {
        if (master.offset+4 > master.rbsp.size()) return false;
        Write32(master.rbsp.data()+master.offset,effectiveUnits);
    }
    output.reserve(size+16);
    size_t cursor = 0;
    for (size_t index=0; index<nals.size(); ++index) {
        const auto& nal=nals[index];
        if (nal.start > cursor) output.insert(output.end(),bytes+cursor,bytes+nal.start);
        const MasterNal* patched=nullptr;
        for (const auto& master:masters) if(master.index==index) {patched=&master; break;}
        if (!patched) {
            output.insert(output.end(),bytes+nal.start,bytes+nal.end);
        } else {
            const auto escaped=Escape(patched->rbsp);
            if (annexB) output.insert(output.end(),bytes+nal.start,bytes+nal.payload);
            else {
                if (escaped.size()>UINT32_MAX) return false;
                uint8_t prefix[4]; Write32(prefix,uint32_t(escaped.size()));
                output.insert(output.end(),prefix,prefix+4);
            }
            output.insert(output.end(),escaped.begin(),escaped.end());
        }
        cursor=nal.end;
    }
    if (cursor < size) output.insert(output.end(),bytes+cursor,bytes+size);
    result.changed = true;
    result.originalMasterNits = sourceMaster;
    result.effectiveMasterNits = displayPeakNits;
    result.maxCLL = fields.maxCLL;
    return true;
}
}
