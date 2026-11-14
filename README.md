# Prism Widgets

Lightweight desktop widgets for Windows 11. It's native C++ (Win32 + Direct2D), ships as one ~480 KB `.exe`, and needs no installer or runtime.

![Themes](res/themes_preview.png)

## Run it

1. Copy `PrismWidgets.exe` anywhere, for example `C:\Apps\PrismWidgets\`.
2. Double-click it. The **Settings** window opens, and your widgets appear on the desktop.
3. Close Settings when you're done. **The widgets keep running in the background**, with a tray icon.
4. To come back to Settings, do any of these:
   * Double-click the `.exe` again.
   * Left-click the tray icon.
   * Right-click a widget and choose **Widget settings…**

When Windows starts it for you (**Start with Windows**), it starts silently with only the widgets and no Settings window.

### Settings window

![Settings](res/settings_appearance.png)

| Page | What you can do |
|---|---|
| **Widgets** | Live previews of every widget, with a switch to show or hide each one. Click a widget to change its size (S/M/L/XL), set its own theme and options (clock style, seconds, 24-hour, week start, hide completed tasks…) or reset its position. |
| **Appearance** | Choose the global theme from previews rendered on your own wallpaper. Also set the accent colour, the text colour on widgets (auto/light/dark) and light or dark mode for the Settings window. |
| **General** | Start with Windows, lock widget positions, snap while dragging, reset positions, pause during fullscreen games and the data folder |
| **Custom widgets** | Create, edit, enable and reload JSON widgets, with a quick reference |
| **Performance** | Live memory use (the same figure as Task Manager), average CPU, the efficiency status and a *Free memory now* button |

### On the desktop

| Action | How |
|---|---|
| Move a widget | Drag it. It snaps to screen edges and other widgets. Hold **Alt** to stop snapping. |
| Quick options | Right-click a widget |
| Add a task | Click **Add a task** on the To-Do widget, type, then press Enter |
| Complete, edit or delete a task | Click the task to complete it. Right-click it to edit or delete. |
| Change the calendar month | Scroll the mouse wheel or click the arrows |
| Quit | Click *Quit Prism Widgets* in Settings, or use the tray menu → Exit |

Command-line flags: `--hidden` starts without opening Settings. `--startup` is used by Start with Windows.

## Themes

| Theme | Look |
|---|---|
| **Liquid Glass** (default) | Clear glass with lens refraction at the edges, light chromatic fringing and specular rim highlights |
| **Frosted Glass** | Classic glassmorphism: strong blur, white tint and a fine border |
| **Acrylic** | Windows-style dark acrylic with blur, tint and grain |
| **Tinted Glass** | Glass tinted with your accent colour |
| **Clear** | No panel. Text sits directly on the wallpaper with a soft shadow |
| **Solid Dark / Solid Light** | Nearly opaque panels |

The glass is rendered from your actual wallpaper, so the widgets blend into it. Prism Widgets reloads automatically when the wallpaper changes, including slideshow and Spotlight changes. With **Text colour → Automatic**, each widget picks light or dark text based on the wallpaper behind it.

## Why it uses so little RAM

* The app is native code with no browser engine, .NET or Electron, and it's statically linked.
* Rendering runs on the CPU only, through a Direct2D software target. This means no GPU driver or VRAM footprint.
* The wallpaper is decoded once into a small proxy. Each widget keeps only its own blurred crop, and the proxy is freed after 8 s of idle time.
* Frame buffers are freed right after they're handed to Windows (DWM).
* The Settings window and its previews exist only while the window is open. They are released when you close it.
* The app redraws only when needed: once a minute for the clock (once a second if seconds are shown) and every 2 s for system stats. Nothing runs between updates.
* It **pauses completely** during full-screen apps and games, when the screen is off and when the PC is locked.
* The process runs at below-normal priority with EcoQoS (efficiency mode) and low memory priority, and it trims its working set when idle.

The expected footprint is about **5–15 MB** in Task Manager with the default three widgets.

## Custom widgets

Custom widgets are JSON files in `%APPDATA%\PrismWidgets\widgets\`. To open that folder, use Tray → Widgets → *Open custom widgets folder*. Tray → Widgets → *Create new custom widget…* makes a template and opens it in Notepad. **Saving the file updates the widget instantly.** See `examples/` for four ready-made widgets: Greeting, System Bars, Launcher and Countdown.

```json
{
  "name": "My Widget",
  "width": 260, "height": 110,      // size in DIPs
  "refresh": 1000,                  // ms between redraws (1000 = every second, 0 = static)
  "theme": "frosted",               // optional default theme
  "elements": [
    { "type": "text", "x": 20, "y": 14, "text": "{time:HH:mm:ss}", "size": 34, "weight": "light" },
    { "type": "bar",  "x": 20, "y": 70, "w": 220, "h": 6, "value": "{cpu}", "max": 100, "color": "accent" },
    { "type": "button", "x": 20, "y": 80, "w": 40, "h": 40, "glyph": "E774", "click": "https://example.com" }
  ]
}
```

**Element types**

| Type | Properties |
|---|---|
| `text` | `x y w h text size weight font color align valign wrap` |
| `icon` | `x y w h glyph size color`. `glyph` is a Segoe Fluent Icons hex code point, such as `E713`. |
| `rect` | `x y w h radius fill stroke thickness` |
| `circle` | `x y r fill stroke thickness`. `x` and `y` are the centre. |
| `line` | `x y x2 y2 color thickness` |
| `bar` | `x y w h value max color track radius` |
| `ring` | `x y r thickness value max color track` |
| `button` | `x y w h radius text glyph size fill color click` |
| `image` | `x y w h path`. The path is absolute or relative to the widgets folder. |

* Any element can have `"click"`, which can be a URL, an app, a folder or a `ms-settings:` link.
* Colours are `fg`, `fg2`, `fg3`, `accent`, `onaccent`, `#RRGGBB` or `#RRGGBBAA`.
* Weights are `thin`, `light`, `normal`, `medium`, `semibold`, `bold`, `black` or a number.

**Variables** (usable in `text`, `value` and `click`):

| Variable | Value |
|---|---|
| `{time:HH:mm}` | Time in the given format. Tokens: `HH H hh h mm ss tt`. |
| `{date:dddd, d MMMM}` | Date in the given format. Tokens: `yyyy MMMM MMM MM dddd ddd dd d`. |
| `{greeting}` | Good morning, Good afternoon and so on |
| `{user}` | Your user name |
| `{cpu}` `{ram}` | Usage in % |
| `{ramused}` `{ramtotal}` | Memory in GB |
| `{battery}` `{charging}` | Battery level, and a symbol while charging |
| `{disk:C}` `{diskfree:C}` | Used % and free GB of a drive |
| `{uptime}` | Time since startup |
| `{countdown:2027-01-01}` | Days left until the date |
| `{todo}` | Open tasks |
| `{env:NAME}` | An environment variable |
| `{file:C:\path.txt}` | The first line of a text file |

## Where data is stored

`%APPDATA%\PrismWidgets\` contains `config.json` (widgets, positions and themes), `todo.json` and `widgets\`. To reset everything, delete this folder while the app is closed.

## Build from source

The tested build cross-compiles with MinGW-w64 on Linux or WSL:

```sh
sudo apt install g++-mingw-w64-x86-64-posix
./build.sh            # → build/PrismWidgets.exe
```

On Windows, `CMakeLists.txt` works with MSYS2 MinGW, and should work with Visual Studio, but that build is untested.

Source layout:

| File | Contents |
|---|---|
| `src/app.cpp` | Widget windows, tray, config, scheduling and pausing |
| `src/settings.cpp` | The Settings window, custom-drawn with Direct2D and freed when closed |
| `src/glass.cpp` | Wallpaper proxy and the software glass renderer (blur, refraction, rim light, shadow) |
| `src/widgets.cpp` | Clock, Calendar, To-Do, System Monitor, custom JSON widgets and samples |
| `src/util.cpp`, `src/json.cpp` | Helpers, variables and the JSON parser |

## Notes

* Widgets sit on the desktop layer, below every app window. They stay visible after **Win + D**. Desktop icons placed underneath a widget are covered by it.
* Windows 11 only. Icons and fonts use Segoe Fluent Icons and Segoe UI Variable.
* The `.exe` isn't code-signed. If SmartScreen warns, choose *More info → Run anyway*.
