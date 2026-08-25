use std::path::{Path, PathBuf};

use axum::{
    body::Body,
    extract::{Path as AxumPath, State},
    http::{header, HeaderValue, StatusCode},
    response::{IntoResponse, Redirect, Response},
};
use tokio::{
    fs::File,
    io::{AsyncReadExt, AsyncSeekExt, SeekFrom},
};
use tokio_util::io::ReaderStream;

use crate::AppState;

#[derive(Clone)]
pub struct UpdateRoot {
    root: PathBuf,
}

impl UpdateRoot {
    pub fn open(path: impl AsRef<Path>) -> std::io::Result<Self> {
        let root = std::fs::canonicalize(path)?;
        if !root.is_dir() {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "update root is not a directory",
            ));
        }
        Ok(Self { root })
    }

    fn exact_file(&self, name: &str) -> Option<PathBuf> {
        let path = self.root.join(name);
        safe_file(&self.root, &path).then_some(path)
    }

    fn package_file(&self, filename: &str) -> Option<PathBuf> {
        if !valid_filename(filename) {
            return None;
        }
        let path = self.root.join(filename);
        safe_file(&self.root, &path).then_some(path)
    }

    fn release_file(&self, version: &str, filename: &str) -> Option<PathBuf> {
        if !valid_version(version) || !valid_filename(filename) {
            return None;
        }
        let path = self.root.join("releases").join(version).join(filename);
        safe_file(&self.root, &path).then_some(path)
    }
}

fn safe_file(root: &Path, path: &Path) -> bool {
    (match std::fs::symlink_metadata(path) {
        Ok(metadata) => metadata.is_file() && !metadata.file_type().is_symlink(),
        Err(_) => false,
    }) && std::fs::canonicalize(path).is_ok_and(|canonical| canonical.starts_with(root))
}

fn valid_version(value: &str) -> bool {
    let digits = value.strip_prefix('v').unwrap_or(value);
    !digits.is_empty()
        && digits
            .split('.')
            .all(|part| !part.is_empty() && part.bytes().all(|byte| byte.is_ascii_digit()))
}

fn valid_filename(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 255
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
        && (value.ends_with(".zip")
            || value.ends_with(".deb")
            || value.ends_with(".rpm")
            || value.ends_with(".AppImage")
            || value.ends_with(".exe"))
}

fn root(state: &AppState) -> Result<&UpdateRoot, StatusCode> {
    state.update_root.as_deref().ok_or(StatusCode::NOT_FOUND)
}

async fn small_file(
    state: AppState,
    name: &'static str,
    content_type: &'static str,
    cache: &'static str,
    csp: Option<&'static str>,
) -> Response {
    let Some(path) = root(&state).ok().and_then(|root| root.exact_file(name)) else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let Ok(bytes) = tokio::fs::read(path).await else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let mut response = Response::new(Body::from(bytes));
    response
        .headers_mut()
        .insert(header::CONTENT_TYPE, HeaderValue::from_static(content_type));
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static(cache));
    response.headers_mut().insert(
        "x-content-type-options",
        HeaderValue::from_static("nosniff"),
    );
    if let Some(csp) = csp {
        response
            .headers_mut()
            .insert("content-security-policy", HeaderValue::from_static(csp));
    }
    response
}

pub async fn manifest(State(state): State<AppState>) -> Response {
    small_file(
        state,
        "version.json",
        "application/json",
        "no-cache, must-revalidate",
        None,
    )
    .await
}

pub async fn checksums(State(state): State<AppState>) -> Response {
    small_file(
        state,
        "SHA256SUMS.txt",
        "text/plain; charset=utf-8",
        "no-cache, must-revalidate",
        None,
    )
    .await
}

pub async fn page(State(state): State<AppState>) -> Response {
    small_file(
        state,
        "update.html",
        "text/html; charset=utf-8",
        "no-cache",
        Some("default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'"),
    )
    .await
}

pub async fn legacy_page() -> Redirect {
    Redirect::permanent("/update.html")
}

pub async fn package(
    State(state): State<AppState>,
    AxumPath(filename): AxumPath<String>,
    headers: axum::http::HeaderMap,
) -> Response {
    let Some(path) = root(&state)
        .ok()
        .and_then(|root| root.package_file(&filename))
    else {
        return StatusCode::NOT_FOUND.into_response();
    };
    serve_package(path, filename, headers).await
}

pub async fn release(
    State(state): State<AppState>,
    AxumPath((version, filename)): AxumPath<(String, String)>,
    headers: axum::http::HeaderMap,
) -> Response {
    let Some(path) = root(&state)
        .ok()
        .and_then(|root| root.release_file(&version, &filename))
    else {
        return StatusCode::NOT_FOUND.into_response();
    };
    serve_package(path, filename, headers).await
}

async fn serve_package(
    path: PathBuf,
    filename: String,
    headers: axum::http::HeaderMap,
) -> Response {
    let size = match std::fs::metadata(&path) {
        Ok(metadata) => metadata.len(),
        Err(_) => return StatusCode::NOT_FOUND.into_response(),
    };
    let range = match parse_range(
        headers
            .get(header::RANGE)
            .and_then(|value| value.to_str().ok()),
        size,
    ) {
        Ok(range) => range,
        Err(()) => return range_not_satisfiable(size),
    };
    let (start, end, partial) = range.unwrap_or((0, size.saturating_sub(1), false));
    let length = if size == 0 { 0 } else { end - start + 1 };
    let mut file = match File::open(path).await {
        Ok(file) => file,
        Err(_) => return StatusCode::NOT_FOUND.into_response(),
    };
    if start > 0 && file.seek(SeekFrom::Start(start)).await.is_err() {
        return StatusCode::NOT_FOUND.into_response();
    }
    let stream = ReaderStream::new(file.take(length));
    let mut response = Response::new(Body::from_stream(stream));
    *response.status_mut() = if partial {
        StatusCode::PARTIAL_CONTENT
    } else {
        StatusCode::OK
    };
    let headers = response.headers_mut();
    headers.insert(
        header::CONTENT_TYPE,
        HeaderValue::from_static("application/octet-stream"),
    );
    headers.insert(
        header::CONTENT_LENGTH,
        HeaderValue::from_str(&length.to_string()).unwrap(),
    );
    headers.insert(
        header::CONTENT_DISPOSITION,
        HeaderValue::from_str(&format!("attachment; filename=\"{filename}\"")).unwrap(),
    );
    headers.insert(
        header::CACHE_CONTROL,
        HeaderValue::from_static("public, max-age=31536000, immutable"),
    );
    headers.insert(header::ACCEPT_RANGES, HeaderValue::from_static("bytes"));
    headers.insert(
        "x-content-type-options",
        HeaderValue::from_static("nosniff"),
    );
    if partial {
        headers.insert(
            header::CONTENT_RANGE,
            HeaderValue::from_str(&format!("bytes {start}-{end}/{size}")).unwrap(),
        );
    }
    response
}

fn parse_range(value: Option<&str>, size: u64) -> Result<Option<(u64, u64, bool)>, ()> {
    let Some(value) = value else {
        return Ok(None);
    };
    let value = value.strip_prefix("bytes=").ok_or(())?;
    if value.contains(',') || size == 0 {
        return Err(());
    }
    let (start, end) = value.split_once('-').ok_or(())?;
    if start.is_empty() {
        let suffix: u64 = end.parse().map_err(|_| ())?;
        if suffix == 0 {
            return Err(());
        }
        let length = suffix.min(size);
        return Ok(Some((size - length, size - 1, true)));
    }
    let start: u64 = start.parse().map_err(|_| ())?;
    if start >= size {
        return Err(());
    }
    let end = if end.is_empty() {
        size - 1
    } else {
        end.parse::<u64>().map_err(|_| ())?.min(size - 1)
    };
    if end < start {
        return Err(());
    }
    Ok(Some((start, end, true)))
}

fn range_not_satisfiable(size: u64) -> Response {
    let mut response = StatusCode::RANGE_NOT_SATISFIABLE.into_response();
    response.headers_mut().insert(
        header::CONTENT_RANGE,
        HeaderValue::from_str(&format!("bytes */{size}")).unwrap(),
    );
    response
}
