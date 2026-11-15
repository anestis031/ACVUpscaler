//! Thin wrappers over the Win32 calls the tool needs: wide strings, ini files, SHA-256, registry,
//! process list, download.

use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};

use windows_sys::Win32::Foundation::{CloseHandle, ERROR_SUCCESS, INVALID_HANDLE_VALUE};
use windows_sys::Win32::Security::Cryptography::{
    BCryptCloseAlgorithmProvider, BCryptHash, BCryptOpenAlgorithmProvider, BCRYPT_ALG_HANDLE, BCRYPT_SHA256_ALGORITHM,
};
use windows_sys::Win32::System::Com::Urlmon::URLDownloadToFileW;
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ};
use windows_sys::Win32::System::WindowsProgramming::{GetPrivateProfileStringW, WritePrivateProfileStringW};
use windows_sys::Win32::UI::Shell::SHGetFolderPathW;

/// UTF-16 with a terminating zero
pub fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

pub fn wide_path(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str().encode_wide().chain(std::iter::once(0)).collect()
}

pub fn from_wide(buffer: &[u16]) -> String {
    let length = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
    String::from_utf16_lossy(&buffer[..length])
}

/// Value of a key in an ini file, empty when the key or the file is missing
pub fn ini_get(file: &Path, section: &str, key: &str) -> String {
    let mut buffer = [0u16; 1024];
    let (file, section, key, empty) = (wide_path(file), wide(section), wide(key), wide(""));
    unsafe {
        GetPrivateProfileStringW(section.as_ptr(), key.as_ptr(), empty.as_ptr(), buffer.as_mut_ptr(), buffer.len() as u32, file.as_ptr());
    }
    from_wide(&buffer)
}

/// Sets a key, or removes it with `None`. The other lines of the file, comments included, stay as they are.
pub fn ini_set(file: &Path, section: &str, key: &str, value: Option<&str>) -> Result<(), String> {
    let (file_w, section, key_w) = (wide_path(file), wide(section), wide(key));
    let value_w = value.map(wide);
    let ok = unsafe {
        WritePrivateProfileStringW(section.as_ptr(), key_w.as_ptr(), value_w.as_ref().map_or(null(), |v| v.as_ptr()), file_w.as_ptr())
    };
    if ok == 0 {
        Err(format!("could not write {} in {}", key, file.display()))
    } else {
        Ok(())
    }
}

pub fn sha256(data: &[u8]) -> Result<String, String> {
    let mut algorithm: BCRYPT_ALG_HANDLE = null_mut();
    let mut digest = [0u8; 32];
    unsafe {
        if BCryptOpenAlgorithmProvider(&mut algorithm, BCRYPT_SHA256_ALGORITHM, null(), 0) != 0 {
            return Err("SHA-256 is not available".into());
        }
        let status = BCryptHash(algorithm, null(), 0, data.as_ptr(), data.len() as u32, digest.as_mut_ptr(), digest.len() as u32);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if status != 0 {
            return Err("SHA-256 could not be computed".into());
        }
    }
    Ok(digest.iter().map(|b| format!("{:02x}", b)).collect())
}

pub fn sha256_file(path: &Path) -> Result<String, String> {
    let data = std::fs::read(path).map_err(|e| format!("could not read {}: {}", path.display(), e))?;
    sha256(&data)
}

/// String value under HKEY_LOCAL_MACHINE
pub fn registry_string(subkey: &str, value: &str) -> Option<String> {
    let mut buffer = [0u16; 1024];
    let mut size = (buffer.len() * 2) as u32;
    let (subkey, value) = (wide(subkey), wide(value));
    let status = unsafe {
        RegGetValueW(HKEY_LOCAL_MACHINE, subkey.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ, null_mut(), buffer.as_mut_ptr() as *mut c_void, &mut size)
    };
    if status == ERROR_SUCCESS {
        Some(from_wide(&buffer))
    } else {
        None
    }
}

/// True when a process with this executable name is running
pub fn process_running(exe_name: &str) -> bool {
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snapshot == INVALID_HANDLE_VALUE {
            return false;
        }
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut found = false;
        let mut more = Process32FirstW(snapshot, &mut entry);
        while more != 0 {
            if from_wide(&entry.szExeFile).eq_ignore_ascii_case(exe_name) {
                found = true;
                break;
            }
            more = Process32NextW(snapshot, &mut entry);
        }
        CloseHandle(snapshot);
        found
    }
}

/// Downloads a file over HTTPS with the system's own downloader. Blocks until it is done.
pub fn download(url: &str, destination: &Path) -> Result<(), String> {
    let (url_w, destination_w) = (wide(url), wide_path(destination));
    let result = unsafe { URLDownloadToFileW(null_mut(), url_w.as_ptr(), destination_w.as_ptr(), 0, null_mut()) };
    if result == 0 {
        Ok(())
    } else {
        Err(format!("download failed (0x{:08X}): {}", result as u32, url))
    }
}

/// The user's Documents folder, wherever it was moved to
pub fn documents_dir() -> Option<PathBuf> {
    const CSIDL_PERSONAL: i32 = 5;
    let mut buffer = [0u16; 260];
    let result = unsafe { SHGetFolderPathW(null_mut(), CSIDL_PERSONAL, null_mut(), 0, buffer.as_mut_ptr()) };
    if result == 0 {
        Some(PathBuf::from(from_wide(&buffer)))
    } else {
        None
    }
}

/// The game folder from the Steam libraries or from Ubisoft Connect, when one of them has the game
pub fn find_game(folder_name: &str, exe_name: &str) -> Option<PathBuf> {
    let mut libraries: Vec<PathBuf> = Vec::new();
    for key in ["SOFTWARE\\WOW6432Node\\Valve\\Steam", "SOFTWARE\\Valve\\Steam"] {
        if let Some(steam) = registry_string(key, "InstallPath") {
            let steam = PathBuf::from(steam);
            // "path"  "D:\\SteamLibrary" lines of the library list
            if let Ok(list) = std::fs::read_to_string(steam.join("steamapps").join("libraryfolders.vdf")) {
                for line in list.lines() {
                    let parts: Vec<&str> = line.split('"').collect();
                    if parts.len() >= 4 && parts[1] == "path" {
                        libraries.push(PathBuf::from(parts[3].replace("\\\\", "\\")));
                    }
                }
            }
            libraries.push(steam);
        }
    }
    for library in libraries {
        let folder = library.join("steamapps").join("common").join(folder_name);
        if folder.join(exe_name).is_file() {
            return Some(folder);
        }
    }
    // Ubisoft Connect lists its installations by a numeric id; 13504 is Valhalla
    for key in ["SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs\\13504", "SOFTWARE\\Ubisoft\\Launcher\\Installs\\13504"] {
        if let Some(folder) = registry_string(key, "InstallDir") {
            let folder = PathBuf::from(folder.replace('/', "\\"));
            if folder.join(exe_name).is_file() {
                return Some(folder);
            }
        }
    }
    None
}
