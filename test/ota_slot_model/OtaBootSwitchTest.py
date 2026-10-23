"""Exercise the production otadata writer against flash with injected failures."""
import pathlib
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
headers = {
    "BoardConfig.h": "#ifndef FREEINK_DEVICE_X4PRO\n#define FREEINK_DEVICE_X4PRO 1\n#endif\n#define FREEINK_DEVICE_X4CLASSIC (!FREEINK_DEVICE_X4PRO)\n",
    "Logging.h": "#define LOG_ERR(...) ((void)0)\n#define LOG_INF(...) ((void)0)\n",
    "esp_partition.h": r"""
#pragma once
#include <cstddef>
#include <cstdint>
using esp_partition_subtype_t = int;
constexpr int ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_APP_OTA_0 = 16, ESP_PARTITION_SUBTYPE_DATA_OTA = 0;
constexpr int ESP_OK = 0;
struct esp_partition_t { int type; int subtype; size_t size; const char* label; };
const esp_partition_t* esp_partition_find_first(int, int, const char*);
int esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
int esp_partition_write(const esp_partition_t*, size_t, const void*, size_t);
int esp_partition_erase_range(const esp_partition_t*, size_t, size_t);
""",
    "esp_ota_ops.h": '#include "esp_partition.h"\nconst esp_partition_t* esp_ota_get_running_partition();\n',
    "esp_system.h": "constexpr int ESP_RST_DEEPSLEEP = 1;\nint esp_reset_reason();\n",
    "esp_timer.h": "#include <cstdint>\ninline int64_t esp_timer_get_time() { return 0; }\n",
    "spi_flash_mmap.h": "constexpr unsigned SPI_FLASH_SEC_SIZE = 4096;\n",
}
sha_source = (root / "test/ota_slot_model/OtaSlotModelTest.cpp").read_text()
sha_class = sha_source[sha_source.index("class Sha256 {"):sha_source.index("std::vector<uint8_t> appImage(")]
headers["mbedtls/sha256.h"] = "#pragma once\n#include <cstring>\n#include <cstdint>\n" + sha_class + r"""
using mbedtls_sha256_context = Sha256;
inline void mbedtls_sha256_init(Sha256*) {}
inline void mbedtls_sha256_free(Sha256*) {}
inline int mbedtls_sha256_starts(Sha256* s, int) { s->reset(); return 0; }
inline int mbedtls_sha256_update(Sha256* s, const uint8_t* d, size_t n) { s->update(d,n); return 0; }
inline int mbedtls_sha256_finish(Sha256* s, uint8_t (&d)[32]) { s->finish(d); return 0; }
"""
headers["esp_heap_caps.h"] = r"""
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
"""

def image(marker):
    header = bytearray(24)
    header[0:2] = bytes([0xe9, 2])
    header[12] = 9
    header[23] = 1
    payload = bytearray(256)
    struct.pack_into("<I", payload, 0, 0xabcd5432)
    payload[16:22] = b"1.2.3\0"
    payload[48:69] = b"readmaxer-xteink-x4pro"
    payload += b"CROSSPOINT-BOARD-V1:x4pro;" + marker
    payload += bytes(65536)
    payload += b"\0" * (-len(payload) % 4)
    result = header + struct.pack("<II", 0x3c000020, len(payload)) + payload
    checksum = 0xef
    for byte in payload:
        checksum ^= byte
    result += struct.pack("<II", 0x42000000 + (len(result) + 8) % 65536, 4) + bytes(4)
    result += bytes(16 - len(result) % 16)
    result[-1] = checksum
    return result + hashlib.sha256(result).digest()

harness = r"""
#include <cassert>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
#include "BoardConfig.h"
#include "OtaBootSwitch.h"
#include "GuestSlot.h"
#include "FirmwareFlasher.h"
#include "FirmwareBoardTag.h"
#include "FirmwareHostTag.h"
#include "GuestSlotModel.h"
#include "mbedtls/sha256.h"
#include <memory>
#include <algorithm>
using namespace ota_boot;
SelectEntry data[2];
esp_partition_t apps[] = {{0,16,0x5c0000,"ota_0"},{0,17,0x5c0000,"ota_1"}};
esp_partition_t otadata{1,0,8192,"otadata"};
unsigned running, erases, writes;
bool otherFork, failWrite, failReadback, reenter, failOnce, lostWriteAck;
std::vector<uint8_t> cp1Image, cp2Image, guestImage, appBytes[2];
int reason;
unsigned appErases=0;
bool cutDuringErase=false, cutOtadata=false;
struct PowerCut {};
std::vector<uint8_t> sdImage;
SelectEntry e(uint32_t seq, uint32_t state) {
  SelectEntry result{};
  result.ota_seq=seq; result.ota_state=state; result.crc=computeSeqCrc(seq);
  return result;
}
void reset(unsigned slot, bool fork, SelectEntry a, SelectEntry b) {
  running=slot; otherFork=fork; data[0]=a; data[1]=b;
  appBytes[slot]=cp2Image; appBytes[1-slot]=fork ? cp1Image : guestImage;
  guest_slot::invalidateCache();
  erases=writes=0; failWrite=failReadback=reenter=failOnce=lostWriteAck=false; reason=1;
  armBootFallback();
}
bool validFork() {
  for (auto& v : data) if (v.ota_seq != UINT32_MAX && v.crc == computeSeqCrc(v.ota_seq) &&
      v.ota_state == kOtaImgValid && (appBytes[entrySlot(v)]==cp1Image || appBytes[entrySlot(v)]==cp2Image)) return true;
  return false;
}
const esp_partition_t* esp_partition_find_first(int type, int subtype, const char*) {
  return type==1 ? &otadata : &apps[subtype-16];
}
const esp_partition_t* esp_ota_get_running_partition() { return &apps[running]; }
int esp_reset_reason() { return reason; }
int esp_partition_read(const esp_partition_t* part, size_t offset, void* dst, size_t len) {
  if (part->type==0) {
    const auto& bytes=appBytes[part->subtype-16];
    if (offset+len>bytes.size()) return -1;
    std::memcpy(dst, bytes.data()+offset,len); return ESP_OK;
  }
  if (failOnce && writes) { failOnce=false; return -1; }
  if (failReadback && writes) return -1;
  std::memcpy(dst, &data[offset/4096], len); return ESP_OK;
}
int esp_partition_erase_range(const esp_partition_t* part, size_t offset, size_t len) {
  if (part->type==0) {
    ++appErases;
    auto& bytes=appBytes[part->subtype-16];
    bytes.resize(std::max(bytes.size(),offset+len),0xff);
    std::fill(bytes.begin()+offset,bytes.begin()+offset+len,0xff);
    assert(!FREEINK_DEVICE_X4PRO || validFork());
    if (cutDuringErase) throw PowerCut{};
    return ESP_OK;
  }
  if (reenter) {
    reenter=false;
    assert(switchTo(&apps[1-running])==SelectionResult::Failed);
  }
  ++erases; std::memset(&data[offset/4096], 0xff, sizeof(SelectEntry));
  assert(!FREEINK_DEVICE_X4PRO || validFork());
  if (cutOtadata) throw PowerCut{};
  return ESP_OK;
}
int esp_partition_write(const esp_partition_t* part, size_t offset, const void* src, size_t len) {
  if (part->type==0) {
    auto& bytes=appBytes[part->subtype-16];
    assert(offset+len<=bytes.size());
    std::memcpy(bytes.data()+offset,src,len);
    assert(!FREEINK_DEVICE_X4PRO || validFork());
    return ESP_OK;
  }
  ++writes;
  if (failWrite) return -1;
  std::memcpy(&data[offset/4096], src, len); assert(!FREEINK_DEVICE_X4PRO || validFork()); return lostWriteAck ? -1 : ESP_OK;
}
const esp_partition_t* esp_ota_get_next_update_partition(const void*) { return &apps[1-running]; }
void delay(int) {}
struct HalFile {
  size_t pos=0;
  explicit operator bool() const { return true; }
  size_t fileSize() const { return sdImage.size(); }
  int read(uint8_t* dst, size_t len) {
    if (pos+len>sdImage.size()) return -1;
    std::memcpy(dst,sdImage.data()+pos,len); pos+=len; return len;
  }
  bool seek(size_t offset) { pos=offset; return pos<=sdImage.size(); }
  void close() {}
};
struct StorageStub { bool openFileForRead(const char*,const char*,HalFile&) { return true; } } Storage;
template <typename T> std::unique_ptr<T> makeUniqueNoThrow(size_t n) {
  return std::unique_ptr<T>(new (std::nothrow) std::remove_extent_t<T>[n]());
}
namespace firmware_flash {
constexpr size_t MIN_FIRMWARE_SIZE=65536;
constexpr size_t CHUNK=4096, BLK=65536, SEC=4096, SHA_TRAILER=32;
VALIDATE_FUNCTION
const char* resultName(Result) { return "fake"; }
bool hashFile(const char*,size_t,uint8_t*,uint8_t (&out)[32]) { std::memset(out,0,32); return true; }
bool hashPartition(const esp_partition_t*,size_t,uint8_t*,uint8_t (&out)[32]) { std::memset(out,0,32); return true; }
FLASH_FUNCTION
}
namespace firmware_flash { uint16_t runningPartitionChipId() { return 9; } }
std::vector<uint8_t> load(const char* path) {
  std::ifstream file(path,std::ios::binary); assert(file.good());
  return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
SLEEP_FUNCTIONS
void seal(std::vector<uint8_t>& bytes) {
  size_t pos=24; uint8_t checksum=0xef;
  for (unsigned i=0;i<bytes[1];++i) {
    uint32_t length; std::memcpy(&length,bytes.data()+pos+4,4); pos+=8;
    for (size_t j=0;j<length;++j) checksum ^= bytes[pos+j];
    pos+=length;
  }
  bytes[bytes.size()-33]=checksum;
  Sha256 sha; uint8_t digest[32]; sha.update(bytes.data(),bytes.size()-32); sha.finish(digest);
  std::memcpy(bytes.data()+bytes.size()-32,digest,32);
}
int main(int argc, char** argv) {
  assert(argc==4 || argc==5);
  cp1Image=load(argv[1]); cp2Image=load(argv[2]); guestImage=load(argv[3]);
#if FREEINK_DEVICE_X4PRO
  if (argc==5) {
    auto probe=load(argv[4]);
    Sha256 sha; uint8_t buffer[64];
    const ImageHasher hasher{&sha,
      [](void* c,const uint8_t* b,uint32_t n) { static_cast<Sha256*>(c)->update(b,n); return true; },
      [](void* c,uint8_t (&d)[32]) { static_cast<Sha256*>(c)->finish(d); return true; }};
    const auto read=[](uint32_t offset,void* dst,uint32_t len,void* ctx) {
      const auto& bytes=*static_cast<std::vector<uint8_t>*>(ctx);
      if(offset>bytes.size() || len>bytes.size()-offset) return false;
      std::memcpy(dst,bytes.data()+offset,len); return true;
    };
    assert(checkAppImage(read,&probe,probe.size(),9,hasher,buffer,sizeof(buffer))==ImageCheck::Ok);
  }
  sdImage=cp2Image;
  assert(firmware_flash::validateImageFile("firmware",apps[0].size)==firmware_flash::Result::OK);
  for (const char* marker : {"READMAXER-HOST-V1;", "READMAXER-GUEST-V1;"}) {
    sdImage=cp2Image; std::memcpy(sdImage.data()+96,marker,std::strlen(marker)); seal(sdImage);
    assert(firmware_flash::validateImageFile("firmware",apps[0].size)==firmware_flash::Result::WRONG_FORK);
  }

  for (unsigned slot=0; slot<2; ++slot) {
    for (unsigned sector=0; sector<2; ++sector) {
      for (bool cut : {false, true}) {
        SelectEntry values[2];
        values[sector]=e(3+slot,kOtaImgValid);
        values[1-sector]=e(5+slot,kOtaImgPendingVerify);
        reset(slot,true,values[0],values[1]);
        auto malformed=cp1Image;
        uint32_t address;
        std::memcpy(&address,malformed.data()+24,4); address+=4;
        std::memcpy(malformed.data()+24,&address,4);
        Sha256 hash; uint8_t digest[32];
        hash.update(malformed.data(),malformed.size()-32); hash.finish(digest);
        std::memcpy(malformed.data()+malformed.size()-32,digest,32);
        appBytes[1-slot]=malformed; guest_slot::invalidateCache();
        reason=0; armBootFallback();
        assert(erases==0 && !guest_slot::bootableNonGuest(&apps[1-slot]));
        sdImage=malformed;
        assert(firmware_flash::validateImageFile("firmware",apps[0].size)==firmware_flash::Result::BAD_SEGMENTS);
        prepared=slept=0; cutOtadata=cut;
        try { sleepNow(); assert(!cut && slept==1 && prepared==1); }
        catch (const PowerCut&) { assert(cut && slept==0 && prepared==0); }
        cutOtadata=false;
        assert(erases==1 && data[sector].ota_seq==3+slot && validFork());
        // Cold boot with rollback enabled only has the preserved VALID fork after a cut.
        for (auto& entry : data) if (entry.ota_state==kOtaImgPendingVerify) entry.ota_state=kOtaImgAborted;
        assert(activeEntry(data)>=0 && entrySlot(data[activeEntry(data)])==slot);
      }
    }
  }
  for (unsigned slot=0; slot<2; ++slot) {
    for (unsigned sector=0; sector<2; ++sector) {
      for (unsigned state : {kOtaImgNew, kOtaImgPendingVerify}) {
        SelectEntry values[2];
        values[sector]=e(3+slot,state); values[1-sector]=e(2+slot,kOtaImgValid);
        reset(slot,true,values[0],values[1]); reason=0;
        armBootFallback();
        assert(erases==1 && data[1-sector].ota_seq==2+slot);
        assert(confirmRunningSlot());
        assert(runningSlotConfirmed() && !confirmationPending());
        unsigned count=erases;
        assert(confirmRunningSlot() && erases==count);
      }
    }
  }
  reset(0,false,e(3,kOtaImgValid),e(4,kOtaImgValid));
  assert(keepRunningSlot()==SelectionResult::Selected && data[0].ota_seq==3 && data[1].ota_state==kOtaImgNew);
  assert(keepRunningSlot()==SelectionResult::Selected && erases==1);
  assert(confirmRunningSlot());
  reset(0,false,e(0xfffffffd,kOtaImgPendingVerify),e(0xfffffffe,kOtaImgValid));
  assert(keepRunningSlot()==SelectionResult::Failed && erases==0);
  assert(!confirmRunningSlot() && confirmationPending() && erases==0);
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  failWrite=true;
  assert(switchTo(&apps[1])==SelectionResult::Failed && erases==1 && validFork());
  failWrite=false;
  assert(switchTo(&apps[1])==SelectionResult::Selected && validFork());
  unsigned count=erases;
  assert(switchTo(&apps[1])==SelectionResult::Selected && erases==count);
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  reenter=true;
  assert(switchTo(&apps[1])==SelectionResult::Selected && erases==1);
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  failReadback=true;
  assert(switchTo(&apps[1])==SelectionResult::Unknown && erases==1 && validFork());
  assert(activeEntry(data)==1 && data[1].ota_state==kOtaImgNew);
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  failOnce=true;
  assert(switchTo(&apps[1])==SelectionResult::Selected);
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  lostWriteAck=true;
  assert(switchTo(&apps[1])==SelectionResult::Selected);

  // CP1/shop has no host marker. CP2 boots unconfirmed in ota_1.
  for (unsigned state : {kOtaImgNew,kOtaImgPendingVerify}) {
    reset(1,true,e(3,kOtaImgValid),e(4,state));
    assert(guest_slot::bootableNonGuest(&apps[0]));
    reason=0; armBootFallback();
    assert(entrySlot(data[activeEntry(data)])==0);
    assert(keepRunningSlot()==SelectionResult::Selected);
    { AppWriteGuard recovery(&apps[0]); assert(!recovery); }
    sdImage=cp2Image; appErases=0;
    assert(firmware_flash::flashFromSdPath("firmware",nullptr,nullptr)==firmware_flash::Result::CONFIRM_REQUIRED);
    assert(appErases==0);
    assert(confirmRunningSlot());
    cutDuringErase=true;
    try { firmware_flash::flashFromSdPath("firmware",nullptr,nullptr); assert(false); }
    catch (const PowerCut&) { assert(appErases==1 && validFork()); }
    cutDuringErase=false;
    assert(firmware_flash::flashFromSdPath("firmware",nullptr,nullptr)==firmware_flash::Result::OK);
    assert(validFork());
    {
      AppWriteGuard replacement(&apps[0]); assert(replacement);
      assert(switchTo(&apps[0])==SelectionResult::Failed);
      // First destination erase and immediate power cut leave a sound VALID CP2.
      appBytes[0].assign(appBytes[0].size(),0xff);
      assert(validFork());
    }
    appBytes[0]=cp2Image;
    assert(switchTo(&apps[0])==SelectionResult::Selected);
    running=0; reason=0; armBootFallback();
    assert(confirmRunningSlot() && runningSlotConfirmed());
  }
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  { AppWriteGuard good(&apps[1]); assert(good); }
  appBytes[0][40]^=1;
  { AppWriteGuard damaged(&apps[1]); assert(!damaged); }
  assert(switchTo(&apps[1])==SelectionResult::Failed);
  appBytes[0]=guestImage;
  { AppWriteGuard guest(&apps[1]); assert(!guest); }
#endif
  reset(0,false,e(3,kOtaImgValid),e(2,kOtaImgAborted));
  sdImage=cp2Image; failReadback=true;
  assert(firmware_flash::flashFromSdPath("firmware",nullptr,nullptr)==firmware_flash::Result::OTADATA_UNKNOWN);
  assert(entrySlot(data[activeEntry(data)])==1);
  SelectEntry blank; std::memset(&blank,0xff,sizeof(blank));
  reset(0,true,blank,blank);
  assert(switchTo(&apps[1]) == (FREEINK_DEVICE_X4PRO ? SelectionResult::Failed : SelectionResult::Selected));
  { AppWriteGuard replacement(&apps[1]); assert(bool(replacement)==!FREEINK_DEVICE_X4PRO); }
}
"""
flasher = (root / "src/network/FirmwareFlasher.cpp").read_text()
flash_function = flasher[flasher.index("Result flashFromSdPath("):flasher.rindex("}  // namespace firmware_flash")]
validate_function = flasher[flasher.index("Result validateImageFile("):flasher.index("\nnamespace {\n// SHA-256")]
harness = harness.replace("FLASH_FUNCTION", flash_function).replace("VALIDATE_FUNCTION", validate_function)
sleep_test = (root / "test/ota_slot_model/SleepGateTest.py").read_text()
sleep_stubs = sleep_test.split('program = r"""', 1)[1].split('""" + functions', 1)[0]
sleep_stubs = sleep_stubs[:sleep_stubs.index("namespace ota_boot {")] + sleep_stubs[sleep_stubs.index("namespace HalPowerManager"):]
sleep_stubs = sleep_stubs.replace("struct StorageStub {", "struct SleepStorageStub {").replace("} Storage;", "} SleepStorage;")
main_source = (root / "src/main.cpp").read_text()
sleep_functions = main_source[main_source.index("static void sleepPrepared("):main_source.index("void setupDisplayAndFonts(")]
harness = harness.replace("SLEEP_FUNCTIONS", sleep_stubs + sleep_functions.replace("Storage.", "SleepStorage."))
with tempfile.TemporaryDirectory(prefix="cp2-writer-") as tmp:
    directory = pathlib.Path(tmp)
    for name, content in headers.items():
        (directory / name).parent.mkdir(parents=True,exist_ok=True)
        (directory / name).write_text(content)
    cpp = directory / "writer.cpp"
    cpp.write_text('#include <initializer_list>\n#include "Logging.h"\n' + harness)
    fixtures = []
    for name, marker in [("cp1", b""), ("cp2", b"READMAXER-HOST-V1;"), ("guest", b"READMAXER-GUEST-V1;")]:
        path = directory / (name + ".bin")
        path.write_bytes(image(marker))
        fixtures.append(str(path))
    for board in [1, 0]:
        if not board:
            for path in fixtures:
                content = bytearray(pathlib.Path(path).read_bytes())
                content = content.replace(b"CROSSPOINT-BOARD-V1:x4pro;", b"CROSSPOINT-BOARD-V1:x4c;\0\0")
                checksum = 0xef
                length = struct.unpack_from("<I", content, 28)[0]
                for byte in content[32:32+length]:
                    checksum ^= byte
                content[-33] = checksum
                content[-32:] = hashlib.sha256(content[:-32]).digest()
                pathlib.Path(path).write_bytes(content)
        exe = directory / ("writer-" + str(board))
        subprocess.run([sys.argv[2], "-std=c++17", "-O2", "-DFREEINK_DEVICE_X4PRO=" + str(board),
                        "-I" + str(directory), "-I" + str(root / "src/network"), str(cpp),
                        *[str(root / "src/network" / file) for file in ["OtaBootSwitch.cpp", "OtaSlotModel.cpp",
                            "GuestSlot.cpp", "GuestSlotModel.cpp", "FirmwareHostTag.cpp", "FirmwareBoardTag.cpp"]], "-o", str(exe)], check=True)
        subprocess.run([str(exe), *fixtures], check=True)
        if board and os.environ.get("CP2_CP1_IMAGE") and os.environ.get("CP2_FORK_IMAGE"):
            subprocess.run([str(exe), os.environ["CP2_CP1_IMAGE"], os.environ["CP2_FORK_IMAGE"], fixtures[2],
                            os.environ.get("CP2_PROBE_IMAGE", fixtures[2])], check=True)
            print("Actual CP1 and CP2 passed migration and malformed-image erase cuts. Recorded RM1 probe passed the shared image check.")
print("Production writers, image checks, app erase, migration, readback outcomes and both board policies passed.")
