#include "OtaBootSwitch.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <spi_flash_mmap.h>

#include <atomic>
#include <cstring>

#include "GuestSlot.h"

#if FREEINK_DEVICE_X4PRO
// initArduino() marks a PENDING_VERIFY image valid before setup() unless this
// returns true. confirmRunningSlot() does it once Home has rendered. Other
// boards keep the weak default.
extern "C" bool verifyRollbackLater() { return true; }
#endif

namespace ota_boot {

namespace {

std::atomic<bool> pendingConfirmation{false};
std::atomic<bool> runningConfirmed{false};
bool selectedOtherSlot = false;
int64_t imageCheckUs = -1;
std::atomic_flag writing = ATOMIC_FLAG_INIT;

class WriteGuard {
 public:
  WriteGuard() : acquired(!writing.test_and_set()) {}
  ~WriteGuard() {
    if (acquired) writing.clear();
  }
  explicit operator bool() const { return acquired; }

 private:
  bool acquired;
};

int otaSlotOf(const esp_partition_t* partition) {
  if (!partition || partition->type != ESP_PARTITION_TYPE_APP) return -1;
  const int slot = static_cast<int>(partition->subtype) - static_cast<int>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  return slot >= 0 && slot < static_cast<int>(kSlotCount) ? slot : -1;
}

const esp_partition_t* findOtadata() {
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || otadata->size < kEntrySectors * SPI_FLASH_SEC_SIZE) return nullptr;
  return otadata;
}

bool readEntries(const esp_partition_t* otadata, SelectEntry (&entries)[kEntrySectors]) {
  for (uint32_t i = 0; i < kEntrySectors; ++i) {
    if (esp_partition_read(otadata, i * SPI_FLASH_SEC_SIZE, &entries[i], sizeof(SelectEntry)) != ESP_OK) return false;
  }
  return true;
}

constexpr WritePolicy policy = FREEINK_DEVICE_X4PRO ? WritePolicy::PreserveBootable : WritePolicy::Legacy;

BootableSlots currentBootableSlots() {
  BootableSlots images{};
#if FREEINK_DEVICE_X4PRO
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    const auto* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot), nullptr);
    images.slots[slot] = guest_slot::bootableNonGuest(part);
  }
#endif
  return images;
}

SelectionResult readSelection(const esp_partition_t* otadata, uint32_t slot, uint32_t state) {
  SelectEntry stored[kEntrySectors];
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (!readEntries(otadata, stored)) continue;
    for (auto& entry : stored) {
      if (entry.ota_state == kOtaImgPendingVerify) entry.ota_state = kOtaImgAborted;
    }
    const int active = activeEntry(stored);
    return active >= 0 && entrySlot(stored[active]) == slot && stored[active].ota_state == state
               ? SelectionResult::Selected
               : SelectionResult::Failed;
  }
  LOG_ERR("BOOT", "otadata selection unknown after write");
  return SelectionResult::Unknown;
}

SelectionResult writeEntry(const esp_partition_t* otadata, const BootableSlots& images, const EntryWrite& write,
                           uint32_t slot, uint32_t state) {
  SelectEntry current[kEntrySectors];
  if (!readEntries(otadata, current) || !writeIsSafe(current, images, write, slot, state, policy)) {
    LOG_ERR("BOOT", "unsafe otadata write refused");
    return SelectionResult::Failed;
  }
  const size_t offset = static_cast<size_t>(write.sector) * SPI_FLASH_SEC_SIZE;
  if (esp_partition_erase_range(otadata, offset, SPI_FLASH_SEC_SIZE) == ESP_OK) {
    // Even a failed write can have reached flash. Classify stored state below.
    esp_partition_write(otadata, offset, &write.entry, sizeof(write.entry));
  }
  return readSelection(otadata, slot, state);
}

enum class Selection { Select, KeepRunning };

SelectionResult updateSelection(const esp_partition_t* dest, const uint32_t state, Selection selection) {
  WriteGuard guard;
  if (!guard) {
    LOG_ERR("BOOT", "otadata writer busy");
    return SelectionResult::Failed;
  }
  const int slot = otaSlotOf(dest);
  if (slot < 0) {
    LOG_ERR("BOOT", "dest is not an OTA app slot");
    return SelectionResult::Failed;
  }
  const int running = otaSlotOf(esp_ota_get_running_partition());
  if (running < 0) return SelectionResult::Failed;
  const esp_partition_t* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  if (!otadata || !readEntries(otadata, entries)) {
    LOG_ERR("BOOT", "otadata missing or unreadable");
    return SelectionResult::Failed;
  }
  const auto images = currentBootableSlots();
  EntryWrite write{};
  switch (selection) {
    case Selection::Select: {
      const int active = activeEntry(entries);
      if (active >= 0 && entrySlot(entries[active]) == static_cast<uint32_t>(slot) &&
          entries[active].ota_state == state && (state == kOtaImgNew || state == kOtaImgValid)) {
        runningConfirmed = slot == running && state == kOtaImgValid;
        return SelectionResult::Selected;
      }
      if (!planSelect(entries, images, static_cast<uint32_t>(slot), state, write, policy)) {
        LOG_ERR("BOOT", "no safe otadata write for slot %d", slot);
        return SelectionResult::Failed;
      }
      break;
    }
    case Selection::KeepRunning:
      switch (planKeepRunningSlot(entries, images, static_cast<uint32_t>(running), write)) {
        case KeepPlan::NoWrite:
          return SelectionResult::Selected;
        case KeepPlan::Unsafe:
          LOG_ERR("BOOT", "no safe otadata write to keep running slot");
          return SelectionResult::Failed;
        case KeepPlan::Write:
          break;
      }
      break;
  }
  const auto result = writeEntry(otadata, images, write, slot, state);
  if (result != SelectionResult::Selected) return result;
  runningConfirmed = dest == esp_ota_get_running_partition() && state == kOtaImgValid;
  LOG_INF("BOOT", "otadata: wrote sector=%u seq=%u state=%u -> %s", static_cast<unsigned>(write.sector),
          static_cast<unsigned>(write.entry.ota_seq), static_cast<unsigned>(state), dest->label);
  return SelectionResult::Selected;
}

}  // namespace

SelectionResult switchTo(const esp_partition_t* dest, const uint32_t state) {
  return updateSelection(dest, state, Selection::Select);
}

AppWriteGuard::AppWriteGuard(const esp_partition_t* dest) {
  if (writing.test_and_set()) return;
  const int destination = otaSlotOf(dest);
  acquired = destination >= 0;
#if FREEINK_DEVICE_X4PRO
  const auto images = currentBootableSlots();
  const auto* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  acquired = acquired && otadata && readEntries(otadata, entries) && appEraseIsSafe(entries, images, destination);
#endif
  if (!acquired) {
    writing.clear();
    LOG_ERR("BOOT", "App erase refused: confirm the running firmware first");
  }
}

AppWriteGuard::~AppWriteGuard() {
  if (acquired) writing.clear();
}

void armBootFallback() {
#if FREEINK_DEVICE_X4PRO
  const int running = otaSlotOf(esp_ota_get_running_partition());
  const esp_partition_t* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  if (running < 0 || !otadata || !readEntries(otadata, entries)) return;

  const auto runningSlot = static_cast<uint32_t>(running);
  const BootPlan plan = planBoot(entries, runningSlot, esp_reset_reason() == ESP_RST_DEEPSLEEP);
  runningConfirmed = checkRunningSlot(entries, runningSlot) == SlotCheck::Confirmed;
  pendingConfirmation = plan.confirmAfterHome;
  if (!plan.armFallback) return;

  const auto otherSlot = (runningSlot + 1) % kSlotCount;
  const esp_partition_t* other = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + otherSlot),
      nullptr);
  const int64_t started = esp_timer_get_time();
  const bool holds = guest_slot::checkAndCache(other);
  imageCheckUs = esp_timer_get_time() - started;
  if (holds) {
    selectedOtherSlot = switchTo(other, kOtaImgValid) == SelectionResult::Selected;
  }
#endif
}

SelectionResult keepRunningSlot() {
  if (!pendingConfirmation) return SelectionResult::Selected;
  const auto result = updateSelection(esp_ota_get_running_partition(), kOtaImgNew, Selection::KeepRunning);
  if (result == SelectionResult::Selected) selectedOtherSlot = false;
  return result;
}

bool confirmRunningSlot() {
  if (!pendingConfirmation) return true;
  if (switchTo(esp_ota_get_running_partition(), kOtaImgValid) != SelectionResult::Selected) {
    LOG_ERR("BOOT", "confirm running slot failed");
    return false;
  }
  pendingConfirmation = false;
  LOG_INF("BOOT", "running slot confirmed (fallback was %s, other-slot image check %lld us)",
          selectedOtherSlot ? "armed" : "not armed", static_cast<long long>(imageCheckUs));
  return true;
}

bool confirmationPending() { return pendingConfirmation; }

bool runningSlotConfirmed() { return runningConfirmed; }

SlotCheck runningSlotCheck() {
  const int running = otaSlotOf(esp_ota_get_running_partition());
  const esp_partition_t* otadata = findOtadata();
  SelectEntry entries[kEntrySectors];
  if (running < 0 || !otadata || !readEntries(otadata, entries)) return SlotCheck::Unconfirmed;
  return checkRunningSlot(entries, static_cast<uint32_t>(running));
}

}  // namespace ota_boot
