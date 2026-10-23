"""Run production sleep paths, including a failure queued after the first check."""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).read_text()
start = source.index("static void sleepPrepared(")
end = source.index("void setupDisplayAndFonts(", start)
functions = source[start:end]
program = r"""
#include <cassert>
#include "OtaSlotModel.h"
int shutdown=0, prepared=0, slept=0, checks=0;
bool keep=false, failSecond=false, deepSleepInProgress=false;
namespace ota_boot {
SelectionResult keepRunningSlot() {
  ++checks;
  return keep && !(failSecond && checks>1) ? SelectionResult::Selected : SelectionResult::Failed;
}
}
namespace HalPowerManager { struct Lock {}; }
struct StorageStub {
  void prepareForDeepSleep() { ++prepared; }
  bool exists(const char*) { return false; }
  void remove(const char*) {}
} Storage;
int gpio;
struct PowerStub { void startDeepSleep(int, bool) { ++slept; } } powerManager;
struct Activity {
  bool isReaderActivity() { return false; }
  void goToSleep(bool) { ++shutdown; }
} activityManager;
struct AppState { bool lastSleepFromReader, showBootScreen; void saveToFile() {} } APP_STATE;
namespace trustedtime { void note() {} }
namespace bleturner { bool beforeSleep(int) { return true; } }
void deliverSleepPluginEvents(bool) {}
namespace CrossPointSettings {
enum class SLEEP_SCREEN_MODE { QUICK_RESUME };
enum class QUICK_RESUME_SLEEP_SCREEN { QUICK_RESUME_AFTER_TIMEOUT };
}
struct Settings {
CrossPointSettings::SLEEP_SCREEN_MODE sleepScreen;
CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN quickResumeSleepScreen;
} SETTINGS;
void saveSleepFrameBuffer() {}
const char* SLEEP_FRAME_FILE="sleep";
constexpr int WIFI_MODE_NULL=0, WIFI_OFF=0;
struct Wifi { int getMode() { return 0; } void disconnect(bool) {} void mode(int) {} } WiFi;
struct Device { void deepSleep() {} } halTiltSensor, display;
#define FREEINK_DEVICE_PICCO 0
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
""" + functions + r"""
int main() {
  sleepNow();
  enterDeepSleep();
  assert(prepared==0 && slept==0 && shutdown==0 && !deepSleepInProgress);
  keep=true; failSecond=true; checks=0;
  enterDeepSleep();
  assert(checks==1 && prepared==1 && slept==1 && shutdown==1 && deepSleepInProgress);
  checks=0;
  sleepNow();
  assert(checks==1 && prepared==2 && slept==2);
}
"""
with tempfile.TemporaryDirectory(prefix="cp2-sleep-gate-") as tmp:
    cpp = pathlib.Path(tmp) / "sleep.cpp"
    exe = pathlib.Path(tmp) / "sleep"
    cpp.write_text(program)
    include = pathlib.Path(sys.argv[1]).parent / "network"
    subprocess.run([sys.argv[2], "-std=c++17", "-I" + str(include), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Both sleep paths check once before teardown; a queued second-check failure cannot strand the display.")
