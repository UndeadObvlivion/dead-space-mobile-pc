// Reader/patcher for the game's "SBIN" binary databases (im::serialization::Database files such as
// published/layoutdata/layouts.xml.sb and published/stringdata/<lang>/deadspace.bin).
//
// Layout of a file: "SBIN", u32 version, then chunks {tag[4], u32 size, u32 FNV-1 hash, data}
// padded to 4 bytes. STRU: struct defs (u16 name+1, u16 first field, u16 count). FIEL: field defs
// (u16 name+1, u16 type, u16 offset, u16 extra). OHDR: u32 per object, low 3 bits = kind
// (0 struct, 1 unstructured, 2 array), value >> 3 = offset into DATA. DATA: object bodies
// (kind 0: u16 struct index + data; kind 2: u16 element type, u16 struct, u32 count + data).
// STRS: u32 count + (u32 length, bytes) strings; string fields store index + 1.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sbin {

struct Field {
    std::string name;
    uint16_t type, offset;
};

class Database {
public:
    bool parse(const std::vector<uint8_t>& file);
    std::vector<uint8_t> serialize() const;

    // strings (indices are the 1-based values stored in string fields)
    uint16_t find_string(const std::string& s) const;
    uint16_t add_string(const std::string& s);
    const std::string& string(uint16_t idx1) const { return strs_[idx1 - 1]; }

    int find_struct(const std::string& name) const;
    std::vector<Field> fields(int struct_idx) const;
    int field_offset(int struct_idx, const std::string& field) const;
    int struct_size(int struct_idx) const;

    size_t object_count() const { return ohdr_.size(); }
    int kind(size_t obj) const { return int(ohdr_[obj] & 7); }
    // pointer to an object's data (after its header)
    uint8_t* body(size_t obj);
    uint16_t struct_of(size_t obj);  // kind 0
    // arrays (kind 2)
    uint32_t array_count(size_t obj);
    uint16_t array_elem_type(size_t obj);
    uint16_t array_struct(size_t obj);
    uint32_t array_stride(size_t obj);

    // Appends a struct object; returns its index.
    size_t add_struct_object(uint16_t struct_idx, const std::vector<uint8_t>& data);
    // Appends an array; returns its index.
    size_t add_array(uint16_t elem_type, uint16_t struct_idx, uint32_t count, const std::vector<uint8_t>& data);
    // Points object `obj` at the most recently added object's body (used to grow arrays in place
    // without changing the object index everyone refers to). Removes the added header.
    void replace_with_last(size_t obj);

private:
    struct Chunk {
        std::string tag;
        std::vector<uint8_t> data;
    };
    std::vector<Chunk> chunks_;
    uint32_t version_ = 2;
    std::vector<std::string> strs_;
    std::vector<uint32_t> ohdr_;
    std::vector<uint8_t> data_;
    struct StructDef {
        uint16_t name1, first, count;
    };
    std::vector<StructDef> structs_;
    struct FieldDef {
        uint16_t name1, type, offset, extra;
    };
    std::vector<FieldDef> fields_;
};

}  // namespace sbin
