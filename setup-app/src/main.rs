//! ACVUpscalerSetup: installs ACVUpscaler into the game folder, changes its settings and removes it again.
//! One window made of plain Win32 controls; `--game-dir` on the command line runs it without the window.
#![windows_subsystem = "windows"]

mod core;
mod sys;

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};

use windows_sys::core::{w, PCWSTR};
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows_sys::Win32::Graphics::Gdi::{CreateFontIndirectW, UpdateWindow, COLOR_BTNFACE, HBRUSH, HFONT};
use windows_sys::Win32::System::Com::CoTaskMemFree;
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::Controls::{InitCommonControlsEx, ICC_BAR_CLASSES, ICC_STANDARD_CLASSES, INITCOMMONCONTROLSEX, TBS_AUTOTICKS};
use windows_sys::Win32::UI::HiDpi::GetDpiForSystem;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::EnableWindow;
use windows_sys::Win32::UI::Shell::{SHBrowseForFolderW, SHGetPathFromIDListW, ShellExecuteW, BIF_NEWDIALOGSTYLE, BIF_RETURNONLYFSDIRS, BROWSEINFOW};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::core::{Choice, GameSettings, Model, Overlay, Quality, Upscaler};

// Messages of the trackbar and the edit control (commctrl.h, winuser.h)
const TBM_GETPOS: u32 = 0x0400;
const TBM_SETPOS: u32 = 0x0405;
const TBM_SETRANGE: u32 = 0x0406;
const EM_SETSEL: u32 = 0x00B1;
const EM_SCROLLCARET: u32 = 0x00B7;

const ID_BROWSE: usize = 100;
const ID_DLSS: usize = 101;
const ID_FSR: usize = 102;
const ID_OFF: usize = 103;
const ID_MODEL: usize = 104;
const ID_STEAM: usize = 105;
const ID_MENU: usize = 106;
const ID_GAIN: usize = 107;
const ID_LOD: usize = 108;
const ID_DEV: usize = 109;
const ID_APPLY: usize = 110;
const ID_UNINSTALL: usize = 111;
const ID_OPEN: usize = 112;
const ID_CLOSE: usize = 113;
const ID_QUALITY: usize = 114;
const ID_SHARPEN: usize = 115;

struct App {
    window: HWND,
    folder: HWND,
    status: HWND,
    dlss: HWND,
    fsr: HWND,
    off: HWND,
    model: HWND,
    steam: HWND,
    menu: HWND,
    gain_label: HWND,
    gain: HWND,
    lod: HWND,
    sharpen: HWND,
    dev: HWND,
    apply: HWND,
    uninstall: HWND,
    log: HWND,
    quality: HWND,
    summary: HWND,
    notes: HWND,
    game: Option<PathBuf>,
    /// The game's own settings file, when it exists
    settings: Option<GameSettings>,
    payload: PathBuf,
    log_text: String,
}

fn load_game_settings() -> Option<GameSettings> {
    core::read_game_settings(&core::game_ini_path(&sys::documents_dir()?))
}

thread_local! {
    static APP: RefCell<Option<App>> = const { RefCell::new(None) };
}

fn payload_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|exe| exe.parent().map(|dir| dir.join("payload"))).unwrap_or_else(|| PathBuf::from("payload"))
}

fn set_text(window: HWND, text: &str) {
    let text = sys::wide(text);
    unsafe {
        SetWindowTextW(window, text.as_ptr());
    }
}

fn checked(window: HWND) -> bool {
    unsafe { SendMessageW(window, BM_GETCHECK, 0, 0) == 1 }
}

fn set_checked(window: HWND, value: bool) {
    unsafe {
        SendMessageW(window, BM_SETCHECK, usize::from(value), 0);
    }
}

fn message(window: HWND, text: &str, flags: MESSAGEBOX_STYLE) -> MESSAGEBOX_RESULT {
    let text = sys::wide(text);
    unsafe { MessageBoxW(window, text.as_ptr(), w!("ACVUpscaler Setup"), flags) }
}

impl App {
    fn append_log(&mut self, lines: &[String]) {
        for line in lines {
            self.log_text += line;
            self.log_text += "\r\n";
        }
        set_text(self.log, &self.log_text);
        unsafe {
            // Scroll to the end
            SendMessageW(self.log, EM_SETSEL, self.log_text.len(), self.log_text.len() as isize);
            SendMessageW(self.log, EM_SCROLLCARET, 0, 0);
        }
    }

    fn choice(&self) -> Choice {
        let model = match unsafe { SendMessageW(self.model, CB_GETCURSEL, 0, 0) } {
            1 => Model::M,
            2 => Model::Default,
            _ => Model::K,
        };
        Choice {
            upscaler: if checked(self.dlss) {
                Upscaler::Dlss
            } else if checked(self.off) {
                Upscaler::Off
            } else {
                Upscaler::Fsr31
            },
            model,
            overlay: if checked(self.menu) { Overlay::OptiMenu } else { Overlay::Steam },
            gain: unsafe { SendMessageW(self.gain, TBM_GETPOS, 0, 0) } as u32,
            lod_bias: checked(self.lod),
            game_sharpening: checked(self.sharpen),
            dev_mode: checked(self.dev),
        }
    }

    /// The FSR quality picked in the list; `None` leaves the game's setting as it is
    fn chosen_quality(&self) -> Option<Quality> {
        self.settings.as_ref()?;
        let index = unsafe { SendMessageW(self.quality, CB_GETCURSEL, 0, 0) };
        Quality::ALL.get(usize::try_from(index).ok()?).copied()
    }

    fn show_choice(&self, choice: &Choice) {
        set_checked(self.dlss, choice.upscaler == Upscaler::Dlss);
        set_checked(self.fsr, choice.upscaler == Upscaler::Fsr31);
        set_checked(self.off, choice.upscaler == Upscaler::Off);
        set_checked(self.steam, choice.overlay == Overlay::Steam);
        set_checked(self.menu, choice.overlay == Overlay::OptiMenu);
        set_checked(self.lod, choice.lod_bias);
        set_checked(self.sharpen, choice.game_sharpening);
        set_checked(self.dev, choice.dev_mode);
        let index = match choice.model {
            Model::K => 0,
            Model::M => 1,
            Model::Default => 2,
        };
        unsafe {
            SendMessageW(self.model, CB_SETCURSEL, index, 0);
            SendMessageW(self.gain, TBM_SETPOS, 1, choice.gain as isize);
        }
        self.update_enabled();
    }

    /// The DLSS options only make sense with DLSS; everything is locked without a game folder
    fn update_enabled(&self) {
        let have_game = self.game.is_some();
        let dlss = checked(self.dlss);
        unsafe {
            for control in [self.model, self.steam, self.menu] {
                EnableWindow(control, i32::from(have_game && dlss));
            }
            for control in [self.dlss, self.fsr, self.off, self.gain, self.lod, self.sharpen, self.dev, self.apply] {
                EnableWindow(control, i32::from(have_game));
            }
            EnableWindow(self.uninstall, i32::from(self.game.as_deref().is_some_and(core::is_installed)));
            EnableWindow(self.quality, i32::from(have_game && self.settings.is_some()));
        }
        set_text(self.gain_label, &format!("Particle mask strength (rain, fire): {}", unsafe { SendMessageW(self.gain, TBM_GETPOS, 0, 0) }));

        let choice = self.choice();
        let mut summary = core::summary(&choice, self.chosen_quality(), self.settings.as_ref());
        if let (Some(game), Some(settings)) = (&self.game, &self.settings) {
            if let Some((w, h)) = core::last_render_size(game, settings.output) {
                summary += &format!(" Last session ran at {}x{}.", w, h);
            }
        }
        set_text(self.summary, &summary);
        set_text(self.notes, &core::restart_notes(&choice));
    }

    fn refresh(&mut self) {
        self.settings = load_game_settings();
        let index = self.settings.as_ref().filter(|s| s.fsr_on).and_then(|s| s.quality).and_then(|q| Quality::ALL.iter().position(|x| *x == q));
        unsafe {
            SendMessageW(self.quality, CB_SETCURSEL, index.map_or(usize::MAX, |i| i), 0);
        }
        match self.game.clone() {
            Some(game) => {
                set_text(self.folder, &game.to_string_lossy());
                let installed = core::is_installed(&game);
                let running = sys::process_running(core::GAME_EXE);
                set_text(
                    self.status,
                    &format!(
                        "{}{}",
                        if installed { "ACVUpscaler is installed in this folder." } else { "ACVUpscaler is not installed in this folder yet." },
                        if running { "  The game is running: close it before changing anything." } else { "" }
                    ),
                );
                if installed {
                    self.show_choice(&core::read_choice(&game));
                }
            }
            None => {
                set_text(self.folder, "");
                set_text(self.status, "The game folder was not found. Press Browse and pick the folder that contains ACValhalla.exe.");
            }
        }
        self.update_enabled();
    }

    fn browse(&mut self) {
        let mut info: BROWSEINFOW = unsafe { std::mem::zeroed() };
        info.hwndOwner = self.window;
        info.lpszTitle = w!("Folder of Assassin's Creed Valhalla (the one with ACValhalla.exe)");
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        let mut buffer = [0u16; 1024];
        let picked = unsafe {
            let list = SHBrowseForFolderW(&info);
            if list.is_null() {
                return;
            }
            let ok = SHGetPathFromIDListW(list, buffer.as_mut_ptr());
            CoTaskMemFree(list as *const _);
            ok != 0
        };
        if !picked {
            return;
        }
        let folder = PathBuf::from(sys::from_wide(&buffer));
        if folder.join(core::GAME_EXE).is_file() {
            self.game = Some(folder);
            self.show_choice(&Choice::default());
            self.refresh();
        } else {
            message(self.window, &format!("{} is not in that folder.", core::GAME_EXE), MB_OK | MB_ICONWARNING);
        }
    }

    fn apply(&mut self) {
        let Some(game) = self.game.clone() else { return };
        let choice = self.choice();
        let actions = match core::plan_install(&game, &self.payload, &choice, None) {
            Ok(actions) => actions,
            Err(error) => {
                message(self.window, &error, MB_OK | MB_ICONWARNING);
                return;
            }
        };
        let mut question = format!("Game folder:\r\n{}\r\n\r\nThis will be done:\r\n{}\r\n", game.display(), core::describe(&actions));
        if core::needs_download(&actions) {
            question += &format!(
                "\r\nThe DLSS file ({} MB) is downloaded from NVIDIA's GitHub. By continuing you accept NVIDIA's licence for it:\r\n{}\r\n",
                59,
                core::DLSS_LICENCE_URL
            );
        }
        let quality = self.chosen_quality();
        if let (Some(quality), Some(settings)) = (quality, &self.settings) {
            if settings.quality != Some(quality) || !settings.fsr_on {
                question += &format!(
                    "\r\nIn the game's settings file ({}) FSR is switched on with quality {}. A backup copy of the file is made the first time.\r\n",
                    settings.path.display(),
                    quality.name()
                );
            }
        }
        question += "\r\nNo file in the game folder that belongs to the game is touched. Continue?";
        if message(self.window, &question, MB_YESNO | MB_ICONQUESTION) != IDYES {
            return;
        }

        unsafe {
            SetCursor(LoadCursorW(null_mut(), IDC_WAIT));
        }
        if core::needs_download(&actions) {
            self.append_log(&["downloading DLSS, this can take a minute ...".into()]);
        }
        let result = core::apply_install(&game, &self.payload, &choice, &actions, Some(core::GAME_EXE), &|path| sys::download(core::DLSS_URL, path));
        unsafe {
            SetCursor(LoadCursorW(null_mut(), IDC_ARROW));
        }
        // The game's own quality setting, and the render scale the add-on needs for it at the next start
        let result = result.and_then(|mut lines| {
            if let (Some(quality), Some(settings)) = (quality, &self.settings) {
                lines.extend(core::set_game_quality(&game, settings, quality, Some(core::GAME_EXE))?);
            }
            Ok(lines)
        });
        match result {
            Ok(mut lines) => {
                lines.push("done. The changes take effect at the next start of the game.".into());
                self.append_log(&lines);
            }
            Err(error) => {
                self.append_log(&[format!("FAILED: {}", error)]);
                message(self.window, &error, MB_OK | MB_ICONERROR);
            }
        }
        self.refresh();
    }

    fn uninstall(&mut self) {
        let Some(game) = self.game.clone() else { return };
        let question = format!(
            "Game folder:\r\n{}\r\n\r\nThis will be done:\r\n{}\r\n\r\nNo file of the game itself is touched. Continue?",
            game.display(),
            core::plan_uninstall(&game).join("\r\n")
        );
        if message(self.window, &question, MB_YESNO | MB_ICONQUESTION) != IDYES {
            return;
        }
        match core::uninstall(&game, Some(core::GAME_EXE)) {
            Ok(mut lines) => {
                lines.push("uninstalled.".into());
                self.append_log(&lines);
            }
            Err(error) => {
                self.append_log(&[format!("FAILED: {}", error)]);
                message(self.window, &error, MB_OK | MB_ICONERROR);
            }
        }
        self.refresh();
    }
}

/// Creates one child control. Coordinates are for 96 dpi and get scaled.
unsafe fn control(parent: HWND, font: HFONT, scale: f32, class: PCWSTR, text: &str, style: u32, ex_style: u32, rect: (i32, i32, i32, i32), id: usize) -> HWND {
    let text = sys::wide(text);
    let s = |value: i32| (value as f32 * scale).round() as i32;
    let window = CreateWindowExW(
        ex_style,
        class,
        text.as_ptr(),
        WS_CHILD | WS_VISIBLE | style,
        s(rect.0),
        s(rect.1),
        s(rect.2),
        s(rect.3),
        parent,
        id as HMENU,
        GetModuleHandleW(null()),
        null(),
    );
    SendMessageW(window, WM_SETFONT, font as usize, 1);
    window
}

unsafe fn create_controls(window: HWND) -> App {
    let scale = GetDpiForSystem() as f32 / 96.0;
    let mut metrics: NONCLIENTMETRICSW = std::mem::zeroed();
    metrics.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &mut metrics as *mut _ as *mut _, 0);
    let font = CreateFontIndirectW(&metrics.lfMessageFont);

    let c = |class: PCWSTR, text: &str, style: u32, ex_style: u32, rect: (i32, i32, i32, i32), id: usize| control(window, font, scale, class, text, style, ex_style, rect, id);
    let (radio, check, push) = (BS_AUTORADIOBUTTON as u32 | WS_TABSTOP, BS_AUTOCHECKBOX as u32 | WS_TABSTOP, BS_PUSHBUTTON as u32 | WS_TABSTOP);

    c(w!("STATIC"), "Game folder:", 0, 0, (12, 14, 80, 18), 0);
    let folder = c(w!("EDIT"), "", ES_READONLY as u32 | ES_AUTOHSCROLL as u32, WS_EX_CLIENTEDGE, (96, 11, 388, 22), 0);
    c(w!("BUTTON"), "Browse...", push, 0, (492, 10, 80, 24), ID_BROWSE);
    let status = c(w!("STATIC"), "", 0, 0, (12, 40, 560, 34), 0);

    c(w!("BUTTON"), "Upscaler", BS_GROUPBOX as u32, 0, (12, 78, 560, 94), 0);
    let dlss = c(w!("BUTTON"), "DLSS - best picture, NVIDIA RTX cards only (uses OptiScaler)", radio | WS_GROUP, 0, (24, 98, 536, 20), ID_DLSS);
    let fsr = c(w!("BUTTON"), "FSR 3.1 - works on every graphics card", radio, 0, (24, 121, 536, 20), ID_FSR);
    let off = c(w!("BUTTON"), "Off - the game's own anti-aliasing and FSR 1", radio, 0, (24, 144, 536, 20), ID_OFF);

    c(w!("STATIC"), "Render quality (the game's FSR setting):", 0, 0, (12, 183, 250, 18), 0);
    let quality = c(w!("COMBOBOX"), "", CBS_DROPDOWNLIST as u32 | WS_TABSTOP | WS_VSCROLL, 0, (268, 179, 304, 120), ID_QUALITY);
    for item in Quality::ALL {
        let text = sys::wide(item.name());
        SendMessageW(quality, CB_ADDSTRING, 0, text.as_ptr() as isize);
    }

    c(w!("BUTTON"), "DLSS options", BS_GROUPBOX as u32, 0, (12, 210, 560, 100), 0);
    c(w!("STATIC"), "Model:", 0, 0, (24, 233, 50, 18), 0);
    let model = c(w!("COMBOBOX"), "", CBS_DROPDOWNLIST as u32 | WS_TABSTOP | WS_VSCROLL, 0, (78, 229, 482, 120), ID_MODEL);
    for item in [w!("K - fast, recommended for RTX 20 and 30 series"), w!("M - a little sharper, clearly slower on RTX 20 and 30 series"), w!("Default of the DLSS file")] {
        SendMessageW(model, CB_ADDSTRING, 0, item as isize);
    }
    let steam = c(w!("BUTTON"), "Keep the Steam overlay (OptiScaler's own menu is not available)", radio | WS_GROUP, 0, (24, 260, 536, 20), ID_STEAM);
    let menu = c(w!("BUTTON"), "OptiScaler's menu on the Insert key (the Steam overlay does not show)", radio, 0, (24, 283, 536, 20), ID_MENU);

    let gain_label = c(w!("STATIC"), "", 0, 0, (12, 322, 300, 18), 0);
    let gain = c(w!("msctls_trackbar32"), "", TBS_AUTOTICKS | WS_TABSTOP, 0, (316, 318, 256, 28), ID_GAIN);
    SendMessageW(gain, TBM_SETRANGE, 1, (16 << 16) as isize);
    let lod = c(w!("BUTTON"), "Sharper textures (texture LOD bias matched to the render quality)", check, 0, (12, 350, 560, 20), ID_LOD);
    let sharpen = c(w!("BUTTON"), "Sharpening pass of the game on top (off: softer picture, slightly faster)", check, 0, (12, 373, 560, 20), ID_SHARPEN);
    let dev = c(w!("BUTTON"), "Developer tools (keys F6 to F10 in the game, detailed log)", check, 0, (12, 396, 560, 20), ID_DEV);

    // What the choices above amount to, and what needs a restart of the game
    let summary = c(w!("STATIC"), "", 0, WS_EX_STATICEDGE, (12, 425, 560, 54), 0);
    let notes = c(w!("STATIC"), "", 0, 0, (12, 485, 560, 52), 0);

    let apply = c(w!("BUTTON"), "Install / Apply", push | BS_DEFPUSHBUTTON as u32, 0, (12, 543, 150, 28), ID_APPLY);
    let uninstall = c(w!("BUTTON"), "Uninstall", push, 0, (170, 543, 110, 28), ID_UNINSTALL);
    c(w!("BUTTON"), "Open game folder", push, 0, (288, 543, 150, 28), ID_OPEN);
    c(w!("BUTTON"), "Close", push, 0, (462, 543, 110, 28), ID_CLOSE);

    let log = c(
        w!("EDIT"),
        "",
        ES_READONLY as u32 | ES_MULTILINE as u32 | ES_AUTOVSCROLL as u32 | WS_VSCROLL,
        WS_EX_CLIENTEDGE,
        (12, 581, 560, 110),
        0,
    );

    App {
        window,
        folder,
        status,
        dlss,
        fsr,
        off,
        model,
        steam,
        menu,
        gain_label,
        gain,
        lod,
        sharpen,
        dev,
        apply,
        uninstall,
        log,
        quality,
        summary,
        notes,
        game: sys::find_game(core::GAME_FOLDER, core::GAME_EXE),
        settings: None,
        payload: payload_dir(),
        log_text: String::new(),
    }
}

fn with_app(action: impl FnOnce(&mut App)) {
    APP.with(|cell| {
        // A message box inside an action pumps messages: those re-entrant ones are simply not handled here
        if let Ok(mut app) = cell.try_borrow_mut() {
            if let Some(app) = app.as_mut() {
                action(app);
            }
        }
    });
}

unsafe extern "system" fn window_proc(window: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        WM_CREATE => {
            let mut app = create_controls(window);
            app.show_choice(&Choice::default());
            app.refresh();
            if let Err(error) = core::check_folders(app.game.as_deref().unwrap_or(Path::new(".")), &app.payload) {
                if app.game.is_some() {
                    app.append_log(&[error]);
                }
            }
            APP.with(|cell| *cell.borrow_mut() = Some(app));
            0
        }
        WM_COMMAND => {
            match wparam & 0xFFFF {
                ID_BROWSE => with_app(App::browse),
                ID_APPLY => with_app(App::apply),
                ID_UNINSTALL => with_app(App::uninstall),
                ID_OPEN => with_app(|app| {
                    if let Some(game) = &app.game {
                        let path = sys::wide_path(game);
                        ShellExecuteW(app.window, w!("open"), path.as_ptr(), null(), null(), SW_SHOWNORMAL);
                    }
                }),
                ID_CLOSE => {
                    DestroyWindow(window);
                }
                // Any change of a choice updates the summary
                ID_DLSS | ID_FSR | ID_OFF | ID_STEAM | ID_MENU | ID_MODEL | ID_QUALITY | ID_LOD | ID_SHARPEN | ID_DEV => with_app(|app| app.update_enabled()),
                _ => {}
            }
            0
        }
        // The slider moved
        WM_HSCROLL => {
            with_app(|app| app.update_enabled());
            0
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            0
        }
        _ => DefWindowProcW(window, msg, wparam, lparam),
    }
}

fn run_window() {
    unsafe {
        let controls = INITCOMMONCONTROLSEX { dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32, dwICC: ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);

        let instance = GetModuleHandleW(null());
        let class = WNDCLASSW {
            style: 0,
            lpfnWndProc: Some(window_proc),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: instance,
            hIcon: LoadIconW(null_mut(), IDI_APPLICATION),
            hCursor: LoadCursorW(null_mut(), IDC_ARROW),
            hbrBackground: (COLOR_BTNFACE + 1) as usize as HBRUSH,
            lpszMenuName: null(),
            lpszClassName: w!("ACVUpscalerSetup"),
        };
        RegisterClassW(&class);

        let scale = GetDpiForSystem() as f32 / 96.0;
        let style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        let mut rect = windows_sys::Win32::Foundation::RECT { left: 0, top: 0, right: (584.0 * scale) as i32, bottom: (703.0 * scale) as i32 };
        AdjustWindowRect(&mut rect, style, 0);
        let window = CreateWindowExW(
            0,
            w!("ACVUpscalerSetup"),
            w!("ACVUpscaler Setup 1.0.0-beta.1"),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            null_mut(),
            null_mut(),
            instance,
            null(),
        );
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        let mut msg: MSG = std::mem::zeroed();
        while GetMessageW(&mut msg, null_mut(), 0, 0) > 0 {
            // Tab and arrow keys between the controls
            if IsDialogMessageW(window, &msg) == 0 {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

/// `--game-dir <dir> --install|--uninstall|--show [options]`: no window, the result goes to `--log <file>`.
/// `--show` changes nothing and reports the present installation as the options that would recreate it.
/// Options: `--payload <dir>`, `--upscaler dlss|fsr31|off`, `--model k|m|default`, `--overlay steam|menu`,
/// `--gain <0..16>`, `--no-lod-bias`, `--no-game-sharpening`, `--dev`, `--dlss-file <file>` (use this copy instead of downloading),
/// `--quality uq|quality|balanced|performance` with `--game-ini <file>` (the game's own settings file; default:
/// the one in Documents), `--no-running-check`. For test series also `--quality native` (the game's FSR off) and
/// `--pixel-density <0.5..2>` (the game's resolution scale) and `--fps-limit <fps>|off|off,<fps>` (the game's
/// frame rate limit).
fn run_command_line(args: &[String]) -> i32 {
    let value = |name: &str| args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned();
    let flag = |name: &str| args.iter().any(|a| a == name);

    let game = PathBuf::from(value("--game-dir").unwrap_or_default());
    let payload = value("--payload").map(PathBuf::from).unwrap_or_else(payload_dir);
    let running_check = if flag("--no-running-check") { None } else { Some(core::GAME_EXE) };
    let choice = Choice {
        upscaler: match value("--upscaler").as_deref() {
            Some("dlss") => Upscaler::Dlss,
            Some("off") => Upscaler::Off,
            _ => Upscaler::Fsr31,
        },
        model: match value("--model").as_deref() {
            Some("m") => Model::M,
            Some("default") => Model::Default,
            _ => Model::K,
        },
        overlay: if value("--overlay").as_deref() == Some("menu") { Overlay::OptiMenu } else { Overlay::Steam },
        gain: value("--gain").and_then(|v| v.parse().ok()).unwrap_or(4).min(16),
        lod_bias: !flag("--no-lod-bias"),
        game_sharpening: !flag("--no-game-sharpening"),
        dev_mode: flag("--dev"),
    };
    let dlss_file = value("--dlss-file").map(PathBuf::from);

    let quality = match value("--quality").as_deref() {
        Some("uq") => Some(Quality::UltraQuality),
        Some("quality") => Some(Quality::Quality),
        Some("balanced") => Some(Quality::Balanced),
        Some("performance") => Some(Quality::Performance),
        _ => None,
    };

    // Test series only: the game's FSR off and the game's resolution scale
    let native = value("--quality").as_deref() == Some("native");
    let pixel_density = value("--pixel-density").and_then(|v| v.parse::<f32>().ok());
    let game_ini = value("--game-ini").map(PathBuf::from).or_else(|| sys::documents_dir().map(|d| core::game_ini_path(&d)));
    let set_render = |mut lines: Vec<String>| -> Result<Vec<String>, String> {
        let fps_limit = match value("--fps-limit") {
            Some(text) => Some(core::parse_fps_limit(&text).ok_or("--fps-limit takes a frame rate, off, or off,<frame rate>")?),
            None => None,
        };
        if native || pixel_density.is_some() || fps_limit.is_some() {
            let ini = game_ini.as_deref().ok_or("the game's settings file was not found")?;
            lines.extend(core::set_game_render(&game, ini, native, pixel_density, fps_limit, running_check)?);
        }
        Ok(lines)
    };

    let result = if flag("--show") {
        let settings = game_ini.as_deref().and_then(core::read_game_settings);
        Ok(vec![core::command_line(&game, settings.as_ref())])
    } else if flag("--uninstall") {
        core::uninstall(&game, running_check).and_then(set_render)
    } else if flag("--install") {
        core::plan_install(&game, &payload, &choice, dlss_file.as_deref())
            .and_then(|actions| core::apply_install(&game, &payload, &choice, &actions, running_check, &|path| sys::download(core::DLSS_URL, path)))
            .and_then(|mut lines| {
                if let Some(quality) = quality {
                    let ini = value("--game-ini").map(PathBuf::from).or_else(|| sys::documents_dir().map(|d| core::game_ini_path(&d)));
                    let settings = ini.as_deref().and_then(core::read_game_settings).ok_or("the game's settings file was not found")?;
                    lines.extend(core::set_game_quality(&game, &settings, quality, running_check)?);
                    lines.push(core::summary(&choice, Some(quality), Some(&settings)));
                }
                Ok(lines)
            })
            .and_then(set_render)
    } else {
        Err("give --install, --uninstall or --show".into())
    };

    let (code, text) = match result {
        Ok(lines) => (0, format!("OK\r\n{}\r\n", lines.join("\r\n"))),
        Err(error) => (1, format!("FAILED: {}\r\n", error)),
    };
    if let Some(log) = value("--log") {
        let _ = std::fs::write(log, text);
    }
    code
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--game-dir") {
        std::process::exit(run_command_line(&args));
    }
    run_window();
}
