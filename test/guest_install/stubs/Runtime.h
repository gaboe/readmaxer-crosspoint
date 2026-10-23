#pragma once

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

constexpr int ESP_OK = 0;
constexpr int ESP_MAC_WIFI_STA = 0;
constexpr int ESP_IPADDR_TYPE_V4 = 0;
struct esp_partition_t {
  uint32_t address;
  uint32_t size;
};

namespace fake {
inline std::map<std::string, std::vector<uint8_t>> files;
inline esp_partition_t running{0x10000, 0x5c0000};
inline esp_partition_t destination{0x5d0000, 0x5c0000};
inline std::vector<uint8_t> flash;
inline bool guardAllowed = true;
inline bool guardHeld = false;
inline bool pending = false;
inline bool samePartition = false;
inline bool corruptReadback = false;
inline bool failWrite = false;
inline bool failRead = false;
inline bool changeSource = false;
inline bool failReceiptClose = false;
inline bool failReceiptRename = false;
inline int erases = 0;
inline size_t erasedBytes = 0;
inline int writes = 0;
inline int selections = 0;
inline uint32_t selectedState = 0;
}  // namespace fake

class HalFile {
 public:
  std::string path;
  size_t pos = 0;
  size_t fileSize() const { return fake::files.at(path).size(); }
  uint64_t fileSize64() const { return fileSize(); }
  bool seek(size_t offset) {
    pos = offset;
    return offset <= fileSize();
  }
  int read(void* out, size_t len) {
    if (fake::failRead || pos > fileSize() || len > fileSize() - pos) return -1;
    std::memcpy(out, fake::files.at(path).data() + pos, len);
    pos += len;
    return static_cast<int>(len);
  }
  size_t write(const uint8_t* data, size_t len) {
    auto& bytes = fake::files.at(path);
    bytes.insert(bytes.end(), data, data + len);
    return len;
  }
  bool close() { return !fake::failReceiptClose; }
};

inline struct StorageStub {
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (!fake::files.contains(path)) return false;
    file.path = path;
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    fake::files[path].clear();
    file.path = path;
    return true;
  }
  bool exists(const char* path) { return fake::files.contains(path); }
  bool remove(const char* path) { return fake::files.erase(path) != 0; }
  bool rename(const char* oldPath, const char* newPath) {
    if (fake::failReceiptRename && std::strcmp(oldPath, "/readmaxer/last-install.tmp") == 0) return false;
    if (!exists(oldPath) || exists(newPath)) return false;
    fake::files[newPath] = std::move(fake::files.at(oldPath));
    fake::files.erase(oldPath);
    return true;
  }
} Storage;

inline const esp_partition_t* esp_ota_get_running_partition() { return &fake::running; }
inline const esp_partition_t* esp_ota_get_next_update_partition(const void*) {
  return fake::samePartition ? &fake::running : &fake::destination;
}
inline int esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t len) {
  EXPECT_EQ(partition, &fake::destination);
  EXPECT_TRUE(fake::guardHeld);
  EXPECT_EQ(offset % 4096, 0u);
  EXPECT_EQ(len % 4096, 0u);
  EXPECT_LE(len, 65536u);
  ++fake::erases;
  fake::erasedBytes += len;
  std::fill_n(fake::flash.begin() + offset, len, 0xff);
  return ESP_OK;
}
inline int esp_partition_write(const esp_partition_t* partition, size_t offset, const void* data, size_t len) {
  EXPECT_EQ(partition, &fake::destination);
  EXPECT_TRUE(fake::guardHeld);
  ++fake::writes;
  if (fake::failWrite) return -1;
  std::memcpy(fake::flash.data() + offset, data, len);
  return ESP_OK;
}
inline int esp_partition_read(const esp_partition_t* partition, size_t offset, void* data, size_t len) {
  EXPECT_EQ(partition, &fake::destination);
  EXPECT_TRUE(fake::guardHeld);
  if (fake::failRead) return -1;
  std::memcpy(data, fake::flash.data() + offset, len);
  if (fake::corruptReadback && len) static_cast<uint8_t*>(data)[0] ^= 1;
  return ESP_OK;
}
inline int esp_read_mac(uint8_t* bytes, int) {
  constexpr uint8_t mac[] = {0x98, 0xc3, 0x77, 0xbe, 0xa8, 0x40};
  std::memcpy(bytes, mac, sizeof(mac));
  return ESP_OK;
}

struct mbedtls_sha256_context {
  EVP_MD_CTX* ctx = nullptr;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context* sha) { sha->ctx = EVP_MD_CTX_new(); }
inline void mbedtls_sha256_free(mbedtls_sha256_context* sha) { EVP_MD_CTX_free(sha->ctx); }
inline int mbedtls_sha256_starts(mbedtls_sha256_context* sha, int) {
  return EVP_DigestInit_ex(sha->ctx, EVP_sha256(), nullptr) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* sha, const uint8_t* data, size_t len) {
  return EVP_DigestUpdate(sha->ctx, data, len) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* sha, uint8_t* out) {
  return EVP_DigestFinal_ex(sha->ctx, out, nullptr) == 1 ? 0 : -1;
}

inline void delay(unsigned) {}
inline uint32_t millis() { return 100; }
inline void resetTaskWatchdogIfSubscribed() {}
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)

inline struct MdnsStub {
  bool begin(const char*) { return true; }
  void end() {}
  void addService(const char*, const char*, int) {}
} MDNS;
inline struct WifiStub {
  uint32_t localIP() { return 0x0100007f; }
} WiFi;
struct mdns_ip_addr_t {
  struct {
    int type;
    struct {
      struct {
        uint32_t addr;
      } ip4;
    } u_addr;
  } addr;
};
inline int mdns_delegate_hostname_add(const char*, const mdns_ip_addr_t*) { return ESP_OK; }
