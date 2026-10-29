# Webserver Endpoints

This document describes the HTTP, WebSocket, WebDAV, and discovery endpoints
available while CrossPoint Reader is in File Transfer, Calibre Wireless, or the
Settings > General > Install READMAXER screen.

- HTTP server: port 80
- WebSocket upload server: port 81
- UDP discovery listener: port 8134
- WebDAV: port 80, handled by the same HTTP server

Examples use `crosspoint.local`. If mDNS does not resolve on your network, use
the IP address shown on the device screen.

## HTTP Pages

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/` | Home/status page |
| `GET` | `/files` | File manager page |
| `GET` | `/settings` | Web settings page |
| `GET` | `/fonts` | SD-card font manager page |
| `GET` | `/js/jszip.min.js` | JavaScript asset used by the file manager |

## Device Status

### `GET /api/status`

```bash
curl http://crosspoint.local/api/status
```

Response:

```json
{
  "version": "1.0.0",
  "firmwareFamily": "crosspoint",
  "pluginApi": 1,
  "pluginCapabilities": "catalog.lists,catalog.notices,catalog.bundle_install,catalog.version_tracking,catalog.compatibility,catalog.item_details",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600,
  "device": "X4"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Firmware version |
| `firmwareFamily` | string | Stable compatibility family; stock CrossPoint uses `crosspoint` |
| `pluginApi` | number | Numeric plugin API level used for catalog compatibility |
| `pluginCapabilities` | string | Comma-separated firmware capabilities used for catalog compatibility |
| `ip` | string | Device IP address |
| `mode` | string | `"STA"` for joined Wi-Fi or `"AP"` for hotspot mode |
| `rssi` | number | Wi-Fi RSSI in dBm; `0` in AP mode |
| `freeHeap` | number | Free heap in bytes |
| `uptime` | number | Seconds since boot |
| `deviceId` | string | With `?plugin=<name>`: 64 hex chars, stable per device and plugin (SHA-256 of an on-device NVS secret and the name); omitted if NVS is unavailable |
| `device` | string | `"X3"` or `"X4"` hardware detection |

## File Management

### `GET /api/files`

Lists files and folders under a directory.

```bash
curl "http://crosspoint.local/api/files?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Directory to list |

Response:

```json
[
  {"name":"MyBook.epub","size":1234567,"isDirectory":false,"isEpub":true},
  {"name":"Notes","size":0,"isDirectory":true,"isEpub":false}
]
```

Hidden dotfiles are omitted unless the device setting `showHiddenFiles` is
enabled. `System Volume Information` and `XTCache` are always hidden/protected.

The saved-credential stores (`/.crosspoint/wifi.json`, `opds.json`,
`koreader.json`) can never be downloaded, uploaded, renamed, moved, deleted or
written by a plugin over the network, however the path is spelled.

### `GET /download`

Downloads a file from the SD card.

```bash
curl -OJ "http://crosspoint.local/download?path=/Books/MyBook.epub"
```

Query parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | File path to download |

Protected dotfiles, `System Volume Information`, `XTCache`, and the credential
stores cannot be downloaded. EPUB files are served as `application/epub+zip`; other files use
`application/octet-stream`.

### `POST /upload`

Uploads a file with HTTP multipart form data.

```bash
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Destination directory |

Successful response:

```text
File uploaded successfully: mybook.epub
```

Notes:

- Existing files with the same name are refused.
- EPUB cache data for the uploaded path is cleared after a successful upload.
- Uploading a book file (`.epub`, `.txt`, `.md`, `.xtc`), here or through the
  WebSocket upload, marks the Library index stale, so the book appears the next
  time the Library opens. Hidden files and folders (names that start with `.`)
  don't mark it.
- HTTP upload uses a 4 KB write buffer before flushing to the SD card.

### READMAXER development loop

On X4 Pro, the file `/readmaxer/devloop` makes the fork open **Install READMAXER**
after a normal boot. The screen joins saved Wi-Fi and starts the web server.
Back leaves the screen. Recovery, crash reporting, and first-boot slot
confirmation still take priority. Without the file, boot routing is unchanged.

Create the empty flag while a web server screen is open:

```sh
touch devloop
curl --noproxy '*' -F 'file=@devloop;filename=devloop' 'http://READER_HOST/upload?path=/readmaxer'
```

Remove the flag through `POST /delete`:

```sh
curl --noproxy '*' -d 'path=/readmaxer/devloop' 'http://READER_HOST/delete'
```

Replace `READER_HOST` with the hostname or IP shown on the reader. The flag
is read at boot. These endpoints have no authentication. Use a trusted network.

### `GET /api/readmaxer/state`

X4 Pro only. The endpoint is enabled while **Install READMAXER** is open.
It reports the running app slot, guest installation, the devloop flag, build
identity, and retained CrossPoint boot stages. Example response:

```json
{
  "running_slot": "ota_0",
  "guest_installed": true,
  "guest_version": "1.2.3",
  "devloop": true,
  "fork_sha": "4ba93709757e4dd10400a827d769525c8fbf0bb4",
  "cp_boots": [
    {
      "counter": 2,
      "reset_reason": 3,
      "last_stage": "first-draw",
      "sd_attempts": [{"stage": "sector-zero-read", "esp_err": 0}]
    },
    {
      "counter": 1,
      "reset_reason": 3,
      "last_stage": "card-init",
      "sd_attempts": [{"stage": "card-init", "esp_err": -32768}]
    }
  ]
}
```

`running_slot` is `ota_0`, `ota_1`, or `unknown`. `guest_version` is empty when
no guest is installed. Outside the install screen, the endpoint returns HTTP
403 with `OPEN_INSTALL_SCREEN`.

`fork_sha` is the full Git HEAD SHA captured at build time. It does not identify
uncommitted edits included in that build.

`cp_boots` contains at most two boots, newest first. Each entry starts at the
first statement of CrossPoint `setup()`. `counter` is an unsigned 16-bit value
and wraps from 65535 to zero. `reset_reason` is the numeric ESP-IDF
`esp_reset_reason()` value. `last_stage` is the latest recorded boot stage.
`first-draw` means entry to the first panel refresh call. It does not prove
that the physical panel completed a refresh.

`sd_attempts` contains at most four entries in attempt order. Each entry retains
only its latest observed stage and result. Host or slot setup uses the first
entry before card initialization starts. Stages include `host-init`, `slot-init`,
`card-init`, `sector-zero-read`, and `volume-mount`. `esp_err` is a signed value:

- `0` means success for that recorded call.
- `-32768` means the call was entered but had not returned when recorded.
- `-32767` means an error did not fit in a signed 16-bit value.
- Other values are SDK error codes. A failed volume mount uses synthetic
  `ESP_FAIL`, or `-1`, because the filesystem mount returns only a boolean.

The versioned record uses RTC no-init memory and one CRC for both boots. A bad
CRC or unsupported version discards the history and restarts the counter at
one. A reset during a record update can invalidate both entries. Power loss
can erase the history. A boot that never reaches `setup()` adds no entry.
HTTP cannot report an ongoing early boot hang. Read the older entry after a
recovery boot that preserves RTC and reaches the install screen. Warm-reset
retention and physical panel behavior still require device verification.

### `POST /api/readmaxer/start`

X4 Pro only. The endpoint is enabled while **Install READMAXER** is open.
The request has no body. It uses the Home READMAXER item's guarded switch to
the installed guest, sends JSON, then restarts. `OK` means the selection was
read back. It does not prove that the guest started.

```json
{"result":"OK"}
```

If no guest is installed, HTTP 409 reports `NO_GUEST`. A switch guard refusal
reports `GUARD_REFUSED`. Neither case writes otadata. A failed selection
reports HTTP 409 with `SELECTION_FAILED`; otadata may have changed. An active
upload returns HTTP 409 with `UPLOAD_BUSY`. `SELECTION_UNKNOWN` means the
selection readback failed. The reader restarts, but the boot result is
unconfirmed. Inspect the reader after either selection error. Outside the
install screen, HTTP 403 reports `OPEN_INSTALL_SCREEN`.

### `POST /api/readmaxer/install`

X4 Pro only. The endpoint is enabled while **Settings > General > Install READMAXER** is open.
Known POC limit: this screen exposes the install endpoint to every client on the
joined Wi-Fi. Use it only on a trusted network.
That screen joins the last saved Wi-Fi network, or the first saved network if there is no last network.
It waits up to 30 seconds. A failure stops Wi-Fi and shows an error.
The server answers to both `crosspoint.local` and `readmaxer-<station-mac>.local`.

The request has no body. It installs the staged files `/readmaxer/install.bin` and
`/readmaxer/install.json`. The JSON file is limited to 512 bytes and contains a
64-character hexadecimal `sha256` field for the complete binary.
Use the [Rust uploader](../tools/install-guest/README.md) to stage the files.

Before any erase, the installer checks the complete file's SHA-256, CP2 image rules,
project name, guest and host marker counts, device MAC marker, destination size,
running-slot confirmation, and CP2 `AppWriteGuard`.
A gate refusal leaves both application slots and otadata unchanged.
The write runs synchronously. Other uploads cannot run during the install.
An upload already in progress returns HTTP 409 with `UPLOAD_BUSY`.
Outside the install screen, the endpoint returns HTTP 403 with `OPEN_INSTALL_SCREEN`.

After writing, the installer hashes the destination and compares it with the manifest.
It selects the guest as NEW through CP2's guarded writer, preserving a VALID non-guest fallback.
It saves `/readmaxer/last-install.json`, sends the response, and restarts.
An SD receipt failure does not undo a completed selection. `receipt_saved` reports that failure.
Receipt replacement keeps the previous receipt until the new file is renamed.
If replacement fails, the installer restores the previous receipt. If that restore
also fails, the previous receipt remains at `/readmaxer/last-install.bak`.

The response is HTTP 200 for `OK` or HTTP 409 for an install error:

```json
{"result":"OK","sha256":"<64 hexadecimal characters>","receipt_saved":true}
```

`SELECTION_UNKNOWN` means the stored selection could not be read reliably.
The reader restarts, but the boot result is unconfirmed. Check the reader before retrying.
Read, write, or readback failures after the first erase can leave a partial guest slot.
They never select that slot and keep a CRC-valid VALID otadata entry naming the bootable fork.
Selection failure or unknown readback after an otadata erase also keeps that VALID
fork entry through CP2's guarded plan. The other otadata sector may have changed.
The install screen stays open for an error or another upload. Back closes the server and Wi-Fi.
There is no handoff-sector write or USB fallback.

### `POST /api/input`

Send form data `button=confirm`, `button=back`, `button=next`, or
`button=previous` while the web server runs. The endpoint injects one click
through the shared remote-button input path. HTTP 200 reports `OK`. HTTP 409
reports `CLICK_PENDING` if a click is already queued, or `INPUT_BUSY` if a
physical input blocks injection. An unknown button returns HTTP 400 with
`INVALID_BUTTON`. A server activity consumes the click before the next input
frame resets it. Input has no authentication. Use a trusted network.

### `GET /api/screen`

While the web server runs, this endpoint streams the current 1-bit framebuffer
as a binary PBM file (`P4`). The header gives the physical panel width and
height. The server streams panel rows directly from the existing framebuffer
and holds the render lock while it sends the frame. The image may omit display
effects that are outside the framebuffer.
Screen capture has no authentication. Use a trusted network.

### `POST /mkdir`

Creates a folder.

```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

Form parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | Yes | - | New folder name |
| `path` | No | `/` | Parent folder |

### `POST /rename`

Renames a file.

```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `name` | Yes | New file name, not a path |

Only files can be renamed through this endpoint. The old EPUB cache path is
cleared before the rename.

### `POST /move`

Moves a file into an existing folder.

```bash
curl -X POST -d "path=/Books/mybook.epub&dest=/Read" http://crosspoint.local/move
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `dest` | Yes | Existing destination folder |

Only files can be moved through this endpoint. The old EPUB cache path is
cleared before the move.

### `POST /delete`

Deletes one or more files or empty folders.

```bash
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete
curl -X POST -d 'paths=["/Books/old.epub","/OldFolder"]' http://crosspoint.local/delete
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes, unless `paths` is provided | Single path to delete |
| `paths` | Yes, unless `path` is provided | JSON array of paths to delete |

`System Volume Information`, `XTCache` and the saved-credential stores cannot be
deleted. Hidden items (names that start with `.`) can be deleted. Non-empty
folders are rejected. EPUB cache data for deleted files is cleared.

## Settings API

### `GET /api/settings`

Returns a streamed JSON array of editable settings. Each item contains common
fields plus type-specific fields.

```bash
curl http://crosspoint.local/api/settings
```

Example item:

```json
{
  "key": "fontSize",
  "name": "Reader Font Size",
  "category": "Reader",
  "type": "enum",
  "value": 1,
  "options": ["12 pt", "14 pt", "16 pt", "18 pt"]
}
```

`value` is always an index into `options`, never the option's text. The
`fontSize` options depend on the selected family. A `.cpfont` family installed
at 10/12/14 pt offers those three sizes. TTF/OTF/TTC families offer the
standard 12/14/16/18 pt sizes. The `fontFamily` and `dictionaryName` options
also depend on the SD card contents.

Types:

| Type | Extra fields |
|------|--------------|
| `toggle` | `value` (`0` or `1`) |
| `enum` | `value`, `options` |
| `value` | `value`, `min`, `max`, `step` |
| `string` | `value` |

The font-family setting includes SD-card font families when they are installed.

### `POST /api/settings`

Applies a partial settings update from a JSON object.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"fontSize":2,"showHiddenFiles":1}' \
  http://crosspoint.local/api/settings
```

Successful response:

```text
Applied 2 setting(s)
```

## Font Management API

### `GET /api/fonts`

Lists installed SD-card font families. On devices that load TTF/OTF/TTC files,
these families appear with `sizes: [0]`. The `0` means that the font file has
no fixed point size; the reader offers 12, 14, 16, and 18 pt.

```bash
curl http://crosspoint.local/api/fonts
```

Response:

```json
{
  "maxFamilies": 128,
  "families": [
    {
      "name": "Literata",
      "sizes": [12, 14, 16, 18],
      "files": [
        {"name": "Literata_12.cpfont", "size": 123456}
      ]
    }
  ]
}
```

### `POST /api/fonts/upload`

Uploads one `.cpfont` file into a family folder.

```bash
curl -X POST \
  -F "family=Literata" \
  -F "file=@Literata_12.cpfont" \
  http://crosspoint.local/api/fonts/upload
```

The handler validates the family name, `.cpfont` filename, and `CPFONT` magic
bytes before accepting the file.

Successful response:

```json
{"ok":true}
```

### `POST /api/fonts/delete`

Deletes an installed font family.

This endpoint removes family subfolders. To remove a loose TTF/OTF/TTC file
from a font root, delete the file from the SD card instead.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"family":"Literata"}' \
  http://crosspoint.local/api/fonts/delete
```

Successful response:

```json
{"ok":true}
```

## OPDS Server API

### `GET /api/opds`

Lists saved OPDS servers. Passwords are never returned.

```bash
curl http://crosspoint.local/api/opds
```

Response:

```json
[
  {
    "index": 0,
    "name": "My Catalog",
    "url": "http://calibre.local:8080/opds",
    "username": "reader",
    "hasPassword": true
  }
]
```

### `POST /api/opds`

Adds or updates an OPDS server. Include `index` to update an existing entry.
If `password` is omitted during an update, the existing password is preserved.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"My Catalog","url":"http://calibre.local:8080/opds","username":"reader","password":"secret"}' \
  http://crosspoint.local/api/opds
```

### `POST /api/opds/delete`

Deletes an OPDS server by index.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/opds/delete
```

## Wi-Fi Credential API

### `GET /api/wifi`

Lists saved Wi-Fi networks. Passwords are never returned.

```bash
curl http://crosspoint.local/api/wifi
```

Response:

```json
[
  {
    "index": 0,
    "ssid": "HomeWiFi",
    "hasPassword": true,
    "isLastConnected": true
  }
]
```

### `POST /api/wifi`

Adds or updates a saved Wi-Fi network. Include `index` to update an existing
entry. If `password` is omitted during an update, the existing password is
preserved.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"ssid":"HomeWiFi","password":"secret"}' \
  http://crosspoint.local/api/wifi
```

### `POST /api/wifi/delete`

Deletes a saved Wi-Fi network by index.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/wifi/delete
```

## WebSocket Upload

### Port 81

The WebSocket path is used for fast binary uploads from the file manager and
Calibre plugin workflows.

Connection:

```text
ws://crosspoint.local:81/
```

Protocol:

1. Client sends text: `START:<filename>:<size>:<path>`
2. Server replies `READY`
3. Client sends binary chunks
4. Server sends `PROGRESS:<received>:<total>` every 64 KB or at completion
5. Server sends `DONE` when complete or `ERROR:<message>` on failure

Example session:

```text
Client -> START:mybook.epub:1234567:/Books
Server -> READY
Client -> [binary chunk]
Server -> PROGRESS:65536:1234567
...
Server -> DONE
```

Error messages include:

| Message | Cause |
|---------|-------|
| `ERROR:Upload already in progress` | A second upload was started before the first completed |
| `ERROR:Invalid START format` | Malformed START message or invalid size token |
| `ERROR:Failed to create file` | Destination file could not be opened |
| `ERROR:No upload in progress` | Binary data arrived without a matching START |
| `ERROR:Upload overflow` | Client sent more bytes than declared |
| `ERROR:Write failed - disk full?` | SD write failed |

Incomplete WebSocket uploads are deleted on disconnect or error.

## WebDAV

The same HTTP server registers a WebDAV-compatible handler for file manager clients.

Supported methods:

```text
OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK
```

Notes:

- `PUT` writes to a temporary `.davtmp` file first, then renames it into place.
- Protected paths are rejected.
- `LOCK` and `UNLOCK` are accepted for client compatibility only. The server
  does not implement full WebDAV Class 2 locking semantics such as persistent
  locks or lock discovery.

## UDP Discovery

The server listens on UDP port `8134`. When it receives the text payload
`hello`, it replies to the sender with:

```text
crosspoint (on <hostname>);81
```

The final field is the WebSocket upload port.

## Network Modes

### Station Mode (STA)

- Device joins an existing 2.4 GHz Wi-Fi network.
- `crosspoint.local` is advertised with mDNS when available.
- `/api/status` returns `"mode": "STA"` and RSSI in dBm.

### Access Point Mode (AP)

- Device creates an open hotspot named `CrossPoint-Reader`.
- The device shows a Wi-Fi QR code and URL QR code.
- The fallback IP is typically `192.168.4.1`.
- `/api/status` returns `"mode": "AP"` and `"rssi": 0`.

### Calibre Wireless

Calibre Wireless starts the same web server in STA mode and displays setup
instructions plus WebSocket upload progress on the device screen.

## Plugin API

Device capabilities for browser-side SD plugins (`plugin.js`). Plugins normally
reach these through the `PluginHost` wrappers (`api.relay`, `api.crypto`,
`api.fetchToSd`, `api.writeFile`); the raw contracts are below. Outbound TLS is
encrypted but the peer is not verified (the transport ships no CA bundle).

### `GET /api/plugins`

Installed plugins that ship a `plugin.js`:
`[{"name":"<folder>","dir":"<root>/<folder>","title":"<title>","mount":"settings"}, ...]`.
`dir` is where the plugin is installed (plugins keep their own files there);
`title` and `mount` come from the plugin's `manifest.json` when present.

### `GET /plugin?name=<plugin>&file=<file>`

Serves one file from the plugin's folder (first root holding that name). Both
parameters must be single path components (no `/`, `\`, or `..`); 404 when
the plugin, file, or a directory is requested.

### `POST /api/relay`

Makes an outbound HTTP(S) request for a plugin (any method; browsers can't, due
to CORS). Body: `{"plugin":"<name>","method":"GET","url":"https://...","headers":{...},"body":"..."}`.

- **Success:** `200` with the upstream body verbatim. The upstream status is
  in the `X-Relay-Status` response header, and its headers in `X-Relay-Headers`
  as JSON `[["name","value"], ...]` in receive order, duplicates kept (every
  `Set-Cookie`). `api.relay()` resolves to `{status, headers, body}`.
- **Redirects** are not followed: a 3xx comes back with its `location` header.
- **Limits:** the upstream body is capped at 32KB; larger payloads belong on
  `/api/fetch`.
- **Errors:** `400 {"error":"missing plugin/url"}`; `502 {"error":"relay failed; ..."}`
  for a transport failure, a truncated body, a body over the cap, or low memory
  (the device log names which).

### `POST /api/crypto`

Stateless wolfSSL primitives. Body `{"op":"<op>", ...}`; binary fields are
base64 in both directions, each at most 64KB encoded. Replies are `200` with
the result fields, or `200 {"error":"..."}`.

| `op` | Request fields | Result |
| --- | --- | --- |
| `random` | `len` (default 16, max 4096) | `data` |
| `sha1` | `data` | `data` (20 bytes) |
| `sha256` | `data` | `data` (32 bytes) |
| `aesenc` | `key`, `iv` (16 bytes each), `data` | `data`: AES-128-CBC with PKCS#7 padding |
| `aesdec` | `key`, `iv`, `data` (block-aligned) | `data`: AES-128-CBC, padding left in place |
| `keygen` | none | `public` (SPKI DER), `private` (PKCS#8 DER): RSA key pair |
| `pubencrypt` | `cert` (X.509 DER), `data` | `data`: RSAES-PKCS1-v1_5 to the certificate's key |
| `sign` | `private` (PKCS#8 DER), `hash` (20 bytes) | `data` (128 bytes): raw RSA signature |
| `rsadec` | `private` (PKCS#8 DER), `data` (one modulus block) | `data`: raw RSA private-key result, padding left in place |
| `pkcs12` | `data` (the bundle), `password` | `key`, `cert` (DER) |

### `POST /api/book-key`

Stores a protected book's content key for the reader. Body:
`{"path":"/abs/book.epub","key":"<base64, 16 bytes>","expires":<epoch seconds, optional>}`.
The device wraps the key with AES-256-GCM under a key derived from its NVS
secret and writes `<path>.key`; the header (including `expires`) is
authenticated, so the file only opens on this reader and its loan date cannot
be edited. The reader enforces `expires` when the book opens.

- **Success:** `200 {"ok":true}`.
- **Errors:** `400 {"error":"bad path/key"}`; `500 {"error":"cannot store key"}`.

### `POST /api/fetch`

Downloads a URL straight to SD, so a large body never passes through the
browser. Body: `{"plugin":"<name>","url":"...","dest":"/abs/path","headers":{...},"offset":0,"maxBytes":0}`.

- `dest` must be absolute with no `..`; missing parent folders are created.
- The transfer is staged in `<dest>.part` and replaces `dest` only when the
  whole body has arrived, so a failed update keeps the previous file.
- Installing a book file (`.epub`, `.txt`, `.md`, `.xtc`) marks the Library
  index stale, so the book appears the next time the Library opens. Requests
  that end with `complete:false`, failed transfers, other file types, and
  hidden files or folders (names that start with `.`) don't mark it.
- Up to 5 redirects are followed. `headers` (typically the plugin's
  `Authorization`) are sent only while the target keeps the starting URL's
  scheme, host, and port; a redirect elsewhere, including https→http, gets none.
- A body cut short mid-transfer resumes on its own with a Range request.
- **Segments:** `maxBytes` (at most 4MB) ends the request after that many
  bytes with `complete:false`; send the next request with `offset` set to the
  returned `bytes` to continue the same `.part`. `api.fetchToSd()` does this
  loop, keeping each browser request short.
- A long transfer answers early with `200` chunked and sends whitespace every
  5s to keep the browser connection open; the JSON result follows at the end.
- **Result:** `{"status":200,"bytes":N,"complete":true,"total":N}` (`total`
  when the server reported a size). `error` is set to `transport failure`,
  `http status`, or `sd write failed` when the file was not installed.
- **Errors:** `400 {"error":"bad url/dest"}`; `409 {"error":"offset mismatch","bytes":N}`
  when `offset` does not match the `.part` size; `502 {"error":"sd write failed" |
  "range unsupported" | "download truncated","bytes":N,"complete":false}`.

### `POST /api/plugin-fs?plugin=<name>&path=<abs path>`

Writes one small file to SD. The content is sent as a multipart file part
(`api.writeFile()` builds it), so binary data, including NUL bytes, arrives
intact. It streams to `<path>.tmp` and replaces `path` only after a complete,
non-empty body. `path` must be absolute, without `..`, and not a credential
store. A plugin keeps its own files in its install folder (`api.dir`). As with
`/api/fetch`, writing a book file marks the Library index stale.

- **Success:** `200 {"ok":true,"bytes":N}`.
- **Errors:** `400` (`bad path`, `empty body`, `missing file part`,
  `upload aborted`); `413 {"error":"too large"}` over 256KB; `500
  {"error":"cannot write" | "sd write failed"}`. The previous file is kept on
  every error.

## Plugin Job Queue

External systems trigger SD-plugin actions without the web UI. The firmware
stores opaque `{plugin, action, args}` blobs (fixed 6-slot pool; args/result
< 192 bytes JSON); an open page hosting the plugin executes them. See
`docs/sd-plugins.md` for the full contract.

### `POST /api/plugin-jobs`

```bash
curl -X POST http://crosspoint.local/api/plugin-jobs \
  -d '{"plugin":"<name>","action":"<action>","args":{"path":"/Books/somefile"}}'
# -> {"id":3}         (503 {"error":"job queue full"} when all slots busy)
```

### `GET /api/plugin-jobs/claim?plugin=<name>`

Executor-side (used by the plugin host page): returns the next pending job
`{"id":3,"claim":7,"action":"<action>","args":{...}}` and marks it running, or `{"id":0}`.
A running job whose lease expires returns to pending and gets a new `claim` when re-claimed.

### `POST /api/plugin-jobs/complete`

Executor-side: `{"id":3,"claim":7,"ok":true,"result":{...}}` -> `{"ok":true}`. The `claim`
must match the one from the claim response; a stale claim (the lease expired and another
executor re-claimed the job) gets `409 {"error":"stale claim"}` and changes nothing.

### `GET /api/plugin-jobs/status?id=<n>`

```bash
curl "http://crosspoint.local/api/plugin-jobs/status?id=3"
# -> {"id":3,"state":"done","result":{"title":"...","dest":"/Books/Book.epub"}}
```

States: `pending`, `running`, `done`, `error`, `unknown` (slot recycled —
poll promptly after completion).

### `GET /plugins-run`

Headless executor page: loads every plugin with its UI hidden so registered
actions run. Keep it open (browser tab or embedded webview) while jobs are
queued; jobs enqueued with no page open stay pending until one opens.
