#include "sbin.h"

#include <algorithm>
#include <cstring>

namespace sbin {

namespace {

uint32_t fnv1(const uint8_t* p, size_t n) {
    uint32_t h = 0x811c9dc5;
    for (size_t i = 0; i < n; i++) h = (h * 0x01000193u) ^ p[i];
    return h;
}

template <class T> T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T> void wr(std::vector<uint8_t>& v, T x) {
    uint8_t b[sizeof x];
    std::memcpy(b, &x, sizeof x);
    v.insert(v.end(), b, b + sizeof x);
}

int type_size(uint16_t t) {
    switch (t) {
    case 1: case 2: case 9: return 1;
    case 3: case 4: case 12: case 13: case 20: return 2;
    case 7: case 8: case 11: return 8;
    default: return 4;
    }
}

}  // namespace

bool Database::parse(const std::vector<uint8_t>& f) {
    if (f.size() < 8 || std::memcmp(f.data(), "SBIN", 4) != 0) return false;
    version_ = rd<uint32_t>(&f[4]);
    size_t o = 8;
    while (o + 12 <= f.size()) {
        Chunk c;
        c.tag.assign(reinterpret_cast<const char*>(&f[o]), 4);
        uint32_t size = rd<uint32_t>(&f[o + 4]);
        if (o + 12 + size > f.size()) return false;
        c.data.assign(f.begin() + long(o + 12), f.begin() + long(o + 12 + size));
        chunks_.push_back(std::move(c));
        o = (o + 12 + size + 3) & ~size_t(3);
    }
    for (auto& c : chunks_) {
        const uint8_t* p = c.data.data();
        if (c.tag == "STRS") {
            uint32_t n = rd<uint32_t>(p);
            size_t q = 4;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ln = rd<uint32_t>(p + q);
                strs_.emplace_back(reinterpret_cast<const char*>(p + q + 4), ln);
                q += 4 + ln;
            }
        } else if (c.tag == "STRU") {
            for (size_t i = 0; i + 6 <= c.data.size(); i += 6)
                structs_.push_back({rd<uint16_t>(p + i), rd<uint16_t>(p + i + 2), rd<uint16_t>(p + i + 4)});
        } else if (c.tag == "FIEL") {
            for (size_t i = 0; i + 8 <= c.data.size(); i += 8)
                fields_.push_back({rd<uint16_t>(p + i), rd<uint16_t>(p + i + 2), rd<uint16_t>(p + i + 4),
                                   rd<uint16_t>(p + i + 6)});
        } else if (c.tag == "OHDR") {
            for (size_t i = 0; i + 4 <= c.data.size(); i += 4) ohdr_.push_back(rd<uint32_t>(p + i));
        } else if (c.tag == "DATA") {
            data_ = c.data;
        }
    }
    return !ohdr_.empty() && !strs_.empty();
}

std::vector<uint8_t> Database::serialize() const {
    std::vector<uint8_t> out = {'S', 'B', 'I', 'N'};
    wr(out, version_);
    for (auto& c : chunks_) {
        std::vector<uint8_t> d;
        if (c.tag == "STRS") {
            wr(d, uint32_t(strs_.size()));
            for (auto& s : strs_) {
                wr(d, uint32_t(s.size()));
                d.insert(d.end(), s.begin(), s.end());
            }
        } else if (c.tag == "OHDR") {
            for (uint32_t h : ohdr_) wr(d, h);
        } else if (c.tag == "DATA") {
            d = data_;
        } else {
            d = c.data;
        }
        out.insert(out.end(), c.tag.begin(), c.tag.end());
        wr(out, uint32_t(d.size()));
        wr(out, fnv1(d.data(), d.size()));
        out.insert(out.end(), d.begin(), d.end());
        while (out.size() & 3) out.push_back(0);
    }
    return out;
}

uint16_t Database::find_string(const std::string& s) const {
    for (size_t i = 0; i < strs_.size(); i++)
        if (strs_[i] == s) return uint16_t(i + 1);
    return 0;
}

uint16_t Database::add_string(const std::string& s) {
    if (uint16_t i = find_string(s)) return i;
    strs_.push_back(s);
    return uint16_t(strs_.size());
}

int Database::find_struct(const std::string& name) const {
    for (size_t i = 0; i < structs_.size(); i++)
        if (strs_[structs_[i].name1 - 1] == name) return int(i);
    return -1;
}

std::vector<Field> Database::fields(int si) const {
    std::vector<Field> r;
    const StructDef& s = structs_[size_t(si)];
    for (int i = s.first; i < s.first + s.count; i++)
        r.push_back({strs_[fields_[size_t(i)].name1 - 1], fields_[size_t(i)].type, fields_[size_t(i)].offset});
    return r;
}

int Database::field_offset(int si, const std::string& name) const {
    for (auto& f : fields(si))
        if (f.name == name) return f.offset;
    return -1;
}

int Database::struct_size(int si) const {
    int sz = 0;
    for (auto& f : fields(si)) sz = std::max(sz, f.offset + type_size(f.type));
    return (sz + 3) & ~3;
}

uint8_t* Database::body(size_t obj) {
    static const int hdr[3] = {2, 4, 8};
    uint32_t h = ohdr_[obj];
    return &data_[(h >> 3) + size_t(hdr[std::min<uint32_t>(h & 7, 2)])];
}

uint16_t Database::struct_of(size_t obj) { return rd<uint16_t>(&data_[ohdr_[obj] >> 3]); }
uint16_t Database::array_elem_type(size_t obj) { return rd<uint16_t>(&data_[ohdr_[obj] >> 3]); }
uint16_t Database::array_struct(size_t obj) { return rd<uint16_t>(&data_[(ohdr_[obj] >> 3) + 2]); }
uint32_t Database::array_count(size_t obj) { return rd<uint32_t>(&data_[(ohdr_[obj] >> 3) + 4]); }
uint32_t Database::array_stride(size_t obj) {
    uint16_t et = array_elem_type(obj);
    return et == 16 ? uint32_t(struct_size(array_struct(obj))) : uint32_t(type_size(et));
}

size_t Database::add_struct_object(uint16_t si, const std::vector<uint8_t>& data) {
    while ((data_.size() + 2) & 3) data_.push_back(0);  // body must be 4-aligned
    uint32_t off = uint32_t(data_.size());
    wr(data_, si);
    data_.insert(data_.end(), data.begin(), data.end());
    ohdr_.push_back(off << 3 | 0);
    return ohdr_.size() - 1;
}

size_t Database::add_array(uint16_t et, uint16_t si, uint32_t count, const std::vector<uint8_t>& data) {
    while (data_.size() & 3) data_.push_back(0);
    uint32_t off = uint32_t(data_.size());
    wr(data_, et);
    wr(data_, si);
    wr(data_, count);
    data_.insert(data_.end(), data.begin(), data.end());
    ohdr_.push_back(off << 3 | 2);
    return ohdr_.size() - 1;
}

void Database::replace_with_last(size_t obj) {
    ohdr_[obj] = ohdr_.back();
    ohdr_.pop_back();
}

}  // namespace sbin
