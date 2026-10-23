# Install the READMAXER guest over Wi-Fi

1. Save the reader's Wi-Fi network in Settings.
2. Open **Settings > General > Install READMAXER** on the X4 Pro.
3. Wait for the screen to show its hostname and IP address.
4. Run the uploader from the repository root. The Mac needs Rust and curl.

```sh
READER_HOST=readmaxer-98c377bea840.local cargo run --locked --release --manifest-path tools/install-guest/Cargo.toml -- /path/to/x4pro-guest.bin
```

Use the hostname shown on your reader. `READER_HOST` defaults to `crosspoint.local`.
An IP address also works.

The tool copies the image to temporary storage and calculates its SHA-256.
It replaces only the staging files `/readmaxer/install.bin` and `/readmaxer/install.json`.
It uploads both files through `/upload` before it requests installation.
The reader checks the image, writes the other slot, checks the written bytes,
and selects the guest through the guarded otadata writer.

An `OK` response means the reader selected the verified guest and requested a restart.
It does not prove that the guest started. Check the reader's screen.
`receipt_saved` reports whether the reader saved `/readmaxer/last-install.json`.
The receipt contains the image SHA-256, result, and Unix time from the reader's clock.
The time can be unset if the reader has no valid clock.

If the install response is lost, the result is unconfirmed.
The tool reads the saved receipt once and prints it if the reader is reachable.
The receipt can be from an earlier install. Compare its SHA-256 with the intended image.
Do not repeat the install automatically. Check the reader first.
The image must target this reader's MAC address and use the project name `readmaxer-xteink-x4pro`.
The installer refuses an unconfirmed running slot. Open Home before trying again.
Wi-Fi failure returns an error. There is no USB fallback.

Run the host tool tests with:

```sh
cargo test --locked --manifest-path tools/install-guest/Cargo.toml
```
