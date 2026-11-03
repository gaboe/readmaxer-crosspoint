# Control the READMAXER guest over Wi-Fi

Known POC limit: the web server has no authentication. Any client on the joined
Wi-Fi can install or start the guest, update CrossPoint, send input, and read the screen. Use a
trusted network.

1. Save the reader's Wi-Fi network in Settings.
2. Open **Settings > General > Install READMAXER** on the X4 Pro.
3. Wait for the screen to show its hostname and IP address.
4. Run the tool from the repository root. The Mac needs Rust and curl.

```sh
READER_HOST=readmaxer-98c377bea840.local cargo run --locked --release --manifest-path tools/install-guest/Cargo.toml -- install /path/to/x4pro-guest.bin
```

Use the hostname shown on your reader. `READER_HOST` defaults to `crosspoint.local`.
An IP address also works.

The tool also accepts these commands after the install screen is open:

```sh
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- state
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- start
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- fetch-diag ./reader-diag
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- input back
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- screen ./screen.pbm
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- update-fork ./readmaxer-fork.bin
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- guest-log
cargo run --locked --manifest-path tools/install-guest/Cargo.toml -- wait 60
```

Set `READER_HOST` for each command, or export it once in the shell. `wait`
polls `/api/status` until the reader answers or the timeout expires. The
default timeout is 60 seconds. `fetch-diag` downloads `diag.txt` and
`diag-last.txt` from `/readmaxer`; a missing file is reported and skipped.
`screen` writes a binary PBM image in physical panel orientation.
`guest-log` prints the valid RTC log saved when the fork booted. It reports HTTP 404
if the fork found no valid guest log.
`update-fork` uploads a snapshot of the image to the SD root and calls
`POST /api/readmaxer/update-fork?path=/<file>.bin`. The route works only while
the Install READMAXER screen is open. It uses the Settings SD firmware update
checks, guarded write, readback, and boot selection. A complete HTTP 409
response means that the route refused the image before a flash write. A lost
response or an HTTP 500 result leaves the update unconfirmed. After `OK`, the
tool waits for a new boot and prints the version from `/api/status`. Check the
reader before another attempt if the result is unconfirmed. The uploaded file
stays on the SD root until you remove it.

To enter the install screen after every normal boot, turn on **Settings > System > DEV LOOP** on the X4 Pro
(OFF deletes the file again), or upload an empty `/readmaxer/devloop` file while a web server screen is open:

```sh
touch devloop
curl --noproxy '*' -F 'file=@devloop;filename=devloop' "http://$READER_HOST/upload?path=/readmaxer"
```

Remove the flag through `/delete` to restore normal boot routing:

```sh
curl --noproxy '*' -d 'path=/readmaxer/devloop' "http://$READER_HOST/delete"
```

The flag takes effect at the next boot. Recovery, crash reporting, and
first-boot slot confirmation still open their own screens. Back leaves the
install screen without removing the flag.

The tool copies the image to temporary storage and calculates its SHA-256.
It replaces only the staging files `/readmaxer/install.bin` and `/readmaxer/install.json`.
It uploads both files through `/upload` before it requests installation.
The reader checks the image, writes the other slot, checks the written bytes,
and selects the guest through the guarded otadata writer.
The tool prints image upload throughput and total install time. It retries an
upload up to three times if the connection fails with `No route to host` before
curl sends any bytes. It does not retry after the upload starts.

An `OK` response means the reader selected the verified guest and requested a restart.
It does not prove that the guest started. Check the reader's screen.
`receipt_saved` reports whether the reader saved `/readmaxer/last-install.json`.
The receipt contains the image SHA-256, result, Unix time, `timing_ms`, and `bytes`.
The install response has the same timing and byte counts.
The time can be unset if the reader has no valid clock.

A complete HTTP 409 JSON response reports a definite install error.
A refusal before the first erase changes no flash. A later write or readback failure
can leave a partial guest slot. It never selects that slot and keeps a CRC-valid
VALID otadata entry naming the bootable fork. A selection failure or unknown
readback after an otadata erase also keeps that fork entry through CP2's guarded plan.

If the install response is lost, malformed, or reports `SELECTION_UNKNOWN`, the result is unconfirmed.
The tool reads the saved receipt once and prints it if the reader is reachable.
The receipt can be from an earlier install. Compare its SHA-256 with the intended image.
Do not repeat the install automatically. Check the reader first.
The image must target this reader's MAC address and use the project name `readmaxer-xteink-x4pro`.
The installer refuses an unconfirmed running slot. Open Home before trying again.
Wi-Fi failure returns an error. There is no USB fallback.
`start` uses the same guarded guest switch as the Home READMAXER item.
`NO_GUEST` means no guest is installed. `GUARD_REFUSED` means the switch guard
refused without an otadata write. A lost response, `SELECTION_FAILED`, or
`SELECTION_UNKNOWN` leaves the boot result unconfirmed. Check the reader
before another start request.

Run the host tool tests with:

```sh
cargo test --locked --manifest-path tools/install-guest/Cargo.toml
```
