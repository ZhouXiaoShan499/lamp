#pragma once
// LIR1 raw-IR record store on LittleFS.
//
// Record layout (little endian, identical to tools/mvp_calc.py selftest):
//   magic     u32  'LIR1' (0x4C495231)
//   version   u16  1
//   freq_khz  u16  38
//   raw_len   u16  20..1024
//   reserved  u16  0
//   raw[]     u16  mark/space durations in microseconds
//   crc32     u32  CRC-32 (zlib polynomial) over header + payload
//
// Why a file instead of an NVS blob: a full air-conditioner state frame is
// ~600 entries == ~1.2 KB. Sixteen of those would fragment/overflow NVS,
// and NVS blobs are not designed for 2 KB payloads. LittleFS handles it and
// gives us atomic rename() for crash safety.

#include <Arduino.h>

#include "config.h"

namespace irstore {

constexpr uint16_t kMaxRawLen = IR_CAPTURE_BUFFER;  // 1024 entries
constexpr uint8_t kSlots = 16;
constexpr uint16_t kMinRawLen = IR_MIN_RAW_LEN;
constexpr uint32_t kMagic = 0x4C495231UL;  // 'LIR1'
constexpr uint16_t kVersion = 1;

enum class Result : uint8_t {
    Ok = 0,
    NotFound,
    Corrupt,
    OutOfRange,
    IoError,
};

// Returned by value from load(); ~2 KB, so callers should keep one static
// instance rather than allocating on the stack inside a loop.
struct Record {
    uint16_t freq_khz;
    uint16_t len;
    uint16_t raw[kMaxRawLen];
};

// Mounts LittleFS, creates directories and recovers interrupted writes.
void begin();

// Persists a raw capture. Rejects length outside [kMinRawLen, kMaxRawLen].
Result save(uint8_t slot, const uint16_t* raw, uint16_t len, uint16_t freq_khz = 38);

// Reads and CRC-checks a slot. `out.len` is 0 when the result is not Ok.
Result load(uint8_t slot, Record& out);

bool exists(uint8_t slot);
Result erase(uint8_t slot);
uint8_t usedSlots();

const char* toString(Result result);

// Shared with the host-side self test in tools/mvp_calc.py.
uint32_t crc32(const uint8_t* data, size_t len);

}  // namespace irstore