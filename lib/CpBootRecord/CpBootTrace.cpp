#include "CpBootTrace.h"

#if FREEINK_DEVICE_X4PRO
#include <driver/sdmmc_host.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <sdmmc_cmd.h>

namespace cp_boot {
namespace {
RTC_NOINIT_ATTR volatile uint8_t retained[Record::Bytes];
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
bool recordingStorage = false;
bool drew = false;
size_t attempts = 0;
bool sectorPending = false;

Record load() {
  Record::Encoded bytes;
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = retained[i];
  Record record;
  record.decode(bytes);
  return record;
}
void save(const Record& record) {
  const auto& bytes = record.encode();
  for (size_t i = 0; i < bytes.size(); ++i) retained[i] = bytes[i];
  __sync_synchronize();
}
void mark(Stage stage) {
  auto record = load();
  record.stage(stage);
  save(record);
}
}  // namespace

void begin(uint8_t reason) {
  auto record = load();
  record.begin(reason);
  save(record);
}
void storageBegin() {
  recordingStorage = true;
  attempts = 0;
  sectorPending = false;
  mark(Stage::StorageBegin);
}
void sdResult(Stage stage, int32_t error) {
  if (!recordingStorage) return;
  auto record = load();
  record.attempt(attempts ? attempts - 1 : 0, stage, error);
  save(record);
}
void cardBegin() {
  if (!recordingStorage) return;
  if (attempts < Record::Attempts) ++attempts;
  sectorPending = true;
  sdResult(Stage::CardInit, Pending);
}
bool sectorBegin(size_t sector, size_t count) {
  if (!recordingStorage || !sectorPending || sector != 0 || count != 1) return false;
  sectorPending = false;
  sdResult(Stage::SectorZero, Pending);
  return true;
}
void sectorEnd(int32_t error) {
  sdResult(Stage::SectorZero, error);
  if (error == ESP_OK) mark(Stage::VolumeMount);
}
void storageEnd(bool ok) {
  auto record = load();
  if (!ok && record.lastStage(0) == Stage::VolumeMount) {
    record.attempt(attempts ? attempts - 1 : 0, Stage::VolumeMount, ESP_FAIL);
  }
  record.stage(ok ? Stage::StorageEnd : Stage::StorageFailed);
  save(record);
  recordingStorage = false;
}
void panelInit() { mark(Stage::PanelInit); }
void firstDraw() {
  portENTER_CRITICAL(&lock);
  if (!drew) {
    mark(Stage::FirstDraw);
    drew = true;
  }
  portEXIT_CRITICAL(&lock);
}
Record snapshot() {
  portENTER_CRITICAL(&lock);
  auto record = load();
  portEXIT_CRITICAL(&lock);
  return record;
}
}  // namespace cp_boot

extern "C" {
esp_err_t __real_sdmmc_host_init();
esp_err_t __real_sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t* config);
esp_err_t __real_sdmmc_card_init(const sdmmc_host_t* host, sdmmc_card_t* card);
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t sector, size_t count);

esp_err_t __wrap_sdmmc_host_init() {
  cp_boot::sdResult(cp_boot::Stage::HostInit, cp_boot::Pending);
  const auto error = __real_sdmmc_host_init();
  cp_boot::sdResult(cp_boot::Stage::HostInit, error);
  return error;
}
esp_err_t __wrap_sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t* config) {
  cp_boot::sdResult(cp_boot::Stage::SlotInit, cp_boot::Pending);
  const auto error = __real_sdmmc_host_init_slot(slot, config);
  cp_boot::sdResult(cp_boot::Stage::SlotInit, error);
  return error;
}
esp_err_t __wrap_sdmmc_card_init(const sdmmc_host_t* host, sdmmc_card_t* card) {
  cp_boot::cardBegin();
  const auto error = __real_sdmmc_card_init(host, card);
  cp_boot::sdResult(cp_boot::Stage::CardInit, error);
  return error;
}
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t sector, size_t count) {
  const bool tracked = cp_boot::sectorBegin(sector, count);
  const auto error = __real_sdmmc_read_sectors(card, dst, sector, count);
  if (tracked) cp_boot::sectorEnd(error);
  return error;
}
}
#endif
