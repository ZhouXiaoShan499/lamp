#include "ir_store.h"

#include <LittleFS.h>

namespace irstore {
namespace {

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t freq_khz;
    uint16_t raw_len;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 12, "LIR1 header must be 12 bytes");

constexpr size_t kMaxFileSize = sizeof(Header) + kMaxRawLen * 2 + 4;  // 2064 B

void slotPath(char* out, size_t out_size, uint8_t slot, const char* ext) {
    snprintf(out, out_size, "%s/slot%02u%s", IR_DIR, static_cast<unsigned>(slot), ext);
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

const char* toString(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::NotFound: return "not_found";
        case Result::Corrupt: return "corrupt";
        case Result::OutOfRange: return "out_of_range";
        case Result::IoError: return "io_error";
    }
    return "unknown";
}

void begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("[ir] LittleFS mount failed");
        return;
    }
    if (!LittleFS.exists(IR_DIR)) LittleFS.mkdir(IR_DIR);

    // Recover any write interrupted between the two renames below.
    char target[40], tmp[40], bak[40];
    for (uint8_t slot = 0; slot < kSlots; ++slot) {
        slotPath(target, sizeof(target), slot, ".lir");
        slotPath(tmp, sizeof(tmp), slot, ".tmp");
        slotPath(bak, sizeof(bak), slot, ".bak");

        if (LittleFS.exists(tmp)) LittleFS.remove(tmp);
        if (!LittleFS.exists(target) && LittleFS.exists(bak)) {
            LittleFS.rename(bak, target);
            Serial.printf("[ir] recovered slot %u from backup\n", slot);
        } else if (LittleFS.exists(bak)) {
            LittleFS.remove(bak);
        }
    }
}

Result save(uint8_t slot, const uint16_t* raw, uint16_t len, uint16_t freq_khz) {
    if (slot >= kSlots) return Result::OutOfRange;
    if (raw == nullptr) return Result::OutOfRange;
    if (len < kMinRawLen || len > kMaxRawLen) return Result::OutOfRange;

    static uint8_t buffer[kMaxFileSize];
    Header header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.freq_khz = freq_khz;
    header.raw_len = len;
    header.reserved = 0;

    const size_t payload_bytes = static_cast<size_t>(len) * 2;
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), raw, payload_bytes);
    const size_t body_bytes = sizeof(header) + payload_bytes;
    const uint32_t crc = crc32(buffer, body_bytes);
    memcpy(buffer + body_bytes, &crc, sizeof(crc));
    const size_t total = body_bytes + sizeof(crc);

    char target[40], tmp[40], bak[40];
    slotPath(target, sizeof(target), slot, ".lir");
    slotPath(tmp, sizeof(tmp), slot, ".tmp");
    slotPath(bak, sizeof(bak), slot, ".bak");

    File file = LittleFS.open(tmp, FILE_WRITE);
    if (!file) return Result::IoError;
    const size_t written = file.write(buffer, total);
    file.close();
    if (written != total) {
        LittleFS.remove(tmp);
        return Result::IoError;
    }

    // Two-step commit so an interrupted save never leaves a half-written
    // record where a reader would expect a valid one.
    if (LittleFS.exists(bak)) LittleFS.remove(bak);
    if (LittleFS.exists(target) && !LittleFS.rename(target, bak)) {
        LittleFS.remove(tmp);
        return Result::IoError;
    }
    if (!LittleFS.rename(tmp, target)) {
        if (LittleFS.exists(bak)) LittleFS.rename(bak, target);  // roll back
        LittleFS.remove(tmp);
        return Result::IoError;
    }
    if (LittleFS.exists(bak)) LittleFS.remove(bak);
    return Result::Ok;
}

Result load(uint8_t slot, Record& out) {
    out.len = 0;
    if (slot >= kSlots) return Result::OutOfRange;

    char target[40];
    slotPath(target, sizeof(target), slot, ".lir");
    if (!LittleFS.exists(target)) return Result::NotFound;

    File file = LittleFS.open(target, FILE_READ);
    if (!file) return Result::IoError;
    const size_t size = file.size();
    if (size < sizeof(Header) + 4 || size > kMaxFileSize) {
        file.close();
        return Result::Corrupt;
    }

    static uint8_t buffer[kMaxFileSize];
    const size_t read_bytes = file.read(buffer, size);
    file.close();
    if (read_bytes != size) return Result::IoError;

    Header header{};
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != kMagic || header.version != kVersion) return Result::Corrupt;
    if (header.raw_len < kMinRawLen || header.raw_len > kMaxRawLen) return Result::Corrupt;

    const size_t payload_bytes = static_cast<size_t>(header.raw_len) * 2;
    const size_t expected = sizeof(Header) + payload_bytes + 4;
    if (size != expected) return Result::Corrupt;

    uint32_t stored_crc = 0;
    memcpy(&stored_crc, buffer + sizeof(Header) + payload_bytes, sizeof(stored_crc));
    if (crc32(buffer, sizeof(Header) + payload_bytes) != stored_crc) return Result::Corrupt;

    memcpy(out.raw, buffer + sizeof(Header), payload_bytes);
    out.len = header.raw_len;
    out.freq_khz = header.freq_khz == 0 ? 38 : header.freq_khz;
    return Result::Ok;
}

bool exists(uint8_t slot) {
    if (slot >= kSlots) return false;
    char target[40];
    slotPath(target, sizeof(target), slot, ".lir");
    return LittleFS.exists(target);
}

Result erase(uint8_t slot) {
    if (slot >= kSlots) return Result::OutOfRange;
    char target[40];
    slotPath(target, sizeof(target), slot, ".lir");
    if (!LittleFS.exists(target)) return Result::NotFound;
    return LittleFS.remove(target) ? Result::Ok : Result::IoError;
}

uint8_t usedSlots() {
    uint8_t count = 0;
    for (uint8_t slot = 0; slot < kSlots; ++slot) {
        if (exists(slot)) ++count;
    }
    return count;
}

}  // namespace irstore