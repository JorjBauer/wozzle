use std::path::PathBuf;

use tauri::ipc::{InvokeBody, Request};
use tauri::{DragDropEvent, Manager, RunEvent, WebviewEvent};
use tauri_plugin_fs::FsExt;

// Where a file dragged out of the window waits while the drag is on. The
// OS copies it from here to wherever it's dropped.
fn drag_dir() -> PathBuf {
  std::env::temp_dir().join(format!("wozzle-drag-{}", std::process::id()))
}

// Write a file out of the image to disk, so it can be dragged to the
// Finder (or Explorer, or a file manager). The bytes are the request's
// raw body and the name is in a header, which saves turning a 32MB file
// into a JSON array of numbers.
#[tauri::command]
fn stage_drag_file(request: Request<'_>) -> Result<String, String> {
  let InvokeBody::Raw(bytes) = request.body() else {
    return Err("stage_drag_file wants raw bytes".into());
  };
  let name = request
    .headers()
    .get("x-wozzle-name")
    .and_then(|v| v.to_str().ok())
    .unwrap_or("file");
  // Apple II names are ASCII; anything that could leave the folder goes
  let safe: String = name
    .chars()
    .map(|c| if c.is_ascii_alphanumeric() || " ._-".contains(c) { c } else { '_' })
    .collect();
  let safe = safe.trim_start_matches('.');
  let safe = if safe.is_empty() { "file" } else { safe };

  // A folder per drag, so two files of the same name never collide
  let dir = drag_dir().join(format!("{}", std::time::SystemTime::now()
    .duration_since(std::time::UNIX_EPOCH)
    .map(|d| d.as_nanos())
    .unwrap_or(0)));
  std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
  let path = dir.join(safe);
  std::fs::write(&path, bytes).map_err(|e| e.to_string())?;
  Ok(path.to_string_lossy().into_owned())
}

// The macOS menu bar. It's the default one, except that Quit asks the
// window to close rather than ending the app outright, so the window gets
// to ask about unsaved changes first.
#[cfg(target_os = "macos")]
fn mac_menu(app: &tauri::AppHandle) -> tauri::Result<tauri::menu::Menu<tauri::Wry>> {
  use tauri::menu::{Menu, MenuItem, PredefinedMenuItem, Submenu};
  let sep = || PredefinedMenuItem::separator(app);
  let quit = MenuItem::with_id(app, "quit", "Quit Wozzle", true, Some("CmdOrCtrl+Q"))?;
  let app_menu = Submenu::with_items(app, "Wozzle", true, &[
    &PredefinedMenuItem::about(app, None, None)?,
    &sep()?,
    &PredefinedMenuItem::services(app, None)?,
    &sep()?,
    &PredefinedMenuItem::hide(app, None)?,
    &PredefinedMenuItem::hide_others(app, None)?,
    &PredefinedMenuItem::show_all(app, None)?,
    &sep()?,
    &quit,
  ])?;
  // Without an Edit menu, Cmd-C and Cmd-V don't work in text fields
  let edit = Submenu::with_items(app, "Edit", true, &[
    &PredefinedMenuItem::undo(app, None)?,
    &PredefinedMenuItem::redo(app, None)?,
    &sep()?,
    &PredefinedMenuItem::cut(app, None)?,
    &PredefinedMenuItem::copy(app, None)?,
    &PredefinedMenuItem::paste(app, None)?,
    &PredefinedMenuItem::select_all(app, None)?,
  ])?;
  let window = Submenu::with_items(app, "Window", true, &[
    &PredefinedMenuItem::minimize(app, None)?,
    &PredefinedMenuItem::maximize(app, None)?,
    &sep()?,
    &PredefinedMenuItem::close_window(app, None)?,
  ])?;
  Menu::with_items(app, &[&app_menu, &edit, &window])
}

// A quit from the Dock, a logout, or AppleScript goes straight to
// [NSApp terminate:], which never asks the windows anything. tao's app
// delegate doesn't answer applicationShouldTerminate:, so we add that
// answer to it: while there are windows, close them (each asks about its
// unsaved changes) and cancel the terminate; the app ends when the last
// window goes, through tao's own exit (which is [NSApp stop:], not
// terminate:, so it doesn't come back through here).
#[cfg(target_os = "macos")]
mod mac_quit {
  use objc2::runtime::{AnyClass, AnyObject, Imp, Sel};
  use objc2::{ffi, sel};
  use std::sync::OnceLock;
  use tauri::Manager;

  static APP: OnceLock<tauri::AppHandle> = OnceLock::new();

  const NS_TERMINATE_CANCEL: usize = 0;
  const NS_TERMINATE_NOW: usize = 1;

  extern "C-unwind" fn should_terminate(_: &AnyObject, _: Sel, _: *mut AnyObject) -> usize {
    let Some(app) = APP.get() else { return NS_TERMINATE_NOW };
    let windows = app.webview_windows();
    if windows.is_empty() {
      return NS_TERMINATE_NOW;
    }
    log::info!("quit requested; closing {} window(s) first", windows.len());
    for window in windows.values() {
      let _ = window.close();
    }
    NS_TERMINATE_CANCEL
  }

  pub fn install(app: &tauri::AppHandle) {
    let _ = APP.set(app.clone());
    let Some(class) = AnyClass::get(c"TaoAppDelegateParent") else {
      log::warn!("no tao app delegate class; a Dock quit won't ask about unsaved changes");
      return;
    };
    unsafe {
      let imp: Imp = std::mem::transmute(
        should_terminate as extern "C-unwind" fn(&AnyObject, Sel, *mut AnyObject) -> usize,
      );
      // Returns NSApplicationTerminateReply (an NSUInteger), takes the app
      ffi::class_addMethod(
        class as *const AnyClass as *mut AnyClass,
        sel!(applicationShouldTerminate:),
        imp,
        c"Q@:@".as_ptr(),
      );
    }
  }
}

// The path for save_file comes percent-encoded in a header, since a
// header can only carry ASCII
fn percent_decode(s: &str) -> Option<String> {
  let b = s.as_bytes();
  let mut out = Vec::with_capacity(b.len());
  let mut i = 0;
  while i < b.len() {
    if b[i] == b'%' {
      let hex = s.get(i + 1..i + 3)?;
      out.push(u8::from_str_radix(hex, 16).ok()?);
      i += 3;
    } else {
      out.push(b[i]);
      i += 1;
    }
  }
  String::from_utf8(out).ok()
}

// Save an image to a path the user picked (in the save dialog, or the
// open one when saving back). We write it next to the real file and
// rename it over, so a save that fails partway leaves the old file as it
// was. Only paths the file scope already allows can be written, the same
// as the fs plugin's writeFile.
#[tauri::command]
fn save_file(webview: tauri::Webview, request: Request<'_>) -> Result<(), String> {
  let InvokeBody::Raw(bytes) = request.body() else {
    return Err("save_file wants raw bytes".into());
  };
  let path = request
    .headers()
    .get("x-wozzle-path")
    .and_then(|v| v.to_str().ok())
    .and_then(percent_decode)
    .ok_or("save_file needs a path")?;
  let path = PathBuf::from(path);
  if !webview.fs_scope().is_allowed(&path) {
    return Err(format!("not allowed to write {}", path.display()));
  }
  let mut tmp = path.clone().into_os_string();
  tmp.push(".wozsave");
  let tmp = PathBuf::from(tmp);
  if let Err(e) = std::fs::write(&tmp, bytes).and_then(|_| std::fs::rename(&tmp, &path)) {
    let _ = std::fs::remove_file(&tmp);
    return Err(e.to_string());
  }
  Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
  let app = tauri::Builder::default()
    // Picking image files, and reading and writing the ones picked
    .plugin(tauri_plugin_dialog::init())
    .plugin(tauri_plugin_fs::init())
    // Dragging files out of the window
    .plugin(tauri_plugin_drag::init())
    .invoke_handler(tauri::generate_handler![stage_drag_file, save_file])
    // A file dropped on the window may be read, just as one picked in the
    // open dialog may. Nothing else outside the app is reachable.
    .on_webview_event(|webview, event| {
      if let WebviewEvent::DragDrop(DragDropEvent::Drop { paths, .. }) = event {
        let scope = webview.fs_scope();
        for path in paths {
          if path.is_file() {
            let _ = scope.allow_file(path);
          }
        }
      }
    })
    .on_menu_event(|app, event| {
      if event.id() == "quit" {
        // Each window asks about its unsaved changes as it closes, and
        // the app ends when the last one goes
        for window in app.webview_windows().values() {
          let _ = window.close();
        }
      }
    })
    .setup(|app| {
      #[cfg(target_os = "macos")]
      {
        app.set_menu(mac_menu(app.handle())?)?;
        mac_quit::install(app.handle());
      }
      if cfg!(debug_assertions) {
        app.handle().plugin(
          tauri_plugin_log::Builder::default()
            .level(log::LevelFilter::Info)
            .build(),
        )?;
      }
      Ok(())
    })
    .build(tauri::generate_context!())
    .expect("error while building tauri application");

  app.run(|_app, event| {
    if let RunEvent::Exit = event {
      let _ = std::fs::remove_dir_all(drag_dir());
    }
  });
}
