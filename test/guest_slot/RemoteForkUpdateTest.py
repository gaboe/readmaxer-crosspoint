"""Exercise the production remote fork update handler with host fakes."""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
source = (root / "src/network/CrossPointWebServer.cpp").read_text()
signature = "void CrossPointWebServer::handleForkUpdate()"
start = source.index(signature)
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
handler = source[start:end]

program = r"""
#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

struct String : std::string {
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  size_t length() const { return size(); }
  int indexOf(char needle, size_t from) const {
    const auto found = find(needle, from);
    return found == npos ? -1 : static_cast<int>(found);
  }
  bool endsWith(const char* suffix) const { return ends_with(suffix); }
  String substring(size_t from) const { return substr(from); }
};

namespace FsHelpers {
bool isSafePathComponent(const String& name) {
  if (name.empty() || name == "..") return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}
}

namespace firmware_flash {
enum class Result { OK, WRONG_FORK, GUARD_REFUSED, WRITE_FAIL, OTADATA_UNKNOWN };
Result preflight = Result::OK;
Result flashResult = Result::OK;
int inspectCalls = 0;
int flashCalls = 0;
Result inspectImageFile(const char*, size_t*) { ++inspectCalls; return preflight; }
Result flashFromSdPath(const char*, void*, void*) { ++flashCalls; return flashResult; }
const char* resultName(Result result) {
  switch (result) {
    case Result::OK: return "OK";
    case Result::WRONG_FORK: return "WRONG_FORK";
    case Result::GUARD_REFUSED: return "GUARD_REFUSED";
    case Result::WRITE_FAIL: return "WRITE_FAIL";
    case Result::OTADATA_UNKNOWN: return "OTADATA_UNKNOWN";
  }
  return "?";
}
}

namespace ota_boot {
bool pending = false;
bool guardAllowed = true;
bool confirmationPending() { return pending; }
struct AppWriteGuard {
  explicit AppWriteGuard(const void*) {}
  explicit operator bool() const { return guardAllowed; }
};
}

const void* esp_ota_get_next_update_partition(void*) { return reinterpret_cast<void*>(1); }
bool wsUploadInProgress = false;
int restarts = 0;
void delay(int) {}
struct { void restart() { ++restarts; } } ESP;

struct Client { void flush() {} };
struct Server {
  String path = "/fork.bin";
  int status = 0;
  std::string body;
  Client connection;
  String arg(const char*) const { return path; }
  void send(int code, const char*, const char* response) { status = code; body = response; }
  void sendHeader(const char*, const char*) {}
  Client& client() { return connection; }
};

struct CrossPointWebServer {
  Server* server;
  struct { bool file = false; } upload, fontUpload;
  void (*guestInstallStatus)(bool, const char*, void*) = nullptr;
  void* guestInstallContext = nullptr;
  void handleForkUpdate();
};

HANDLER

void status(bool, const char*, void*) {}
void reset(Server& server, CrossPointWebServer& web) {
  server = {};
  web.guestInstallStatus = status;
  web.upload.file = false;
  web.fontUpload.file = false;
  wsUploadInProgress = false;
  firmware_flash::preflight = firmware_flash::Result::OK;
  firmware_flash::flashResult = firmware_flash::Result::OK;
  firmware_flash::inspectCalls = 0;
  firmware_flash::flashCalls = 0;
  ota_boot::pending = false;
  ota_boot::guardAllowed = true;
  restarts = 0;
}

int main() {
  Server server;
  CrossPointWebServer web{&server};

  reset(server, web);
  web.guestInstallStatus = nullptr;
  web.handleForkUpdate();
  assert(server.status == 409 && server.body == R"({"result":"OPEN_INSTALL_SCREEN"})");
  assert(firmware_flash::inspectCalls == 0 && firmware_flash::flashCalls == 0);

  reset(server, web);
  web.upload.file = true;
  web.handleForkUpdate();
  assert(server.status == 409 && firmware_flash::flashCalls == 0);

  reset(server, web);
  server.path = "/../fork.bin";
  web.handleForkUpdate();
  assert(server.status == 409 && firmware_flash::inspectCalls == 0 && firmware_flash::flashCalls == 0);

  reset(server, web);
  firmware_flash::preflight = firmware_flash::Result::WRONG_FORK;
  web.handleForkUpdate();
  assert(server.status == 409 && server.body == R"({"result":"WRONG_FORK"})");
  assert(firmware_flash::flashCalls == 0);

  reset(server, web);
  ota_boot::pending = true;
  web.handleForkUpdate();
  assert(server.status == 409 && firmware_flash::flashCalls == 0);

  reset(server, web);
  ota_boot::guardAllowed = false;
  web.handleForkUpdate();
  assert(server.status == 409 && firmware_flash::flashCalls == 0);

  reset(server, web);
  firmware_flash::flashResult = firmware_flash::Result::WRITE_FAIL;
  web.handleForkUpdate();
  assert(server.status == 500 && firmware_flash::flashCalls == 1 && restarts == 0);

  reset(server, web);
  web.handleForkUpdate();
  assert(server.status == 200 && server.body == R"({"result":"OK"})");
  assert(firmware_flash::flashCalls == 1 && restarts == 1);
}
""".replace("HANDLER", handler)

with tempfile.TemporaryDirectory(prefix="readmaxer-fork-update-") as directory:
    source_file = pathlib.Path(directory) / "test.cpp"
    executable = pathlib.Path(directory) / "test"
    source_file.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++20", str(source_file), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
