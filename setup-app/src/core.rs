//! What an installation consists of, and how the game folder is brought to it.
//!
//! Rules that keep the game folder safe:
//!  - Only files this tool owns are ever replaced or deleted. A file is owned when the manifest lists it
//!    with its current hash, or when it is byte-identical to one of the files this tool ships.
//!  - Any other file under a name the tool needs is a conflict: nothing is changed and the user is told.
//!  - Nothing is done while the game is running.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use crate::sys;

pub const GAME_FOLDER: &str = "Assassin's Creed Valhalla";
pub const GAME_EXE: &str = "ACValhalla.exe";
pub const SUBDIR: &str = "ACVUpscaler";
const MANIFEST: &str = "ACVUpscaler\\install_manifest.txt";
const ADDON_INI: &str = "ACVUpscaler\\ACVUpscaler.ini";
const OPTI_INI: &str = "OptiScaler.ini";
const RESHADE_INI: &str = "ReShade.ini";
const ADDON_PATH: &str = ".\\ACVUpscaler";

pub const DLSS_FILE: &str = "nvngx_dlss.dll";
/// Where the add-on looks for the DLSS file when it calls DLSS itself
const DLSS_IN_SUBDIR: &str = "ACVUpscaler\\nvngx_dlss.dll";
pub const DLSS_VERSION: &str = "310.9.16";
pub const DLSS_URL: &str =
    "https://raw.githubusercontent.com/NVIDIA/DLSS/9981c6350dc58c0ce21f024fd65e31e851c31689/lib/Windows_x86_64/rel/nvngx_dlss.dll";
pub const DLSS_SHA256: &str = "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e";
pub const DLSS_LICENCE_URL: &str = "https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt";

/// Files in the `payload` folder next to the tool
const P_ADDON: &str = "ACVUpscaler.addon64";
const P_FFX_LOADER: &str = "amd_fidelityfx_loader_dx12.dll";
const P_FFX_UPSCALER: &str = "amd_fidelityfx_upscaler_dx12.dll";
const P_RESHADE: &str = "ReShade64.dll";
pub const PAYLOAD_FILES: [&str; 4] = [P_ADDON, P_FFX_LOADER, P_FFX_UPSCALER, P_RESHADE];
/// OptiScaler 0.9.4 as the releases up to 1.0.0-beta.2 placed it for DLSS: no longer shipped, still recognised
/// as a file of this tool so that such an installation can be taken over
const OPTISCALER_SHA256: &str = "fbfb6676b829dad7e020fb830586a16aa0ec6add78016db48ef12e2ae1803231";

/// Names in the game folder that can hold one of our DLLs, whatever the layout
const DLL_SLOTS: [&str; 4] = ["dxgi.dll", "winmm.dll", "ReShade64.dll", DLSS_FILE];
/// Written by ReShade, OptiScaler, the add-on and (with the developer tools on) DLSS while the game runs
const RUNTIME_FILES: [&str; 7] = [
    "ReShade.log",
    "OptiScaler.log",
    "ACVUpscaler\\acvupscaler.log",
    "ACVUpscaler\\acvupscaler_perf.log",
    "ACVUpscaler\\nvngx.log",
    "ACVUpscaler\\nvsdk_ngx.log",
    "ACVUpscaler\\nvngx_dlss_310_9_16.log",
];

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Upscaler {
    Dlss,
    Fsr31,
    Off,
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Model {
    K,
    M,
    Default,
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Choice {
    pub upscaler: Upscaler,
    pub model: Model,
    pub gain: u32,
    pub lod_bias: bool,
    /// The sharpening pass (RCAS) of the game on top of the upscaler
    pub game_sharpening: bool,
    /// Strength in percent of the add-on's own sharpening pass, which runs where the game's does not: with
    /// `game_sharpening` off and at native resolution. 0: none, 84: the strength of the game's pass.
    pub sharpness: u32,
    pub dev_mode: bool,
    /// NVIDIA Reflex low latency mode of the add-on (its limited form). Also a checkbox in the add-on's panel in
    /// the game, which writes the same setting.
    pub reflex: bool,
}

impl Default for Choice {
    fn default() -> Self {
        Choice { upscaler: Upscaler::Fsr31, model: Model::K, gain: 4, lod_bias: true, game_sharpening: true, sharpness: 0, dev_mode: false, reflex: false }
    }
}

#[derive(Clone, PartialEq, Debug)]
enum Source {
    Payload(&'static str),
    Dlss,
}

/// Target path in the game folder -> where its content comes from
fn wanted_files(choice: &Choice) -> Vec<(&'static str, Source)> {
    let mut files = vec![
        ("ACVUpscaler\\ACVUpscaler.addon64", Source::Payload(P_ADDON)),
        ("ACVUpscaler\\amd_fidelityfx_loader_dx12.dll", Source::Payload(P_FFX_LOADER)),
        ("ACVUpscaler\\amd_fidelityfx_upscaler_dx12.dll", Source::Payload(P_FFX_UPSCALER)),
    ];
    files.push(("dxgi.dll", Source::Payload(P_RESHADE)));
    if choice.upscaler == Upscaler::Dlss {
        files.push((DLSS_IN_SUBDIR, Source::Dlss));
    }
    files
}

#[derive(Default, Debug, PartialEq)]
struct Manifest {
    /// Relative path -> SHA-256, or "-" for files whose content changes (ini files)
    files: BTreeMap<String, String>,
    reshade_ini_created: bool,
    /// AddonPath that was in an existing ReShade.ini before the first installation
    addon_path_before: Option<String>,
}

impl Manifest {
    fn load(game: &Path) -> Manifest {
        let mut manifest = Manifest::default();
        if let Ok(text) = fs::read_to_string(game.join(MANIFEST)) {
            for line in text.lines() {
                let parts: Vec<&str> = line.splitn(3, '|').collect();
                match parts.as_slice() {
                    ["file", path, hash] => {
                        manifest.files.insert(path.to_string(), hash.to_string());
                    }
                    ["reshade_ini_created"] => manifest.reshade_ini_created = true,
                    ["addon_path_before", value] => manifest.addon_path_before = Some(value.to_string()),
                    ["addon_path_before"] => manifest.addon_path_before = Some(String::new()),
                    _ => {}
                }
            }
        }
        manifest
    }

    fn save(&self, game: &Path) -> Result<(), String> {
        let mut text = String::from("# Files added to the game folder by ACVUpscalerSetup. Uninstall removes exactly these.\n");
        for (path, hash) in &self.files {
            text += &format!("file|{}|{}\n", path, hash);
        }
        if self.reshade_ini_created {
            text += "reshade_ini_created\n";
        }
        if let Some(value) = &self.addon_path_before {
            text += &format!("addon_path_before|{}\n", value);
        }
        fs::write(game.join(MANIFEST), text).map_err(|e| format!("could not write the manifest: {}", e))
    }
}

#[derive(Debug, PartialEq)]
pub enum Action {
    Copy { payload: PathBuf, target: String },
    Replace { payload: PathBuf, target: String },
    Download { target: String },
    Keep { target: String },
    Delete { target: String },
}

impl Action {
    pub fn describe(&self) -> String {
        match self {
            Action::Copy { target, .. } => format!("add      {}", target),
            Action::Replace { target, .. } => format!("replace  {} (a file of this tool)", target),
            Action::Download { target } => format!("download {} (DLSS {} from github.com/NVIDIA/DLSS)", target, DLSS_VERSION),
            Action::Keep { target } => format!("keep     {}", target),
            Action::Delete { target } => format!("remove   {}", target),
        }
    }
    fn changes_something(&self) -> bool {
        !matches!(self, Action::Keep { .. })
    }
}

pub fn describe(actions: &[Action]) -> String {
    let lines: Vec<String> = actions.iter().filter(|a| a.changes_something()).map(Action::describe).collect();
    if lines.is_empty() {
        "no files to add or remove (settings only)".into()
    } else {
        lines.join("\r\n")
    }
}

pub fn needs_download(actions: &[Action]) -> bool {
    actions.iter().any(|a| matches!(a, Action::Download { .. }))
}

fn hash_if_exists(path: &Path) -> Result<Option<String>, String> {
    if path.is_file() {
        sys::sha256_file(path).map(Some)
    } else {
        Ok(None)
    }
}

/// Hashes of everything this tool ships or downloads: a file equal to one of them is ours to replace
fn shipped_hashes(payload: &Path, dlss_hash: &str) -> Result<Vec<String>, String> {
    let mut hashes = vec![dlss_hash.to_string(), OPTISCALER_SHA256.to_string()];
    for name in [P_ADDON, P_FFX_LOADER, P_FFX_UPSCALER, P_RESHADE] {
        hashes.push(sys::sha256_file(&payload.join(name))?);
    }
    Ok(hashes)
}

fn owned(manifest: &Manifest, shipped: &[String], target: &str, current_hash: &str) -> bool {
    shipped.iter().any(|h| h == current_hash) || manifest.files.get(target).map_or(false, |h| h == "-" || h == current_hash)
}

pub fn check_folders(game: &Path, payload: &Path) -> Result<(), String> {
    if !game.join(GAME_EXE).is_file() {
        return Err(format!("{} is not in {}", GAME_EXE, game.display()));
    }
    for name in PAYLOAD_FILES {
        if !payload.join(name).is_file() {
            return Err(format!("the file payload\\{} is missing next to this tool. Extract the whole zip first.", name));
        }
    }
    Ok(())
}

/// What has to happen to the files of the game folder for this choice. Fails on the first conflict.
pub fn plan_install(game: &Path, payload: &Path, choice: &Choice, dlss_local: Option<&Path>) -> Result<Vec<Action>, String> {
    plan_install_for(game, payload, choice, dlss_local, DLSS_SHA256)
}

/// `dlss_hash`: the checksum the DLSS file has to have (the tests use a stand-in file)
fn plan_install_for(game: &Path, payload: &Path, choice: &Choice, dlss_local: Option<&Path>, dlss_hash: &str) -> Result<Vec<Action>, String> {
    check_folders(game, payload)?;
    let manifest = Manifest::load(game);
    let shipped = shipped_hashes(payload, dlss_hash)?;
    let wanted = wanted_files(choice);
    let mut actions = Vec::new();

    for (target, source) in &wanted {
        let (source_path, source_hash) = match source {
            Source::Payload(name) => (Some(payload.join(name)), sys::sha256_file(&payload.join(name))?),
            Source::Dlss => {
                // The copy given on the command line, or the one an earlier layout left in its other place:
                // nothing is downloaded twice
                let mut local = dlss_local.map(Path::to_path_buf);
                if local.is_none() {
                    for other in [DLSS_FILE, DLSS_IN_SUBDIR] {
                        if other != *target && hash_if_exists(&game.join(other))?.as_deref() == Some(dlss_hash) {
                            local = Some(game.join(other));
                        }
                    }
                }
                (local, dlss_hash.to_string())
            }
        };
        if let (Source::Dlss, Some(local)) = (source, dlss_local) {
            if sys::sha256_file(local)? != dlss_hash {
                return Err(format!("{} is not DLSS {} (the checksum differs)", local.display(), DLSS_VERSION));
            }
        }
        let target = target.to_string();
        match hash_if_exists(&game.join(&target))? {
            None => match source_path {
                Some(path) => actions.push(Action::Copy { payload: path, target }),
                None => actions.push(Action::Download { target }),
            },
            Some(current) if current == source_hash => actions.push(Action::Keep { target }),
            Some(current) if owned(&manifest, &shipped, &target, &current) => match source_path {
                Some(path) => actions.push(Action::Replace { payload: path, target }),
                None => {
                    actions.push(Action::Delete { target: target.clone() });
                    actions.push(Action::Download { target });
                }
            },
            Some(_) => {
                return Err(format!(
                    "{} already exists in the game folder and is not a file of this tool (another mod?). Nothing was changed. \
                     Remove or rename that file yourself if you want ACVUpscaler to use this name.",
                    target
                ))
            }
        }
    }

    // Our own files that this choice no longer needs (DLSS before and not now, or an older layout with OptiScaler)
    let mut candidates: Vec<String> = manifest.files.keys().cloned().collect();
    candidates.extend(DLL_SLOTS.iter().map(|s| s.to_string()));
    candidates.sort();
    candidates.dedup();
    for target in candidates {
        let generated = target == ADDON_INI || target == RESHADE_INI;
        if generated || wanted.iter().any(|(wanted_target, _)| *wanted_target == target) {
            continue;
        }
        if let Some(current) = hash_if_exists(&game.join(&target))? {
            if owned(&manifest, &shipped, &target, &current) {
                actions.push(Action::Delete { target });
            }
        }
    }
    Ok(actions)
}

fn ensure_parent(path: &Path) -> Result<(), String> {
    match path.parent() {
        Some(parent) => fs::create_dir_all(parent).map_err(|e| format!("could not create {}: {}", parent.display(), e)),
        None => Ok(()),
    }
}

fn bool_text(value: bool) -> &'static str {
    if value {
        "1"
    } else {
        "0"
    }
}

fn write_settings(game: &Path, choice: &Choice, manifest: &mut Manifest, log: &mut Vec<String>) -> Result<(), String> {
    // The add-on's own settings. RenderScale in the same file belongs to the add-on and is left alone.
    let addon_ini = game.join(ADDON_INI);
    ensure_parent(&addon_ini)?;
    let section = "ACVUpscaler";
    sys::ini_set(&addon_ini, section, "Mode", Some(if choice.upscaler == Upscaler::Off { "GameTAA" } else { "FSR31" }))?;
    let own_dlss = choice.upscaler == Upscaler::Dlss;
    sys::ini_set(&addon_ini, section, "Upscaler", Some(if own_dlss { "DLSS" } else { "FSR31" }))?;
    sys::ini_set(
        &addon_ini,
        section,
        "DlssPreset",
        Some(match choice.model {
            Model::K => "K",
            Model::M => "M",
            Model::Default => "Default",
        }),
    )?;
    sys::ini_set(&addon_ini, section, "ReactiveGain", Some(&choice.gain.to_string()))?;
    sys::ini_set(&addon_ini, section, "LodBias", Some(bool_text(choice.lod_bias)))?;
    sys::ini_set(&addon_ini, section, "GameSharpening", Some(bool_text(choice.game_sharpening)))?;
    sys::ini_set(&addon_ini, section, "Sharpness", Some(&choice.sharpness.to_string()))?;
    sys::ini_set(&addon_ini, section, "KeepContexts", Some("0"))?; // Only for OptiScaler placed by hand (INSTALL.md)
    sys::ini_set(&addon_ini, section, "DevMode", Some(bool_text(choice.dev_mode)))?;
    sys::ini_set(&addon_ini, section, "Reflex", Some(bool_text(choice.reflex)))?;
    manifest.files.insert(ADDON_INI.into(), "-".into());
    log.push(format!("settings {}", ADDON_INI));

    // ReShade has to look for add-ons in our subfolder
    let reshade_ini = game.join(RESHADE_INI);
    if !reshade_ini.exists() {
        fs::write(&reshade_ini, "[ADDON]\r\nAddonPath=.\\ACVUpscaler\r\n[OVERLAY]\r\nTutorialProgress=4\r\n")
            .map_err(|e| format!("could not create {}: {}", RESHADE_INI, e))?;
        manifest.reshade_ini_created = true;
        manifest.files.insert(RESHADE_INI.into(), "-".into());
        log.push(format!("add      {}", RESHADE_INI));
    } else if sys::ini_get(&reshade_ini, "ADDON", "AddonPath") != ADDON_PATH {
        if !manifest.reshade_ini_created && manifest.addon_path_before.is_none() {
            manifest.addon_path_before = Some(sys::ini_get(&reshade_ini, "ADDON", "AddonPath"));
        }
        sys::ini_set(&reshade_ini, "ADDON", "AddonPath", Some(ADDON_PATH))?;
        log.push(format!("settings {} (AddonPath)", RESHADE_INI));
    }

    // An installation of a release up to 1.0.0-beta.2 with OptiScaler: its settings file and its log go
    if manifest.files.contains_key(OPTI_INI) {
        let opti_ini = game.join(OPTI_INI);
        if opti_ini.exists() {
            fs::remove_file(&opti_ini).map_err(|e| format!("could not remove {}: {}", OPTI_INI, e))?;
            log.push(format!("remove   {}", OPTI_INI));
        }
        manifest.files.remove(OPTI_INI);
        let _ = fs::remove_file(game.join("OptiScaler.log"));
    }
    Ok(())
}

/// Carries out a plan and writes the settings. `download` fetches the DLSS file to the given path.
pub fn apply_install(
    game: &Path,
    choice: &Choice,
    actions: &[Action],
    game_exe_to_check: Option<&str>,
    download: &dyn Fn(&Path) -> Result<(), String>,
) -> Result<Vec<String>, String> {
    if let Some(exe) = game_exe_to_check {
        if sys::process_running(exe) {
            return Err("the game is running. Close it first.".into());
        }
    }
    let mut manifest = Manifest::load(game);
    let mut log = Vec::new();
    fs::create_dir_all(game.join(SUBDIR)).map_err(|e| format!("could not create {}: {}", SUBDIR, e))?;

    for action in actions {
        match action {
            Action::Copy { payload: source, target } | Action::Replace { payload: source, target } => {
                let destination = game.join(target);
                ensure_parent(&destination)?;
                fs::copy(source, &destination).map_err(|e| format!("could not copy {}: {}", target, e))?;
                manifest.files.insert(target.clone(), sys::sha256_file(&destination)?);
            }
            Action::Download { target } => {
                let destination = game.join(target);
                let temporary = game.join(SUBDIR).join("download.tmp");
                download(&temporary)?;
                let hash = sys::sha256_file(&temporary)?;
                if hash != DLSS_SHA256 {
                    let _ = fs::remove_file(&temporary);
                    return Err(format!("the downloaded file is not DLSS {} (checksum {}). Nothing was installed from it.", DLSS_VERSION, hash));
                }
                fs::rename(&temporary, &destination).map_err(|e| format!("could not place {}: {}", target, e))?;
                manifest.files.insert(target.clone(), hash);
            }
            Action::Keep { target } => {
                manifest.files.insert(target.clone(), sys::sha256_file(&game.join(target))?);
            }
            Action::Delete { target } => {
                fs::remove_file(game.join(target)).map_err(|e| format!("could not remove {}: {}", target, e))?;
                manifest.files.remove(target);
            }
        }
        if action.changes_something() {
            log.push(action.describe());
        }
        // After every step, so that an interrupted run still knows what it added
        manifest.save(game)?;
    }

    write_settings(game, choice, &mut manifest, &mut log)?;
    manifest.save(game)?;
    Ok(log)
}

/// What uninstalling would remove, as text
pub fn plan_uninstall(game: &Path) -> Vec<String> {
    let manifest = Manifest::load(game);
    let mut lines: Vec<String> = manifest.files.keys().map(|p| format!("remove   {}", p)).collect();
    for name in RUNTIME_FILES {
        if game.join(name).is_file() {
            lines.push(format!("remove   {} (log)", name));
        }
    }
    if let Some(before) = &manifest.addon_path_before {
        lines.push(format!("restore  AddonPath in {} to \"{}\"", RESHADE_INI, before));
    }
    lines
}

pub fn is_installed(game: &Path) -> bool {
    game.join(MANIFEST).is_file()
}

/// Removes what the manifest lists. Files that were changed since they were installed are left alone.
pub fn uninstall(game: &Path, game_exe_to_check: Option<&str>) -> Result<Vec<String>, String> {
    if let Some(exe) = game_exe_to_check {
        if sys::process_running(exe) {
            return Err("the game is running. Close it first.".into());
        }
    }
    if !is_installed(game) {
        return Err("no installation of this tool was found in the game folder".into());
    }
    let manifest = Manifest::load(game);
    let mut log = Vec::new();

    for (target, hash) in &manifest.files {
        let path = game.join(target);
        if !path.is_file() {
            continue;
        }
        if target == RESHADE_INI && !manifest.reshade_ini_created {
            continue;
        }
        if hash != "-" && sys::sha256_file(&path)? != *hash {
            log.push(format!("left     {} (changed since it was installed)", target));
            continue;
        }
        fs::remove_file(&path).map_err(|e| format!("could not remove {}: {}", target, e))?;
        log.push(format!("remove   {}", target));
    }

    if !manifest.reshade_ini_created {
        if let Some(before) = &manifest.addon_path_before {
            let reshade_ini = game.join(RESHADE_INI);
            if reshade_ini.is_file() {
                sys::ini_set(&reshade_ini, "ADDON", "AddonPath", if before.is_empty() { None } else { Some(before.as_str()) })?;
                log.push(format!("restore  AddonPath in {}", RESHADE_INI));
            }
        }
    }

    for name in RUNTIME_FILES {
        let path = game.join(name);
        if path.is_file() && fs::remove_file(&path).is_ok() {
            log.push(format!("remove   {}", name));
        }
    }
    // ReShade creates an empty preset file on its first run
    let preset = game.join("ReShadePreset.ini");
    if manifest.reshade_ini_created && fs::metadata(&preset).map_or(false, |m| m.len() == 0) && fs::remove_file(&preset).is_ok() {
        log.push("remove   ReShadePreset.ini (empty)".into());
    }

    fs::remove_file(game.join(MANIFEST)).map_err(|e| format!("could not remove the manifest: {}", e))?;
    // Only goes away when nothing else is left in it (snapshots of the developer tools stay)
    if fs::remove_dir(game.join(SUBDIR)).is_err() {
        log.push(format!("left     {}\\ (it still contains other files)", SUBDIR));
    }
    Ok(log)
}

/// The choice an existing installation was made with, as far as the files tell
pub fn read_choice(game: &Path) -> Choice {
    let mut choice = Choice::default();
    let addon_ini = game.join(ADDON_INI);
    if !addon_ini.is_file() {
        return choice;
    }
    let get = |key: &str| sys::ini_get(&addon_ini, "ACVUpscaler", key);
    let optiscaler = game.join(OPTI_INI).is_file() && (game.join("winmm.dll").is_file() || game.join("ReShade64.dll").is_file());
    let own_dlss = get("Upscaler").eq_ignore_ascii_case("DLSS");
    choice.upscaler = if get("Mode").eq_ignore_ascii_case("GameTAA") {
        Upscaler::Off
    } else if optiscaler || own_dlss {
        Upscaler::Dlss
    } else {
        Upscaler::Fsr31
    };
    if own_dlss {
        choice.model = match get("DlssPreset").to_ascii_uppercase().as_str() {
            "M" => Model::M,
            "DEFAULT" => Model::Default,
            _ => Model::K,
        };
    }
    if optiscaler {
        let opti_ini = game.join(OPTI_INI);
        choice.model = match (sys::ini_get(&opti_ini, "DLSS", "RenderPresetOverride").as_str(), sys::ini_get(&opti_ini, "DLSS", "RenderPresetForAll").as_str()) {
            ("true", "11") => Model::K,
            ("true", "13") => Model::M,
            _ => Model::Default,
        };
    }
    if let Ok(gain) = get("ReactiveGain").parse::<f32>() {
        choice.gain = gain.round().clamp(0.0, 16.0) as u32;
    }
    choice.lod_bias = get("LodBias") != "0";
    choice.game_sharpening = get("GameSharpening") != "0";
    choice.sharpness = get("Sharpness").parse::<u32>().unwrap_or(0).min(100);
    choice.dev_mode = get("DevMode") == "1";
    choice.reflex = get("Reflex") == "1";
    choice
}

/// The installation in `game` as the options of this tool's command line that would recreate it (`--show`)
pub fn command_line(game: &Path, game_settings: Option<&GameSettings>) -> String {
    if !is_installed(game) {
        return String::from("--uninstall");
    }
    let choice = read_choice(game);
    let mut args = vec!["--install", "--upscaler"];
    args.push(match choice.upscaler {
        Upscaler::Dlss => "dlss",
        Upscaler::Fsr31 => "fsr31",
        Upscaler::Off => "off",
    });
    if choice.upscaler == Upscaler::Dlss {
        args.extend(["--model", match choice.model {
            Model::K => "k",
            Model::M => "m",
            Model::Default => "default",
        }]);
    }
    let gain = choice.gain.to_string();
    args.extend(["--gain", gain.as_str()]);
    if !choice.lod_bias {
        args.push("--no-lod-bias");
    }
    if !choice.game_sharpening {
        args.push("--no-game-sharpening");
    }
    let sharpness = choice.sharpness.to_string();
    if choice.sharpness != 0 {
        args.extend(["--sharpness", sharpness.as_str()]);
    }
    if choice.dev_mode {
        args.push("--dev");
    }
    if choice.reflex {
        args.push("--reflex");
    }
    args.join(" ") + &game_render_args(game_settings)
}

/// The game's own render settings as options of the command line (empty without a settings file)
pub fn game_render_args(game_settings: Option<&GameSettings>) -> String {
    let Some(settings) = game_settings else { return String::new() };
    let quality = match (settings.fsr_on, settings.quality) {
        (false, _) => " --quality native",
        (true, Some(Quality::UltraQuality)) => " --quality uq",
        (true, Some(Quality::Quality)) => " --quality quality",
        (true, Some(Quality::Balanced)) => " --quality balanced",
        (true, Some(Quality::Performance)) => " --quality performance",
        (true, None) => "",
    };
    let limit = match settings.fps_limit {
        Some((true, fps)) => format!(" --fps-limit {}", fps),
        Some((false, fps)) => format!(" --fps-limit off,{}", fps),
        None => String::new(),
    };
    let characters = if settings.character == Some(CHARACTER_FULL) { " --full-characters" } else { "" };
    format!("{} --pixel-density {}{}{}", quality, settings.pixel_density, limit, characters)
}

/// `--fps-limit` of the command line: "60" (limit on at 60), "off" (limit off), "off,30" (off, value 30 kept)
pub fn parse_fps_limit(text: &str) -> Option<(bool, Option<u32>)> {
    let (on, number) = match text.strip_prefix("off") {
        Some(rest) => (false, rest.strip_prefix(',')),
        None => (true, Some(text)),
    };
    match number {
        Some(n) => n.parse::<u32>().ok().filter(|fps| (20..=1000).contains(fps)).map(|fps| (on, Some(fps))),
        None => Some((on, None)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Sandbox {
        root: PathBuf,
        game: PathBuf,
        payload: PathBuf,
    }

    impl Sandbox {
        fn new(name: &str) -> Sandbox {
            let root = std::env::temp_dir().join(format!("acv_setup_test_{}_{}", name, std::process::id()));
            let _ = fs::remove_dir_all(&root);
            let (game, payload) = (root.join("game"), root.join("payload"));
            fs::create_dir_all(game.join("dlc_20")).unwrap();
            fs::create_dir_all(&payload).unwrap();
            fs::write(game.join(GAME_EXE), b"game executable").unwrap();
            fs::write(game.join("DataPC.forge"), b"game data").unwrap();
            fs::write(game.join("dlc_20").join("data.forge"), b"more game data").unwrap();
            for name in PAYLOAD_FILES {
                fs::write(payload.join(name), format!("content of {}", name)).unwrap();
            }
            Sandbox { root, game, payload }
        }

        /// Relative path -> content of every file below the game folder
        fn listing(&self) -> BTreeMap<String, Vec<u8>> {
            fn walk(base: &Path, dir: &Path, out: &mut BTreeMap<String, Vec<u8>>) {
                for entry in fs::read_dir(dir).unwrap() {
                    let path = entry.unwrap().path();
                    if path.is_dir() {
                        walk(base, &path, out);
                    } else {
                        out.insert(path.strip_prefix(base).unwrap().to_string_lossy().into_owned(), fs::read(&path).unwrap());
                    }
                }
            }
            let mut out = BTreeMap::new();
            walk(&self.game, &self.game, &mut out);
            out
        }

        fn install(&self, choice: &Choice) -> Result<Vec<String>, String> {
            // The fake "DLSS file" cannot have the real checksum, so the tests never ask for the download
            let actions = plan_install(&self.game, &self.payload, choice, None)?;
            assert!(!needs_download(&actions) || choice.upscaler == Upscaler::Dlss);
            apply_install(&self.game, choice, &actions, None, &|_| Err("no download in tests".into()))
        }
    }

    impl Drop for Sandbox {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }

    fn fsr() -> Choice {
        Choice { upscaler: Upscaler::Fsr31, ..Choice::default() }
    }

    #[test]
    fn install_and_uninstall_leave_the_folder_as_it_was() {
        let sandbox = Sandbox::new("roundtrip");
        let before = sandbox.listing();
        sandbox.install(&fsr()).unwrap();

        let installed = sandbox.listing();
        assert_eq!(installed["dxgi.dll"], b"content of ReShade64.dll".to_vec());
        assert!(installed.contains_key("ACVUpscaler\\ACVUpscaler.addon64"));
        assert!(!installed.contains_key("winmm.dll"));
        assert_eq!(sys::ini_get(&sandbox.game.join(RESHADE_INI), "ADDON", "AddonPath"), ADDON_PATH);
        assert_eq!(sys::ini_get(&sandbox.game.join(ADDON_INI), "ACVUpscaler", "Mode"), "FSR31");
        assert_eq!(sys::ini_get(&sandbox.game.join(ADDON_INI), "ACVUpscaler", "DevMode"), "0");

        // What the game and ReShade write while running
        fs::write(sandbox.game.join("ReShade.log"), b"log").unwrap();
        fs::write(sandbox.game.join("ReShadePreset.ini"), b"").unwrap();
        fs::write(sandbox.game.join(SUBDIR).join("acvupscaler.log"), b"log").unwrap();
        sys::ini_set(&sandbox.game.join(ADDON_INI), "ACVUpscaler", "RenderScale", Some("0.5000")).unwrap();

        uninstall(&sandbox.game, None).unwrap();
        assert_eq!(sandbox.listing(), before);
    }

    #[test]
    fn reflex_is_a_choice_and_the_panel_of_the_addon_can_change_it() {
        let sandbox = Sandbox::new("reflex");
        sandbox.install(&fsr()).unwrap();
        let addon_ini = sandbox.game.join(ADDON_INI);
        assert_eq!(sys::ini_get(&addon_ini, "ACVUpscaler", "Reflex"), "0");
        assert_eq!(command_line(&sandbox.game, None), "--install --upscaler fsr31 --gain 4");
        let with_reflex = Choice { reflex: true, ..fsr() };
        sandbox.install(&with_reflex).unwrap();
        assert_eq!(sys::ini_get(&addon_ini, "ACVUpscaler", "Reflex"), "1");
        assert_eq!(read_choice(&sandbox.game), with_reflex);
        assert_eq!(command_line(&sandbox.game, None), "--install --upscaler fsr31 --gain 4 --reflex");
        // The add-on's panel writes the same key while the game runs: the tool shows what it finds
        sys::ini_set(&addon_ini, "ACVUpscaler", "Reflex", Some("0")).unwrap();
        assert_eq!(read_choice(&sandbox.game), fsr());
    }

    #[test]
    fn a_foreign_dxgi_dll_stops_the_installation() {
        let sandbox = Sandbox::new("conflict");
        fs::write(sandbox.game.join("dxgi.dll"), b"somebody else's mod").unwrap();
        let before = sandbox.listing();
        let error = plan_install(&sandbox.game, &sandbox.payload, &fsr(), None).unwrap_err();
        assert!(error.contains("dxgi.dll"), "{}", error);
        assert_eq!(sandbox.listing(), before);
    }

    #[test]
    fn an_existing_reshade_ini_gets_its_addon_path_back() {
        let sandbox = Sandbox::new("reshadeini");
        fs::write(sandbox.game.join(RESHADE_INI), "[ADDON]\r\nAddonPath=C:\\somewhere\r\n[GENERAL]\r\nNoDebugInfo=1\r\n").unwrap();
        // The identical ReShade is already there under its usual name: it is adopted
        fs::copy(sandbox.payload.join(P_RESHADE), sandbox.game.join("dxgi.dll")).unwrap();
        sandbox.install(&fsr()).unwrap();
        assert_eq!(sys::ini_get(&sandbox.game.join(RESHADE_INI), "ADDON", "AddonPath"), ADDON_PATH);
        // A second run must not forget the original value
        sandbox.install(&Choice { gain: 8, ..fsr() }).unwrap();
        uninstall(&sandbox.game, None).unwrap();
        let listing = sandbox.listing();
        assert_eq!(sys::ini_get(&sandbox.game.join(RESHADE_INI), "ADDON", "AddonPath"), "C:\\somewhere");
        assert_eq!(sys::ini_get(&sandbox.game.join(RESHADE_INI), "GENERAL", "NoDebugInfo"), "1");
        assert!(!listing.contains_key("dxgi.dll"), "the adopted ReShade belongs to the installation and goes with it");
    }

    #[test]
    fn dlss_is_installed_next_to_the_addon() {
        let sandbox = Sandbox::new("dlss");
        let before = sandbox.listing();
        // A stand-in for the DLSS file, and its checksum in place of the real one
        let stand_in = sandbox.root.join("stand_in_dlss.dll");
        fs::write(&stand_in, b"stand-in for nvngx_dlss.dll").unwrap();
        let hash = sys::sha256_file(&stand_in).unwrap();
        let dlss = Choice { upscaler: Upscaler::Dlss, model: Model::K, game_sharpening: false, sharpness: 30, ..Choice::default() };
        assert!(needs_download(&plan_install(&sandbox.game, &sandbox.payload, &dlss, None).unwrap()));

        let plan = plan_install_for(&sandbox.game, &sandbox.payload, &dlss, Some(&stand_in), &hash).unwrap();
        apply_install(&sandbox.game, &dlss, &plan, None, &|_| unreachable!()).unwrap();
        let listing = sandbox.listing();
        assert_eq!(listing["dxgi.dll"], b"content of ReShade64.dll".to_vec());
        assert_eq!(listing[DLSS_IN_SUBDIR], b"stand-in for nvngx_dlss.dll".to_vec());
        assert!(!listing.contains_key(DLSS_FILE) && !listing.contains_key("winmm.dll") && !listing.contains_key(OPTI_INI));
        assert_eq!(sys::ini_get(&sandbox.game.join(ADDON_INI), "ACVUpscaler", "Upscaler"), "DLSS");
        assert_eq!(read_choice(&sandbox.game), dlss);
        assert_eq!(command_line(&sandbox.game, None), "--install --upscaler dlss --model k --gain 4 --no-game-sharpening --sharpness 30");

        uninstall(&sandbox.game, None).unwrap();
        assert_eq!(sandbox.listing(), before);
    }
    #[test]
    fn a_file_changed_after_installation_is_not_deleted() {
        let sandbox = Sandbox::new("changed");
        sandbox.install(&fsr()).unwrap();
        fs::write(sandbox.game.join("dxgi.dll"), b"the user put another ReShade here").unwrap();
        let error = plan_install(&sandbox.game, &sandbox.payload, &fsr(), None).unwrap_err();
        assert!(error.contains("dxgi.dll"));
        let log = uninstall(&sandbox.game, None).unwrap();
        assert!(log.iter().any(|l| l.starts_with("left") && l.contains("dxgi.dll")), "{:?}", log);
        assert!(sandbox.game.join("dxgi.dll").exists());
    }

    #[test]
    fn a_wrong_download_is_rejected() {
        let sandbox = Sandbox::new("download");
        let dlss = Choice { upscaler: Upscaler::Dlss, ..Choice::default() };
        let plan = plan_install(&sandbox.game, &sandbox.payload, &dlss, None).unwrap();
        let error = apply_install(&sandbox.game, &dlss, &plan, None, &|path| {
            fs::write(path, b"not the DLSS file").map_err(|e| e.to_string())
        })
        .unwrap_err();
        assert!(error.contains("checksum"), "{}", error);
        assert!(!sandbox.game.join(DLSS_FILE).exists() && !sandbox.game.join(DLSS_IN_SUBDIR).exists());
    }

    /// An installation as the tool of 1.0.0-beta.2 left it, with DLSS through OptiScaler, in one of its two layouts
    fn older_installation_with_optiscaler(sandbox: &Sandbox, menu: bool, dlss: &[u8]) {
        let game = &sandbox.game;
        let reshade = fs::read(sandbox.payload.join(P_RESHADE)).unwrap();
        let optiscaler = b"OptiScaler.dll of the older release".to_vec();
        fs::create_dir_all(game.join(SUBDIR)).unwrap();
        let mut manifest = Manifest::default();
        let mut put = |name: &str, content: &[u8]| {
            fs::write(game.join(name), content).unwrap();
            manifest.files.insert(name.to_string(), sys::sha256_file(&game.join(name)).unwrap());
        };
        for name in [P_ADDON, P_FFX_LOADER, P_FFX_UPSCALER] {
            put(&format!("{}\\{}", SUBDIR, name), b"older build");
        }
        if menu {
            put("dxgi.dll", &optiscaler);
            put("ReShade64.dll", &reshade);
        } else {
            put("dxgi.dll", &reshade);
            put("winmm.dll", &optiscaler);
        }
        put(DLSS_FILE, dlss);
        fs::write(game.join(OPTI_INI), "[DLSS]\r\nRenderPresetOverride=true\r\nRenderPresetForAll=13\r\n").unwrap();
        fs::write(game.join(RESHADE_INI), "[ADDON]\r\nAddonPath=.\\ACVUpscaler\r\n").unwrap();
        fs::write(game.join(ADDON_INI), "[ACVUpscaler]\r\nMode=FSR31\r\nKeepContexts=1\r\n").unwrap();
        for name in [OPTI_INI, RESHADE_INI, ADDON_INI] {
            manifest.files.insert(name.to_string(), "-".into());
        }
        manifest.reshade_ini_created = true;
        manifest.save(game).unwrap();
    }

    #[test]
    fn an_installation_with_optiscaler_is_taken_over() {
        for menu in [false, true] {
            let sandbox = Sandbox::new(if menu { "takeover_menu" } else { "takeover_steam" });
            let before = sandbox.listing();
            let dlss = b"stand-in for nvngx_dlss.dll";
            older_installation_with_optiscaler(&sandbox, menu, dlss);
            let hash = sys::sha256_file(&sandbox.game.join(DLSS_FILE)).unwrap();
            // The window shows what was installed: DLSS, with the model OptiScaler was set to
            let found = read_choice(&sandbox.game);
            assert_eq!((found.upscaler, found.model), (Upscaler::Dlss, Model::M));

            // Apply: nothing to download, the DLSS file moves next to the add-on, OptiScaler and its files go
            let plan = plan_install_for(&sandbox.game, &sandbox.payload, &found, None, &hash).unwrap();
            assert!(!needs_download(&plan), "{:?}", plan);
            apply_install(&sandbox.game, &found, &plan, None, &|_| unreachable!()).unwrap();
            let listing = sandbox.listing();
            assert_eq!(listing["dxgi.dll"], b"content of ReShade64.dll".to_vec());
            assert_eq!(listing[DLSS_IN_SUBDIR], dlss.to_vec());
            assert_eq!(listing["ACVUpscaler\\ACVUpscaler.addon64"], b"content of ACVUpscaler.addon64".to_vec());
            for gone in ["winmm.dll", "ReShade64.dll", DLSS_FILE, OPTI_INI] {
                assert!(!listing.contains_key(gone), "{} is still there", gone);
            }
            let addon_ini = sandbox.game.join(ADDON_INI);
            assert_eq!(sys::ini_get(&addon_ini, "ACVUpscaler", "Upscaler"), "DLSS");
            assert_eq!(sys::ini_get(&addon_ini, "ACVUpscaler", "DlssPreset"), "M");
            assert_eq!(sys::ini_get(&addon_ini, "ACVUpscaler", "KeepContexts"), "0");
            assert_eq!(read_choice(&sandbox.game), found);
            assert_eq!(command_line(&sandbox.game, None), "--install --upscaler dlss --model m --gain 4");

            // To FSR 3.1: the DLSS file goes as well
            sandbox.install(&fsr()).unwrap();
            assert!(!sandbox.game.join(DLSS_IN_SUBDIR).exists());
            assert_eq!(sys::ini_get(&addon_ini, "ACVUpscaler", "Upscaler"), "FSR31");

            uninstall(&sandbox.game, None).unwrap();
            assert_eq!(sandbox.listing(), before);
        }
    }}

// ---- The game's own settings file (Documents\Assassin's Creed Valhalla\ACValhalla.ini) ----

/// The FSR quality levels of the game. With ACVUpscaler they only decide the render resolution.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Quality {
    UltraQuality,
    Quality,
    Balanced,
    Performance,
}

impl Quality {
    pub const ALL: [Quality; 4] = [Quality::UltraQuality, Quality::Quality, Quality::Balanced, Quality::Performance];

    pub fn name(self) -> &'static str {
        match self {
            Quality::UltraQuality => "Ultra Quality",
            Quality::Quality => "Quality",
            Quality::Balanced => "Balanced",
            Quality::Performance => "Performance",
        }
    }

    /// `FSRQuality` in the settings file. 4 was read from the file after a session at 1976x1112; the other
    /// three follow the order of the game's menu and have not been checked against the file.
    fn ini_value(self) -> u32 {
        match self {
            Quality::UltraQuality => 4,
            Quality::Quality => 3,
            Quality::Balanced => 2,
            Quality::Performance => 1,
        }
    }

    fn from_ini(value: u32) -> Option<Quality> {
        Quality::ALL.into_iter().find(|q| q.ini_value() == value)
    }

    /// Scale factors of FSR 1
    fn ratio(self) -> f32 {
        match self {
            Quality::UltraQuality => 1.3,
            Quality::Quality => 1.5,
            Quality::Balanced => 1.7,
            Quality::Performance => 2.0,
        }
    }

    /// Render size for an output size. The game rounds up to a multiple of 8: 1976x1112 (Ultra Quality) and
    /// 1280x720 (Performance) were measured at 2560x1440.
    pub fn render_size(self, output: (u32, u32)) -> (u32, u32) {
        let round = |size: u32| ((size as f32 / self.ratio() / 8.0).ceil() as u32) * 8;
        (round(output.0), round(output.1))
    }
}

#[derive(Clone, PartialEq, Debug)]
pub struct GameSettings {
    pub path: PathBuf,
    /// `UpsampleType=1`: the game's FSR is on
    pub fsr_on: bool,
    pub quality: Option<Quality>,
    pub output: (u32, u32),
    /// The game's own resolution scale; anything but 1 changes the render size further
    pub pixel_density: f32,
    /// The game's frame rate limit: on or off (`IsFpsLimitEnabled`) and its value (`MaxFPS`)
    pub fps_limit: Option<(bool, u32)>,
    /// How detailed the game draws characters at a distance (`Character`, 0 to 4). The game's menu has no entry
    /// for it and its presets stop at 2. `None` when the file does not have the key.
    pub character: Option<u32>,
}

/// The highest value of the game's `Character` setting
pub const CHARACTER_FULL: u32 = 4;

pub fn game_ini_path(documents: &Path) -> PathBuf {
    documents.join(GAME_FOLDER).join("ACValhalla.ini")
}

/// Value of `key` in `[section]`, by plain text search (the game writes the file with bare line feeds)
fn text_ini_get(text: &str, section: &str, key: &str) -> Option<String> {
    let mut inside = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            inside = line.eq_ignore_ascii_case(&format!("[{}]", section));
        } else if inside {
            if let Some((name, value)) = line.split_once('=') {
                if name.trim().eq_ignore_ascii_case(key) {
                    return Some(value.trim().to_string());
                }
            }
        }
    }
    None
}

/// The text with one value changed and every other byte, line ends included, as it was. `None` when the key
/// is not there: nothing is ever added to the game's file.
fn text_ini_set(text: &str, section: &str, key: &str, value: &str) -> Option<String> {
    let mut inside = false;
    let mut position = 0;
    for line in text.split_inclusive('\n') {
        let content = line.trim_end_matches(['\r', '\n']);
        let trimmed = content.trim();
        if trimmed.starts_with('[') {
            inside = trimmed.eq_ignore_ascii_case(&format!("[{}]", section));
        } else if inside {
            if let Some((name, _)) = content.split_once('=') {
                if name.trim().eq_ignore_ascii_case(key) {
                    let line_end = &line[content.len()..];
                    return Some(format!("{}{}={}{}{}", &text[..position], name, value, line_end, &text[position + line.len()..]));
                }
            }
        }
        position += line.len();
    }
    None
}

pub fn read_game_settings(path: &Path) -> Option<GameSettings> {
    let text = fs::read_to_string(path).ok()?;
    let number = |key: &str| text_ini_get(&text, "Graphics", key).and_then(|v| v.parse::<f32>().ok());
    Some(GameSettings {
        path: path.to_path_buf(),
        fsr_on: number("UpsampleType")? == 1.0,
        quality: number("FSRQuality").and_then(|v| Quality::from_ini(v as u32)),
        output: (number("FullscreenWidth")? as u32, number("FullscreenHeight")? as u32),
        pixel_density: number("PixelDensity").unwrap_or(1.0),
        fps_limit: number("IsFpsLimitEnabled").zip(number("MaxFPS")).map(|(on, fps)| (on == 1.0, fps as u32)),
        character: number("Character").map(|v| v as u32),
    })
}

/// Writes the FSR quality into the game's settings file (FSR on) and tells the add-on the render scale that
/// goes with it, so that the texture LOD bias is right at the very next start. The file is copied to
/// `ACValhalla.ini.acvupscaler-backup` before it is changed for the first time.
pub fn set_game_quality(game: &Path, settings: &GameSettings, quality: Quality, game_exe_to_check: Option<&str>) -> Result<Vec<String>, String> {
    if let Some(exe) = game_exe_to_check {
        if sys::process_running(exe) {
            return Err("the game is running. Close it first: it writes its settings file when it exits.".into());
        }
    }
    let mut log = Vec::new();
    if settings.quality != Some(quality) || !settings.fsr_on {
        let text = fs::read_to_string(&settings.path).map_err(|e| format!("could not read {}: {}", settings.path.display(), e))?;
        let changed = text_ini_set(&text, "Graphics", "FSRQuality", &quality.ini_value().to_string())
            .and_then(|text| text_ini_set(&text, "Graphics", "UpsampleType", "1"))
            .ok_or("FSRQuality or UpsampleType is not in the game's settings file. Start the game once and set FSR in its menu.")?;
        write_game_ini(&settings.path, &changed, &mut log)?;
        log.push(format!("settings {} (FSR on, quality {})", settings.path.display(), quality.name()));
    }
    let addon_ini = game.join(ADDON_INI);
    if addon_ini.is_file() {
        let render = quality.render_size(settings.output);
        let scale = render.0 as f32 / settings.output.0.max(1) as f32;
        sys::ini_set(&addon_ini, "ACVUpscaler", "RenderScale", Some(&format!("{:.4}", scale)))?;
        log.push(format!("settings {} (RenderScale {:.4} for the texture LOD bias)", ADDON_INI, scale));
    }
    Ok(log)
}

/// Writes the game's settings file, with a backup copy before the first change ever
fn game_ini_backup(path: &Path) -> PathBuf {
    let mut backup = path.to_path_buf().into_os_string();
    backup.push(".acvupscaler-backup");
    PathBuf::from(backup)
}

fn write_game_ini(path: &Path, text: &str, log: &mut Vec<String>) -> Result<(), String> {
    let backup = game_ini_backup(path);
    if !backup.exists() {
        fs::copy(path, &backup).map_err(|e| format!("could not back up the game's settings file: {}", e))?;
        log.push(format!("backup   {}", backup.display()));
    }
    fs::write(path, text).map_err(|e| format!("could not write {}: {}", path.display(), e))
}

/// Command line only, for test series: the game's FSR off (`native`: `UpsampleType=0`, the game then renders at
/// the output resolution), the game's resolution scale (`PixelDensity`, 1 = 100 %) and / or the game's frame
/// rate limit (see `parse_fps_limit`).
pub fn set_game_render(
    game: &Path,
    ini: &Path,
    native: bool,
    pixel_density: Option<f32>,
    fps_limit: Option<(bool, Option<u32>)>,
    game_exe_to_check: Option<&str>,
) -> Result<Vec<String>, String> {
    if let Some(exe) = game_exe_to_check {
        if sys::process_running(exe) {
            return Err("the game is running. Close it first: it writes its settings file when it exits.".into());
        }
    }
    let mut log = Vec::new();
    let mut text = fs::read_to_string(ini).map_err(|e| format!("could not read {}: {}", ini.display(), e))?;
    if native {
        text = text_ini_set(&text, "Graphics", "UpsampleType", "0").ok_or("UpsampleType is not in the game's settings file")?;
        log.push(format!("settings {} (FSR off)", ini.display()));
    }
    if let Some(density) = pixel_density {
        if !(0.5..=2.0).contains(&density) {
            return Err("the resolution scale has to be between 0.5 and 2".into());
        }
        text = text_ini_set(&text, "Graphics", "PixelDensity", &format!("{:.5}", density)).ok_or("PixelDensity is not in the game's settings file")?;
        log.push(format!("settings {} (resolution scale {:.2})", ini.display(), density));
    }
    if let Some((on, fps)) = fps_limit {
        if let Some(fps) = fps {
            text = text_ini_set(&text, "Graphics", "MaxFPS", &fps.to_string()).ok_or("MaxFPS is not in the game's settings file")?;
        }
        text = text_ini_set(&text, "Graphics", "IsFpsLimitEnabled", if on { "1" } else { "0" }).ok_or("IsFpsLimitEnabled is not in the game's settings file")?;
        log.push(format!("settings {} (frame rate limit {})", ini.display(), if on { "on" } else { "off" }));
    }
    let mut written = Vec::new();
    write_game_ini(ini, &text, &mut written)?;
    log.extend(written);
    let addon_ini = game.join(ADDON_INI);
    if native && addon_ini.is_file() {
        sys::ini_set(&addon_ini, "ACVUpscaler", "RenderScale", Some("1.0000"))?;
    }
    Ok(log)
}

/// Characters in full detail also at a distance (`Character=4` in the game's settings file), or back to what the
/// game had: the value in the backup copy of the file, or 2 without one. Nothing is written when the file already
/// says so, or when it does not have the key.
pub fn set_character_detail(ini: &Path, full: bool, game_exe_to_check: Option<&str>) -> Result<Vec<String>, String> {
    let text = fs::read_to_string(ini).map_err(|e| format!("could not read {}: {}", ini.display(), e))?;
    let value = |text: &str| text_ini_get(text, "Graphics", "Character").and_then(|v| v.parse::<u32>().ok());
    let Some(current) = value(&text) else { return Ok(Vec::new()) };
    let wanted = if full {
        CHARACTER_FULL
    } else if current == CHARACTER_FULL {
        let before = fs::read_to_string(game_ini_backup(ini)).ok().and_then(|backup| value(&backup));
        before.filter(|v| *v < CHARACTER_FULL).unwrap_or(2)
    } else {
        current
    };
    if wanted == current {
        return Ok(Vec::new());
    }
    if let Some(exe) = game_exe_to_check {
        if sys::process_running(exe) {
            return Err("the game is running. Close it first: it writes its settings file when it exits.".into());
        }
    }
    let changed = text_ini_set(&text, "Graphics", "Character", &wanted.to_string()).ok_or("Character is not in the game's settings file")?;
    let mut log = Vec::new();
    write_game_ini(ini, &changed, &mut log)?;
    log.push(format!("settings {} (characters at a distance: {}, Character={})", ini.display(), if full { "full detail" } else { "as the game had them" }, wanted));
    Ok(log)
}

/// Render size of the last session as the add-on saw it, for the given output size
pub fn last_render_size(game: &Path, output: (u32, u32)) -> Option<(u32, u32)> {
    let scale: f32 = sys::ini_get(&game.join(ADDON_INI), "ACVUpscaler", "RenderScale").parse().ok()?;
    if !(0.2..=1.0).contains(&scale) {
        return None;
    }
    // The add-on stores the scale of the width; the game's sizes are multiples of 8
    let snap = |size: u32| ((size as f32 * scale / 8.0).round() as u32) * 8;
    Some((snap(output.0), snap(output.1)))
}

/// One paragraph: what the game will run with after "Install / Apply"
pub fn summary(choice: &Choice, quality: Option<Quality>, settings: Option<&GameSettings>) -> String {
    let upscaler = match choice.upscaler {
        Upscaler::Dlss => format!(
            "DLSS {}, model {}",
            DLSS_VERSION,
            match choice.model {
                Model::K => "K",
                Model::M => "M",
                Model::Default => "default of the file (M at Performance)",
            }
        ),
        Upscaler::Fsr31 => "FSR 3.1".to_string(),
        Upscaler::Off => "the game's own TAA and FSR 1 (ACVUpscaler off)".to_string(),
    };
    let resolution = match settings {
        None => "The game's settings file was not found: start the game once.".to_string(),
        Some(settings) => {
            let (w, h) = settings.output;
            let density = if (settings.pixel_density - 1.0).abs() > 0.001 {
                format!(" The game's resolution scale is {:.2}, which changes these numbers.", settings.pixel_density)
            } else {
                String::new()
            };
            match quality.or(if settings.fsr_on { settings.quality } else { None }) {
                Some(quality) => {
                    let (rw, rh) = quality.render_size(settings.output);
                    format!("{}: rendered at {}x{}, shown at {}x{}.{}", quality.name(), rw, rh, w, h, density)
                }
                None => format!("FSR is off in the game: rendered and shown at {}x{} (anti-aliasing only).{}", w, h, density),
            }
        }
    };
    let reflex = if choice.reflex && choice.upscaler != Upscaler::Off { " Reflex low latency on." } else if choice.reflex { " Reflex low latency on (it works with the upscaler off as well)." } else { "" };
    format!("You will play with: {}. {}{}", upscaler, resolution, reflex)
}

/// What needs a restart of the game, for the notes under the summary
pub fn restart_notes() -> String {
    let mut notes = String::from(
        "Everything set here takes effect at the next start of the game. \
         If you change the FSR quality inside the game instead, restart the game once afterwards: the texture sharpness follows one start behind",
    );
    notes += ".";
    notes
}

#[cfg(test)]
mod settings_tests {
    use super::*;

    const SAMPLE: &str = "[Language]\nSubtitles=1\n[Graphics]\nFullscreenWidth=2560\nFullscreenHeight=1440\nMaxFPS=30\nIsFpsLimitEnabled=0\nPixelDensity=1.00000\nUpsampleType=0\nFSRQuality=4\nAntialiasing=2\n[Options]\nFSRQuality=9\n";

    #[test]
    fn render_sizes_match_what_the_game_was_seen_to_use() {
        assert_eq!(Quality::UltraQuality.render_size((2560, 1440)), (1976, 1112));
        assert_eq!(Quality::Performance.render_size((2560, 1440)), (1280, 720));
        assert_eq!(Quality::Balanced.render_size((2560, 1440)), (1512, 848));
        assert_eq!(Quality::Quality.render_size((2560, 1440)), (1712, 960));
    }

    #[test]
    fn characters_at_full_detail_and_back() {
        let root = std::env::temp_dir().join(format!("acv_setup_test_characters_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        let ini = root.join("ACValhalla.ini");
        // A file without the key is left alone
        fs::write(&ini, SAMPLE).unwrap();
        assert!(set_character_detail(&ini, true, None).unwrap().is_empty());
        assert_eq!(fs::read_to_string(&ini).unwrap(), SAMPLE);

        let with_key = SAMPLE.replace("Antialiasing=2\n", "Antialiasing=2\nCharacter=1\n");
        fs::write(&ini, &with_key).unwrap();
        assert!(set_character_detail(&ini, false, None).unwrap().is_empty(), "off and not at 4: nothing to do");
        set_character_detail(&ini, true, None).unwrap();
        let settings = read_game_settings(&ini).unwrap();
        assert_eq!(settings.character, Some(4));
        assert!(game_render_args(Some(&settings)).ends_with(" --full-characters"));
        assert_eq!(fs::read_to_string(&ini).unwrap(), with_key.replace("Character=1", "Character=4"));
        // Back to the value of the backup copy, which was made before the first change
        set_character_detail(&ini, false, None).unwrap();
        assert_eq!(fs::read_to_string(&ini).unwrap(), with_key);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn one_value_changes_and_nothing_else() {
        let changed = text_ini_set(SAMPLE, "Graphics", "FSRQuality", "2").unwrap();
        assert_eq!(changed, SAMPLE.replace("FSRQuality=4", "FSRQuality=2"));
        assert!(changed.ends_with("[Options]\nFSRQuality=9\n"), "the same key in another section is not touched");
        assert!(!changed.contains('\r'));
        assert_eq!(text_ini_set(SAMPLE, "Graphics", "NoSuchKey", "1"), None);
        let crlf = SAMPLE.replace('\n', "\r\n");
        assert_eq!(text_ini_set(&crlf, "Graphics", "FSRQuality", "1").unwrap(), crlf.replace("FSRQuality=4", "FSRQuality=1"));
    }

    #[test]
    fn quality_is_written_with_a_backup_and_the_render_scale() {
        let root = std::env::temp_dir().join(format!("acv_setup_test_gameini_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("game").join(SUBDIR)).unwrap();
        let ini = root.join("ACValhalla.ini");
        fs::write(&ini, SAMPLE).unwrap();
        fs::write(root.join("game").join(ADDON_INI), "[ACVUpscaler]\r\nMode=FSR31\r\n").unwrap();

        let settings = read_game_settings(&ini).unwrap();
        assert_eq!((settings.fsr_on, settings.quality, settings.output), (false, Some(Quality::UltraQuality), (2560, 1440)));
        assert!(summary(&Choice::default(), None, Some(&settings)).contains("FSR is off in the game"));
        assert!(summary(&Choice::default(), Some(Quality::Balanced), Some(&settings)).contains("Balanced: rendered at 1512x848, shown at 2560x1440"));

        set_game_quality(&root.join("game"), &settings, Quality::Balanced, None).unwrap();
        let after = read_game_settings(&ini).unwrap();
        assert_eq!((after.fsr_on, after.quality), (true, Some(Quality::Balanced)));
        assert_eq!(fs::read_to_string(root.join("ACValhalla.ini.acvupscaler-backup")).unwrap(), SAMPLE);
        assert_eq!(sys::ini_get(&root.join("game").join(ADDON_INI), "ACVUpscaler", "RenderScale"), "0.5906");
        assert_eq!(last_render_size(&root.join("game"), (2560, 1440)), Some((1512, 848)));

        // A second change keeps the first backup
        set_game_quality(&root.join("game"), &after, Quality::Performance, None).unwrap();
        assert_eq!(fs::read_to_string(root.join("ACValhalla.ini.acvupscaler-backup")).unwrap(), SAMPLE);

        // Test series: FSR off with a resolution scale, reported back as command-line options
        set_game_render(&root.join("game"), &ini, true, Some(2.0), None, None).unwrap();
        let native = read_game_settings(&ini).unwrap();
        assert_eq!((native.fsr_on, native.pixel_density), (false, 2.0));
        assert_eq!(game_render_args(Some(&native)), " --quality native --pixel-density 2 --fps-limit off,30");
        assert_eq!(sys::ini_get(&root.join("game").join(ADDON_INI), "ACVUpscaler", "RenderScale"), "1.0000");
        assert!(set_game_render(&root.join("game"), &ini, false, Some(3.0), None, None).is_err());

        // Frame rate limit on for recordings, and back to what it was from the reported options
        set_game_render(&root.join("game"), &ini, false, None, parse_fps_limit("60"), None).unwrap();
        assert_eq!(read_game_settings(&ini).unwrap().fps_limit, Some((true, 60)));
        set_game_render(&root.join("game"), &ini, false, None, parse_fps_limit("off,30"), None).unwrap();
        assert_eq!(read_game_settings(&ini).unwrap().fps_limit, Some((false, 30)));
        assert_eq!(parse_fps_limit("off"), Some((false, None)));
        assert_eq!(parse_fps_limit("fast"), None);
        let _ = fs::remove_dir_all(&root);
    }
}
