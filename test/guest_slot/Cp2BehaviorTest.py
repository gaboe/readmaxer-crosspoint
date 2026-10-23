"""Run production Home refresh and guest cache/switch functions with boundary fakes."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])

def block(text, start):
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

home = (root / "src/activities/home/HomeActivity.cpp").read_text()
guest = (root / "src/network/GuestSlot.cpp").read_text()
grid = (root / "src/components/CoverGridHomeUi.cpp").read_text()
start = home.index("guest = guest_slot::switchAllowed()")
end = home.index("\n", home.index("if (guest.image.installed) guestLabel", start))
enter = home[start:end]
marker = "if (ota_boot::confirmationPending() && ota_boot::confirmRunningSlot())"
first = home.index(marker)
second = home.index(marker, first + len(marker))
grid_refresh = block(home, first)
list_refresh = block(home, second)
functions = "\n".join(block(guest, guest.index(signature)) for signature in [
    "Inspection inspectFresh()", "Inspection inspect()", "bool switchAllowed()", "SwitchResult switchGuest()",
    "bool checkAndCache(", "void invalidateCache()",
])
# The rendering host must retain the guest label and paint a separate icon.
assert "BookIcon" in grid[grid.index("if (guestHeight)"):grid.index("// The board's tab height")]
assert "guestLabel" in grid[grid.index("if (guestHeight)"):grid.index("// The board's tab height")]
assert "hasGuest" not in home[home.index("void HomeActivity::onEnter()"):home.index("void HomeActivity::onEnter()") + 12000]
assert "silentRestart();" in home and "GUI.drawPopup(renderer, tr(STR_UPDATE_FAILED));" in home
flasher = (root / "src/network/FirmwareFlasher.cpp").read_text()
assert flasher.index("guest_slot::invalidateCache()") < flasher.index("esp_partition_erase_range(dest")
sd = (root / "src/activities/settings/SdFirmwareUpdateActivity.cpp")
if not sd.exists():
    sd = next(root.glob("src/activities/**/SdFirmwareUpdateActivity.cpp"))
sd_source = sd.read_text()
assert sd_source.index("if (result == firmware_flash::Result::OTADATA_UNKNOWN)") < sd_source.index("if (result != firmware_flash::Result::OK)")
unknown_sd = block(sd_source, sd_source.index("if (result == firmware_flash::Result::OTADATA_UNKNOWN)"))
assert "ESP.restart();" in unknown_sd and "return;" in unknown_sd
assert sd.read_text().count("tr(STR_FIRMWARE_READMAXER_INSTALL)") == 2
switch_home = block(home, home.index("switch (guest_slot::switchGuest())"))
program = r"""
#include <cassert>
#include <string>
#include <cstring>
#include "GuestSlotModel.h"
#include "FirmwareFlasher.h"
#define LOG_ERR(...) ((void)0)
#define FREEINK_DEVICE_X4PRO 1
struct esp_partition_t {};
esp_partition_t other;
bool confirmed=false, pending=true, confirmationOk=false, imageValid=true;
unsigned inspections=0, otadataWrites=0, updates=0;
namespace ota_boot {
bool runningSlotConfirmed() { return confirmed; }
bool confirmationPending() { return pending; }
SlotCheck runningSlotCheck() { return confirmed ? SlotCheck::Confirmed : SlotCheck::Unconfirmed; }
SelectionResult nextSelection=SelectionResult::Selected;
SelectionResult switchTo(const esp_partition_t*, unsigned) { ++otadataWrites; return nextSelection; }
bool confirmRunningSlot() { if (confirmationOk) { confirmed=true; pending=false; } return confirmationOk; }
}
namespace guest_slot {
enum class SwitchResult { NoGuest, GuardRefused, SelectionFailed, Selected, Unknown };
struct Inspection { GuestImage image; bool bootableNonGuest=false; const esp_partition_t* partition=nullptr; };
Inspection cached;
bool cacheReady=false;
const esp_partition_t* otherPartition() { return &other; }
bool inspectPartition(const esp_partition_t*, Inspection& result) {
  ++inspections;
  if (imageValid) { result.image.installed=true; std::strcpy(result.image.version,"1.2.3"); result.partition=&other; }
  return imageValid;
}
""" + functions + r"""
}
const int STR_READMAXER=1;
const char* tr(int) { return "READMAXER"; }
struct Grid { std::string label; void setGuest(const char* text) { label=text?text:""; } } grid;
struct Home {
  guest_slot::Inspection guest;
  std::string guestLabel;
  Grid* coverGridUi=&grid;
  void requestUpdate() { ++updates; }
  void enter() { ENTRY }
  void renderGrid() { GRID }
  void renderList() { LIST }
};
int restarts=0, popups=0, renderer=0;
const int STR_UPDATE_FAILED=2;
void silentRestart() { ++restarts; }
struct Gui { void drawPopup(int,const char*) { ++popups; } } GUI;
void homeSwitch() { SWITCH_HOME }
int sdRestarts=0, sdFailures=0;
struct Esp { void restart() { ++sdRestarts; } } ESP;
void sdResult(firmware_flash::Result result) { UNKNOWN_SD ++sdFailures; }
int main() {
  Home home;
  home.enter();
  assert(!home.guest.image.installed && inspections==0);
  home.renderGrid();
  assert(!home.guest.image.installed && updates==0);
  confirmationOk=true;
  home.renderGrid();
  assert(home.guest.image.installed && home.guestLabel=="READMAXER 1.2.3" && grid.label==home.guestLabel && updates==1);
  guest_slot::inspect(); guest_slot::inspect();
  assert(inspections==1);
  assert(guest_slot::switchGuest()==guest_slot::SwitchResult::Selected && inspections==2 && otadataWrites==1);
  imageValid=false;
  assert(guest_slot::switchGuest()==guest_slot::SwitchResult::NoGuest && inspections==3 && otadataWrites==1);
  guest_slot::invalidateCache();
  assert(!guest_slot::inspect().image.installed && inspections==4);
  imageValid=true;
  guest_slot::invalidateCache();
  pending=true; confirmed=false;
  home.enter(); assert(!home.guest.image.installed);
  home.renderList();
  assert(home.guest.image.installed && home.guestLabel=="READMAXER 1.2.3" && updates==2);
  confirmed=false;
  assert(guest_slot::switchGuest()==guest_slot::SwitchResult::GuardRefused && otadataWrites==1);
  confirmed=true; pending=false;
  ota_boot::nextSelection=ota_boot::SelectionResult::Unknown;
  homeSwitch(); assert(restarts==1 && popups==0);
  ota_boot::nextSelection=ota_boot::SelectionResult::Failed;
  homeSwitch(); assert(restarts==1 && popups==1);
  sdResult(firmware_flash::Result::OTADATA_UNKNOWN);
  assert(sdRestarts==1 && sdFailures==0);
  sdResult(firmware_flash::Result::OTADATA_FAIL);
  assert(sdRestarts==1 && sdFailures==1);
}
"""
program = program.replace("UNKNOWN_SD", unknown_sd).replace("SWITCH_HOME", switch_home).replace("ENTRY", enter).replace("GRID", grid_refresh).replace("LIST", list_refresh)
with tempfile.TemporaryDirectory(prefix="cp2-home-guest-") as tmp:
    cpp = pathlib.Path(tmp) / "behavior.cpp"
    exe = pathlib.Path(tmp) / "behavior"
    cpp.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++17", "-I" + str(root / "src/network"), str(cpp),
                    str(root / "src/network/GuestSlotModel.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Hidden/revealed guest, both Home refresh paths, fresh switch authentication, cache, and failure routing passed.")
