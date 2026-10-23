use serde_json::Value;
use sha2::{Digest, Sha256};
use std::error::Error;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

struct Staging(PathBuf);

impl Staging {
    fn create() -> Result<Self> {
        let root = std::env::temp_dir();
        for suffix in 0..100 {
            let path = root.join(format!("readmaxer-install-{}-{suffix}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error.into()),
            }
        }
        Err("Cannot create an install staging directory".into())
    }
}

impl Drop for Staging {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

struct Response {
    status: u16,
    body: String,
}

fn exchange(host: &str, route: &str, args: &[String]) -> Result<Response> {
    let output = Command::new("curl")
        .args([
            "--silent",
            "--show-error",
            "--write-out",
            "\n%{http_code}",
            "--connect-timeout",
            "10",
            "--max-time",
            "300",
            "--noproxy",
            "*",
            "--proto",
            "=http",
        ])
        .args(args)
        .arg(format!("http://{host}{route}"))
        .output()?;
    let body = String::from_utf8_lossy(&output.stdout).into_owned();
    if !output.status.success() {
        return Err(format!(
            "{route}: {body} {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }
    let (body, status) = body.rsplit_once('\n').ok_or("Missing HTTP status")?;
    Ok(Response {
        status: status.parse()?,
        body: body.to_owned(),
    })
}

fn request(host: &str, route: &str, args: &[String]) -> Result<String> {
    let response = exchange(host, route, args)?;
    if !(200..300).contains(&response.status) {
        return Err(format!("{route}: HTTP {}: {}", response.status, response.body).into());
    }
    Ok(response.body)
}

fn validate_host(host: &str) -> Result<()> {
    if host.is_empty()
        || host.starts_with('-')
        || host
            .chars()
            .any(|c| c.is_whitespace() || "/?#@\\".contains(c))
    {
        return Err("READER_HOST must be a hostname or IP address, with an optional port".into());
    }
    Ok(())
}

fn command_result(host: &str, route: &str, action: &str, args: &[String]) -> Result<String> {
    let response = exchange(host, route, args)
        .map_err(|error| format!("{action} is unconfirmed. Check the reader. {error}"))?;
    let result: Value = serde_json::from_str(&response.body)
        .map_err(|_| format!("{action} is unconfirmed. Check the reader. Invalid JSON response"))?;
    let code = result["result"]
        .as_str()
        .ok_or_else(|| format!("{action} is unconfirmed. Check the reader. Missing result code"))?;
    if code == "SELECTION_FAILED" {
        return Err(format!(
            "{action} is unconfirmed. Check the reader. {}",
            response.body
        )
        .into());
    }
    if (400..500).contains(&response.status) {
        return Err(format!("Reader refused {action}: {code}").into());
    }
    if response.status != 200 || code != "OK" {
        return Err(format!(
            "{action} is unconfirmed. Check the reader. {}",
            response.body
        )
        .into());
    }
    Ok(response.body)
}

fn start(host: &str) -> Result<String> {
    command_result(
        host,
        "/api/readmaxer/start",
        "Start",
        &["--request".into(), "POST".into()],
    )
}

fn state(host: &str) -> Result<String> {
    request(host, "/api/readmaxer/state", &[])
}

fn input(host: &str, button: &str) -> Result<String> {
    if !["confirm", "back", "next", "previous"].contains(&button) {
        return Err("Button must be confirm, back, next, or previous".into());
    }
    command_result(
        host,
        "/api/input",
        "Input",
        &["--data".into(), format!("button={button}")],
    )
}

fn download(host: &str, route: &str, target: &Path, missing_ok: bool) -> Result<bool> {
    let staging = Staging::create()?;
    let staged = staging.0.join("response");
    let output = Command::new("curl")
        .args([
            "--silent",
            "--show-error",
            "--write-out",
            "%{http_code}",
            "--connect-timeout",
            "10",
            "--max-time",
            "300",
            "--noproxy",
            "*",
            "--proto",
            "=http",
            "--output",
        ])
        .arg(&staged)
        .arg(format!("http://{host}{route}"))
        .output()?;
    if !output.status.success() {
        return Err(format!(
            "{route}: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }
    let status: u16 = String::from_utf8(output.stdout)?.parse()?;
    if status == 404 && missing_ok {
        return Ok(false);
    }
    if status != 200 {
        return Err(format!("{route}: HTTP {status}").into());
    }
    fs::copy(staged, target)?;
    Ok(true)
}

fn fetch_diag(host: &str, dir: &Path) -> Result<String> {
    fs::create_dir_all(dir)?;
    let mut report = Vec::new();
    for name in ["diag.txt", "diag-last.txt"] {
        let route = format!("/download?path=/readmaxer/{name}");
        let found = download(host, &route, &dir.join(name), true)?;
        report.push(format!(
            "{name}: {}",
            if found { "saved" } else { "missing" }
        ));
    }
    Ok(report.join("\n"))
}

fn screen(host: &str, output: &Path) -> Result<String> {
    download(host, "/api/screen", output, false)?;
    let mut file = File::open(output)?;
    let mut magic = [0u8; 3];
    if file.read_exact(&mut magic).is_err() || &magic != b"P4\n" {
        fs::remove_file(output)?;
        return Err("Reader returned an invalid PBM".into());
    }
    Ok(format!("Saved {}", output.display()))
}

fn wait(host: &str, timeout: Duration) -> Result<String> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Ok(response) = exchange(
            host,
            "/api/status",
            &[
                "--connect-timeout".into(),
                "2".into(),
                "--max-time".into(),
                "2".into(),
            ],
        ) {
            if response.status == 200 {
                return Ok("Reader is online".into());
            }
        }
        let now = Instant::now();
        if now >= deadline {
            return Err(
                format!("Reader did not answer within {} seconds", timeout.as_secs()).into(),
            );
        }
        thread::sleep(Duration::from_secs(1).min(deadline - now));
    }
}

fn upload_to(host: &str, path: &Path, name: &str, directory: &str) -> Result<()> {
    let path = path.to_str().ok_or("Image path must be valid UTF-8")?;
    let quoted = path.replace('\\', "\\\\").replace('"', "\\\"");
    request(
        host,
        &format!("/upload?path={directory}"),
        &[
            "--form".into(),
            format!("file=@\"{quoted}\";filename={name}"),
        ],
    )?;
    Ok(())
}

fn upload(host: &str, path: &Path, name: &str) -> Result<()> {
    upload_to(host, path, name, "/readmaxer")
}

fn wait_for_restart(host: &str, previous_uptime: u64, previous_version: &str) -> Result<String> {
    let deadline = Instant::now() + Duration::from_secs(120);
    let mut went_offline = false;
    loop {
        match exchange(
            host,
            "/api/status",
            &[
                "--connect-timeout".into(),
                "2".into(),
                "--max-time".into(),
                "2".into(),
            ],
        ) {
            Ok(response) if response.status == 200 => {
                if let Ok(status) = serde_json::from_str::<Value>(&response.body) {
                    if let (Some(version), Some(uptime)) =
                        (status["version"].as_str(), status["uptime"].as_u64())
                    {
                        if went_offline || uptime < previous_uptime || version != previous_version {
                            return Ok(format!("CrossPoint version: {version}"));
                        }
                    }
                }
            }
            _ => went_offline = true,
        }
        if Instant::now() >= deadline {
            return Err("Update is unconfirmed. Reader did not return after restart".into());
        }
        thread::sleep(Duration::from_secs(1));
    }
}

fn update_fork(host: &str, image: &Path) -> Result<String> {
    let before: Value = serde_json::from_str(&request(host, "/api/status", &[])?)?;
    let previous_version = before["version"].as_str().ok_or("Missing reader version")?;
    let previous_uptime = before["uptime"].as_u64().ok_or("Missing reader uptime")?;
    let staging = Staging::create()?;
    let snapshot = staging.0.join("fork.bin");
    fs::copy(image, &snapshot)?;
    File::open(&snapshot)?.sync_all()?;
    let nonce = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
    let name = format!("readmaxer-fork-{}-{nonce}.bin", std::process::id());
    upload_to(host, &snapshot, &name, "/")?;
    command_result(
        host,
        &format!("/api/readmaxer/update-fork?path=/{name}"),
        "Fork update",
        &["--request".into(), "POST".into()],
    )?;
    wait_for_restart(host, previous_uptime, previous_version)
}

fn unconfirmed(host: &str, detail: &str) -> Box<dyn Error> {
    let receipt = match request(host, "/download?path=/readmaxer/last-install.json", &[]) {
        Ok(body) => format!("Stored receipt, which can be from an earlier install: {body}"),
        Err(error) => format!("Receipt could not be read: {error}"),
    };
    format!(
        "Install is unconfirmed. Do not retry automatically. Check the reader. {detail}\n{receipt}"
    )
    .into()
}

fn install(host: &str, image: &Path) -> Result<String> {
    validate_host(host)?;
    let staging = Staging::create()?;
    // Hash and upload one immutable snapshot even if the build replaces its output.
    let image_copy = staging.0.join("install.bin");
    let mut source = File::open(image)?;
    let mut target = File::create(&image_copy)?;
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let len = source.read(&mut buffer)?;
        if len == 0 {
            break;
        }
        target.write_all(&buffer[..len])?;
        hash.update(&buffer[..len]);
    }
    target.sync_all()?;
    drop(target);
    let sha = format!("{:x}", hash.finalize());
    let manifest = staging.0.join("install.json");
    fs::write(&manifest, format!("{{\"sha256\":\"{sha}\"}}\n"))?;

    let files: Value = serde_json::from_str(&request(host, "/api/files?path=/readmaxer", &[])?)?;
    let files = files.as_array().ok_or("Invalid file listing from reader")?;
    for name in ["install.bin", "install.json"] {
        if let Some(entry) = files.iter().find(|entry| entry["name"] == name) {
            if entry["isDirectory"] != false {
                return Err(format!("Refusing to delete a directory named {name}").into());
            }
            request(
                host,
                "/delete",
                &["--data".into(), format!("path=/readmaxer/{name}")],
            )?;
        }
    }
    upload(host, &image_copy, "install.bin")?;
    upload(host, &manifest, "install.json")?;
    let response = exchange(
        host,
        "/api/readmaxer/install",
        &["--request".into(), "POST".into()],
    )
    .map_err(|error| unconfirmed(host, &error.to_string()))?;
    let result: Value = serde_json::from_str(&response.body)
        .map_err(|_| unconfirmed(host, "Invalid install response."))?;
    let Some(code) = result["result"].as_str().filter(|code| !code.is_empty()) else {
        return Err(unconfirmed(host, "Invalid install result."));
    };
    if code == "SELECTION_UNKNOWN" {
        return Err(unconfirmed(host, &response.body));
    }
    if response.status == 409 && code != "OK" {
        return Err(format!("Reader reported an install error: {}", response.body).into());
    }
    if response.status != 200 || code != "OK" || result["sha256"] != sha {
        return Err(unconfirmed(host, &response.body));
    }
    Ok(response.body)
}

fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let host = std::env::var("READER_HOST").unwrap_or_else(|_| "crosspoint.local".into());
    let result = (|| -> Result<String> {
        validate_host(&host)?;
        match args.first().and_then(|arg| arg.to_str()) {
            Some("install") if args.len() == 2 => install(&host, Path::new(&args[1])),
            Some("start") if args.len() == 1 => start(&host),
            Some("state") if args.len() == 1 => state(&host),
            Some("fetch-diag") if args.len() == 2 => fetch_diag(&host, Path::new(&args[1])),
            Some("input") if args.len() == 2 => input(&host, args[1].to_str().ok_or("Invalid button")?),
            Some("screen") if args.len() == 2 => screen(&host, Path::new(&args[1])),
            Some("update-fork") if args.len() == 2 => update_fork(&host, Path::new(&args[1])),
            Some("wait") if args.len() <= 2 => {
                let seconds = if let Some(value) = args.get(1) {
                    value.to_str().ok_or("Invalid timeout")?.parse::<u64>()?
                } else {
                    60
                };
                wait(&host, Duration::from_secs(seconds))
            }
            _ => Err("Usage: install-guest install IMAGE.bin | update-fork IMAGE.bin | start | state | fetch-diag DIR | wait [SECONDS] | input BUTTON | screen OUT.pbm".into()),
        }
    })();
    match result {
        Ok(response) => println!("{response}"),
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    fn serve<T>(responses: Vec<(u16, Vec<u8>)>, call: impl FnOnce(&str) -> T) -> (T, Vec<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let host = listener.local_addr().unwrap().to_string();
        let server = thread::spawn(move || {
            let mut requests = Vec::new();
            for (code, body) in responses {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(10)))
                    .unwrap();
                let mut bytes = Vec::new();
                loop {
                    let mut byte = [0];
                    stream.read_exact(&mut byte).unwrap();
                    bytes.push(byte[0]);
                    if bytes.ends_with(b"\r\n\r\n") {
                        break;
                    }
                }
                let headers = String::from_utf8_lossy(&bytes);
                let length = headers
                    .lines()
                    .find_map(|line| {
                        let (key, value) = line.split_once(':')?;
                        key.eq_ignore_ascii_case("content-length")
                            .then(|| value.trim().parse::<usize>().unwrap())
                    })
                    .unwrap_or(0);
                let header_end = bytes.len();
                bytes.resize(header_end + length, 0);
                stream.read_exact(&mut bytes[header_end..]).unwrap();
                requests.push(String::from_utf8_lossy(&bytes).into_owned());
                write!(
                    stream,
                    "HTTP/1.1 {code} Test\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                )
                .unwrap();
                stream.write_all(&body).unwrap();
            }
            requests
        });
        let result = call(&host);
        (result, server.join().unwrap())
    }

    fn exercise(
        fail_upload: bool,
        lose_install_reply: bool,
        refuse_install: bool,
    ) -> (Result<String>, Vec<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let host = listener.local_addr().unwrap().to_string();
        let server = thread::spawn(move || {
            let mut requests = Vec::new();
            let count = if fail_upload {
                2
            } else if lose_install_reply {
                5
            } else {
                4
            };
            for stage in 0..count {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(10)))
                    .unwrap();
                let mut bytes = Vec::new();
                let header_end;
                loop {
                    let mut byte = [0];
                    stream.read_exact(&mut byte).unwrap();
                    bytes.push(byte[0]);
                    if bytes.ends_with(b"\r\n\r\n") {
                        header_end = bytes.len();
                        break;
                    }
                }
                let headers = String::from_utf8_lossy(&bytes);
                let length = headers
                    .lines()
                    .find_map(|line| {
                        let (key, value) = line.split_once(':')?;
                        key.eq_ignore_ascii_case("content-length")
                            .then(|| value.trim().parse::<usize>().unwrap())
                    })
                    .unwrap_or(0);
                bytes.resize(header_end + length, 0);
                stream.read_exact(&mut bytes[header_end..]).unwrap();
                requests.push(String::from_utf8_lossy(&bytes).into_owned());
                if stage == 3 && lose_install_reply {
                    continue;
                }
                let (code, body) = if stage == 0 {
                    (200, "[]".to_owned())
                } else if fail_upload {
                    (500, "SD write failed".to_owned())
                } else if stage == 3 && refuse_install {
                    (409, "{\"result\":\"WRONG_SHA\"}".to_owned())
                } else if stage >= 3 {
                    (
                        200,
                        format!(
                            "{{\"result\":\"OK\",\"sha256\":\"{:x}\",\"receipt_saved\":true}}",
                            Sha256::digest(b"guest fixture")
                        ),
                    )
                } else {
                    (200, "uploaded".to_owned())
                };
                write!(
                    stream,
                    "HTTP/1.1 {code} Test\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                    body.len()
                )
                .unwrap();
            }
            requests
        });
        let staging = Staging::create().unwrap();
        let image = staging.0.join("guest \"file\".bin");
        fs::write(&image, b"guest fixture").unwrap();
        let result = install(&host, &image);
        (result, server.join().unwrap())
    }

    #[test]
    fn uploads_image_and_matching_json_before_install() {
        let (result, requests) = exercise(false, false, false);
        assert!(result.is_ok(), "{result:?}");
        assert!(requests[1].starts_with("POST /upload?path=/readmaxer "));
        assert!(requests[1].contains("filename=\"install.bin\""));
        assert!(requests[1].contains("guest fixture"));
        assert!(requests[2].contains(&format!("{:x}", Sha256::digest(b"guest fixture"))));
        assert!(requests[3].starts_with("POST /api/readmaxer/install "));
    }

    #[test]
    fn upload_failure_never_calls_install() {
        let (result, requests) = exercise(true, false, false);
        assert!(result.unwrap_err().to_string().contains("SD write failed"));
        assert_eq!(requests.len(), 2);
    }

    #[test]
    fn lost_install_response_is_unconfirmed() {
        let (result, requests) = exercise(false, true, false);
        let error = result.unwrap_err().to_string();
        assert!(error.contains("Install is unconfirmed"));
        assert!(error.contains("Stored receipt"));
        assert!(error.contains("\"result\":\"OK\""));
        assert_eq!(requests.len(), 5);
        assert!(requests[4].starts_with("GET /download?path=/readmaxer/last-install.json "));
    }

    #[test]
    fn complete_http_409_is_a_definite_refusal() {
        let (result, requests) = exercise(false, false, true);
        let error = result.unwrap_err().to_string();
        assert!(error.contains("WRONG_SHA"));
        assert!(!error.contains("unconfirmed"));
        assert_eq!(requests.len(), 4);
    }

    #[test]
    fn start_and_state_use_install_screen_routes() {
        let (result, requests) = serve(
            vec![
                (200, br#"{"running_slot":"ota_0","guest_installed":true,"guest_version":"1.2.3","devloop":true}"#.to_vec()),
                (200, br#"{"result":"OK"}"#.to_vec()),
                (200, br#"{"result":"OK"}"#.to_vec()),
            ],
            |host| (state(host), start(host), input(host, "next")),
        );
        assert!(result.0.unwrap().contains("\"guest_version\":\"1.2.3\""));
        assert_eq!(result.1.unwrap(), r#"{"result":"OK"}"#);
        assert!(result.2.is_ok());
        assert!(requests[0].starts_with("GET /api/readmaxer/state "));
        assert!(requests[1].starts_with("POST /api/readmaxer/start "));
        assert!(requests[2].starts_with("POST /api/input "));
        assert!(requests[2].contains("button=next"));
    }

    #[test]
    fn start_refusal_is_definite_and_input_pending_is_reported() {
        let (result, requests) = serve(
            vec![
                (409, br#"{"result":"NO_GUEST"}"#.to_vec()),
                (409, br#"{"result":"CLICK_PENDING"}"#.to_vec()),
            ],
            |host| (start(host), input(host, "back")),
        );
        assert!(result.0.unwrap_err().to_string().contains("NO_GUEST"));
        assert!(result.1.unwrap_err().to_string().contains("CLICK_PENDING"));
        assert!(requests[1].starts_with("POST /api/input "));
        assert!(requests[1].contains("button=back"));
    }

    #[test]
    fn fetch_diag_keeps_found_file_and_tolerates_missing_file() {
        let staging = Staging::create().unwrap();
        let (result, requests) = serve(
            vec![(200, b"diagnostic\n".to_vec()), (404, b"missing".to_vec())],
            |host| fetch_diag(host, &staging.0),
        );
        assert!(result.unwrap().contains("diag-last.txt: missing"));
        assert_eq!(
            fs::read(staging.0.join("diag.txt")).unwrap(),
            b"diagnostic\n"
        );
        assert!(!staging.0.join("diag-last.txt").exists());
        assert!(requests[0].starts_with("GET /download?path=/readmaxer/diag.txt "));
        assert!(requests[1].starts_with("GET /download?path=/readmaxer/diag-last.txt "));
    }

    #[test]
    fn screen_saves_binary_pbm_and_wait_polls_until_ready() {
        let staging = Staging::create().unwrap();
        let output = staging.0.join("screen.pbm");
        let pbm = b"P4\n8 1\n\x80".to_vec();
        let (result, requests) = serve(
            vec![
                (200, pbm.clone()),
                (503, b"busy".to_vec()),
                (200, b"{}".to_vec()),
            ],
            |host| (screen(host, &output), wait(host, Duration::from_secs(4))),
        );
        assert!(result.0.is_ok(), "{:?}", result.0);
        assert!(result.1.is_ok(), "{:?}", result.1);
        assert_eq!(fs::read(output).unwrap(), pbm);
        assert!(requests[0].starts_with("GET /api/screen "));
        assert!(requests[1].starts_with("GET /api/status "));
        assert!(requests[2].starts_with("GET /api/status "));
    }

    #[test]
    fn wait_reports_timeout_after_reader_stays_unavailable() {
        let (result, requests) = serve(vec![(503, b"busy".to_vec())], |host| {
            wait(host, Duration::ZERO)
        });
        assert!(result.unwrap_err().to_string().contains("did not answer"));
        assert_eq!(requests.len(), 1);
    }

    #[test]
    fn failed_selection_is_unconfirmed_even_with_complete_http_409() {
        let (result, _) = serve(
            vec![(409, br#"{"result":"SELECTION_FAILED"}"#.to_vec())],
            start,
        );
        assert!(result.unwrap_err().to_string().contains("unconfirmed"));
    }

    #[test]
    fn closed_install_screen_is_a_definite_refusal() {
        let (result, _) = serve(
            vec![(403, br#"{"result":"OPEN_INSTALL_SCREEN"}"#.to_vec())],
            start,
        );
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("OPEN_INSTALL_SCREEN"));
    }

    #[test]
    fn fork_update_uploads_to_root_and_waits_for_new_boot() {
        let staging = Staging::create().unwrap();
        let image = staging.0.join("fork.bin");
        fs::write(&image, b"fork fixture").unwrap();
        let (result, requests) = serve(
            vec![
                (200, br#"{"version":"old","uptime":42}"#.to_vec()),
                (200, b"uploaded".to_vec()),
                (200, br#"{"result":"OK"}"#.to_vec()),
                (503, b"restarting".to_vec()),
                (200, br#"{"version":"new","uptime":1}"#.to_vec()),
            ],
            |host| update_fork(host, &image),
        );
        assert_eq!(result.unwrap(), "CrossPoint version: new");
        assert!(requests[1].starts_with("POST /upload?path=/ "));
        assert!(requests[1].contains("fork fixture"));
        assert!(requests[2].starts_with("POST /api/readmaxer/update-fork?path=/readmaxer-fork-"));
        assert!(requests[3].starts_with("GET /api/status "));
        assert!(requests[4].starts_with("GET /api/status "));
    }

    #[test]
    fn fork_update_refusal_does_not_wait_for_restart() {
        let staging = Staging::create().unwrap();
        let image = staging.0.join("fork.bin");
        fs::write(&image, b"fork fixture").unwrap();
        let (result, requests) = serve(
            vec![
                (200, br#"{"version":"old","uptime":42}"#.to_vec()),
                (200, b"uploaded".to_vec()),
                (409, br#"{"result":"WRONG_FORK"}"#.to_vec()),
            ],
            |host| update_fork(host, &image),
        );
        assert!(result.unwrap_err().to_string().contains("WRONG_FORK"));
        assert_eq!(requests.len(), 3);
    }
}
