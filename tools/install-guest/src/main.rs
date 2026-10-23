use serde_json::Value;
use sha2::{Digest, Sha256};
use std::error::Error;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

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

fn request(host: &str, route: &str, args: &[String]) -> Result<String> {
    let output = Command::new("curl")
        .args([
            "--silent",
            "--show-error",
            "--fail-with-body",
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
    Ok(body)
}

fn upload(host: &str, path: &Path, name: &str) -> Result<()> {
    let path = path.to_str().ok_or("Image path must be valid UTF-8")?;
    let quoted = path.replace('\\', "\\\\").replace('"', "\\\"");
    request(
        host,
        "/upload?path=/readmaxer",
        &[
            "--form".into(),
            format!("file=@\"{quoted}\";filename={name}"),
        ],
    )?;
    Ok(())
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
    if host.is_empty()
        || host.starts_with('-')
        || host
            .chars()
            .any(|c| c.is_whitespace() || "/?#@\\".contains(c))
    {
        return Err("READER_HOST must be a hostname or IP address, with an optional port".into());
    }
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
    let response = request(
        host,
        "/api/readmaxer/install",
        &["--request".into(), "POST".into()],
    )
    .map_err(|error| unconfirmed(host, &error.to_string()))?;
    let result: Value = serde_json::from_str(&response)
        .map_err(|_| unconfirmed(host, "Invalid install response."))?;
    if result["result"] != "OK" || result["sha256"] != sha {
        return Err(unconfirmed(host, &response));
    }
    Ok(response)
}

fn main() {
    let mut args = std::env::args_os().skip(1);
    let Some(image) = args.next() else {
        eprintln!("Usage: install-guest IMAGE.bin\nOpen Settings > Install READMAXER first.\nREADER_HOST defaults to crosspoint.local.");
        std::process::exit(2);
    };
    if args.next().is_some() {
        eprintln!("Expected one IMAGE.bin path");
        std::process::exit(2);
    }
    let host = std::env::var("READER_HOST").unwrap_or_else(|_| "crosspoint.local".into());
    match install(&host, Path::new(&image)) {
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
    use std::thread;
    use std::time::Duration;

    fn exercise(fail_upload: bool, lose_install_reply: bool) -> (Result<String>, Vec<String>) {
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
        let (result, requests) = exercise(false, false);
        assert!(result.is_ok(), "{result:?}");
        assert!(requests[1].starts_with("POST /upload?path=/readmaxer "));
        assert!(requests[1].contains("filename=\"install.bin\""));
        assert!(requests[1].contains("guest fixture"));
        assert!(requests[2].contains(&format!("{:x}", Sha256::digest(b"guest fixture"))));
        assert!(requests[3].starts_with("POST /api/readmaxer/install "));
    }

    #[test]
    fn upload_failure_never_calls_install() {
        let (result, requests) = exercise(true, false);
        assert!(result.unwrap_err().to_string().contains("SD write failed"));
        assert_eq!(requests.len(), 2);
    }

    #[test]
    fn lost_install_response_is_unconfirmed() {
        let (result, requests) = exercise(false, true);
        let error = result.unwrap_err().to_string();
        assert!(error.contains("Install is unconfirmed"));
        assert!(error.contains("Stored receipt"));
        assert!(error.contains("\"result\":\"OK\""));
        assert_eq!(requests.len(), 5);
        assert!(requests[4].starts_with("GET /download?path=/readmaxer/last-install.json "));
    }
}
