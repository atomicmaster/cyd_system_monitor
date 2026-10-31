// SPDX-License-Identifier: GPL-3.0-only

//! The thin IO under the hook adapters: read one listed config file (bounded),
//! back it up, and replace it atomically. Paths are always under the `home`
//! the caller passes, so tests use a fake home and never the real one.

use std::fs::{self, DirBuilder, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use super::adapters::Edit;
use super::json::MAX_BYTES;

/// Mode of a config file we create.
const NEW_FILE_MODE: u32 = 0o644;

/// The file's text, or `None` when there is no such file. A file that is too
/// large or not UTF-8 is an error ("config unreadable"), never truncated.
pub fn read_config(path: &Path) -> Result<Option<String>, String> {
    let file = match fs::File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("cannot read {}: {e}", path.display())),
    };
    let mut bytes = Vec::new();
    file.take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    if bytes.len() > MAX_BYTES {
        return Err(format!(
            "{} is larger than {MAX_BYTES} bytes",
            path.display()
        ));
    }
    String::from_utf8(bytes)
        .map(Some)
        .map_err(|_| format!("{} is not UTF-8", path.display()))
}

/// `YYYYMMDDHHMMSS` in UTC for seconds since the Unix epoch.
pub fn timestamp(unix_seconds: u64) -> String {
    let days = (unix_seconds / 86_400) as i64;
    let rest = unix_seconds % 86_400;
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}{month:02}{day:02}{:02}{:02}{:02}",
        rest / 3_600,
        rest % 3_600 / 60,
        rest % 60
    )
}

/// What `apply` did.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Applied {
    pub backup: Option<PathBuf>,
}

fn io_note(path: &Path, what: &str, e: io::Error) -> String {
    format!("cannot {what} {}: {e}", path.display())
}

/// Backs up, then writes or deletes, `path`. Parent directories below
/// `config_dir` are created `0700`; `config_dir` itself must already exist
/// (an agent that is not installed never gets a directory).
pub fn apply(path: &Path, edit: &Edit, config_dir: &Path, stamp: &str) -> Result<Applied, String> {
    if *edit == Edit::Unchanged {
        return Ok(Applied::default());
    }
    // A config kept as a symlink (a dotfiles repository) is edited in place.
    let path = match fs::symlink_metadata(path) {
        Ok(meta) if meta.file_type().is_symlink() => {
            fs::canonicalize(path).map_err(|e| io_note(path, "resolve", e))?
        }
        _ => path.to_path_buf(),
    };
    let existing = fs::metadata(&path).ok();
    let backup = match &existing {
        Some(_) => Some(backup(&path, stamp)?),
        None => None,
    };
    match edit {
        Edit::Unchanged => {}
        Edit::Delete => match fs::remove_file(&path) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(io_note(&path, "delete", e)),
        },
        Edit::Write(text) => {
            let dir = path.parent().ok_or("config file has no directory")?;
            if !dir.is_dir() {
                if !config_dir.is_dir() {
                    return Err(format!("{} does not exist", config_dir.display()));
                }
                DirBuilder::new()
                    .recursive(true)
                    .mode(0o700)
                    .create(dir)
                    .map_err(|e| io_note(dir, "create", e))?;
            }
            let mode = existing
                .as_ref()
                .map_or(NEW_FILE_MODE, |m| m.permissions().mode() & 0o7777);
            write_atomic(&path, text, mode)?;
        }
    }
    Ok(Applied { backup })
}

fn backup(path: &Path, stamp: &str) -> Result<PathBuf, String> {
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    for attempt in 0..100 {
        let suffix = if attempt == 0 {
            String::new()
        } else {
            format!("-{attempt}")
        };
        let target = path.with_file_name(format!("{name}.bak-cyd-{stamp}{suffix}"));
        let mut source = fs::File::open(path).map_err(|e| io_note(path, "back up", e))?;
        let mode = source
            .metadata()
            .map_or(NEW_FILE_MODE, |m| m.permissions().mode() & 0o7777);
        match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(mode)
            .open(&target)
        {
            Ok(mut out) => {
                io::copy(&mut source, &mut out).map_err(|e| io_note(&target, "write", e))?;
                return Ok(target);
            }
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(io_note(&target, "write", e)),
        }
    }
    Err(format!("too many backups of {}", path.display()))
}

/// Temp file in the same directory, then rename over the target.
fn write_atomic(path: &Path, text: &str, mode: u32) -> Result<(), String> {
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let temp = path.with_file_name(format!(".{name}.cyd-tmp-{}", std::process::id()));
    let written = (|| -> io::Result<()> {
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(mode)
            .open(&temp)?;
        file.write_all(text.as_bytes())?;
        // The umask may have trimmed the mode; set it exactly.
        file.set_permissions(fs::Permissions::from_mode(mode))?;
        file.sync_all()?;
        fs::rename(&temp, path)
    })();
    written.map_err(|e| {
        let _ = fs::remove_file(&temp);
        io_note(path, "write", e)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("cyd-files-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn mode(path: &Path) -> u32 {
        fs::metadata(path).unwrap().permissions().mode() & 0o7777
    }

    fn names(dir: &Path) -> Vec<String> {
        let mut all: Vec<String> = fs::read_dir(dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        all.sort();
        all
    }

    #[test]
    fn timestamps_are_utc_civil_time() {
        assert_eq!(timestamp(0), "19700101000000");
        assert_eq!(timestamp(951_782_400), "20000229000000");
        assert_eq!(timestamp(1_782_864_000 + 3_661), "20260701010101");
        assert_eq!(timestamp(4_102_444_799), "20991231235959");
    }

    #[test]
    fn reading_is_bounded_and_strict() {
        let dir = scratch("read");
        let file = dir.join("c.json");
        assert_eq!(read_config(&file), Ok(None));
        fs::write(&file, "{}").unwrap();
        assert_eq!(read_config(&file), Ok(Some("{}".to_string())));
        fs::write(&file, vec![b' '; MAX_BYTES + 1]).unwrap();
        assert!(read_config(&file).unwrap_err().contains("larger"));
        fs::write(&file, [0xff, 0xfe]).unwrap();
        assert!(read_config(&file).unwrap_err().contains("UTF-8"));
        assert!(read_config(&dir).is_err(), "a directory is not a config");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_write_backs_up_keeps_the_mode_and_leaves_no_temp_file() {
        let dir = scratch("write");
        let file = dir.join("settings.json");
        fs::write(&file, "old").unwrap();
        fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
        let applied = apply(&file, &Edit::Write("new".into()), &dir, "20260101000000").unwrap();
        assert_eq!(fs::read_to_string(&file).unwrap(), "new");
        assert_eq!(mode(&file), 0o600);
        let backup = applied.backup.unwrap();
        assert_eq!(
            backup.file_name().unwrap(),
            "settings.json.bak-cyd-20260101000000"
        );
        assert_eq!(fs::read_to_string(&backup).unwrap(), "old");
        assert_eq!(
            names(&dir),
            ["settings.json", "settings.json.bak-cyd-20260101000000"]
        );
        // The same second again does not overwrite the first backup.
        fs::write(&file, "older").unwrap();
        let second = apply(&file, &Edit::Write("newer".into()), &dir, "20260101000000").unwrap();
        assert_eq!(
            second.backup.unwrap().file_name().unwrap(),
            "settings.json.bak-cyd-20260101000000-1"
        );
        assert_eq!(fs::read_to_string(&backup).unwrap(), "old");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_new_file_needs_no_backup_and_unchanged_does_nothing() {
        let dir = scratch("new");
        let file = dir.join("hooks.json");
        assert_eq!(
            apply(&file, &Edit::Unchanged, &dir, "s"),
            Ok(Applied::default())
        );
        assert!(!file.exists());
        let applied = apply(&file, &Edit::Write("x".into()), &dir, "s").unwrap();
        assert_eq!(applied.backup, None);
        assert_eq!(mode(&file), NEW_FILE_MODE);
        assert_eq!(names(&dir), ["hooks.json"]);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn delete_backs_up_first() {
        let dir = scratch("delete");
        let file = dir.join("p.js");
        fs::write(&file, "js").unwrap();
        let applied = apply(&file, &Edit::Delete, &dir, "20260101000000").unwrap();
        assert!(!file.exists());
        assert_eq!(fs::read_to_string(applied.backup.unwrap()).unwrap(), "js");
        assert_eq!(
            apply(&file, &Edit::Delete, &dir, "s"),
            Ok(Applied::default())
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn parent_directories_are_created_private_only_below_an_existing_config_dir() {
        let home = scratch("parents");
        let config = home.join(".grok");
        let file = config.join("hooks/cyd-system-monitor.json");
        // No ~/.grok: nothing is created.
        let error = apply(&file, &Edit::Write("x".into()), &config, "s").unwrap_err();
        assert!(error.contains("does not exist"), "{error}");
        assert!(!config.exists());
        fs::create_dir(&config).unwrap();
        apply(&file, &Edit::Write("x".into()), &config, "s").unwrap();
        assert_eq!(mode(&config.join("hooks")), 0o700);
        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn a_symlinked_config_is_edited_through_the_link() {
        let dir = scratch("link");
        let real = dir.join("dotfiles/settings.json");
        fs::create_dir_all(real.parent().unwrap()).unwrap();
        fs::write(&real, "old").unwrap();
        let link = dir.join("settings.json");
        std::os::unix::fs::symlink(&real, &link).unwrap();
        apply(&link, &Edit::Write("new".into()), &dir, "s").unwrap();
        assert!(fs::symlink_metadata(&link)
            .unwrap()
            .file_type()
            .is_symlink());
        assert_eq!(fs::read_to_string(&real).unwrap(), "new");
        let _ = fs::remove_dir_all(&dir);
    }
}
