# InsetFace
<img height="60px" src="https://github.com/xiangao0904/InsetFace/blob/master/plugin/icon_white.svg" /> <img height="60px" src="https://github.com/xiangao0904/InsetFace/blob/master/plugin/icon_black.svg" />
<br />

[![GitHub release (latest by date)](https://img.shields.io/github/v/release/xiangao0904/InsetFace?style=flat-square&color=0078d4)](https://github.com/xiangao0904/InsetFace/releases)
[![Website](https://img.shields.io/badge/Site-insetface.pages.dev-informational?style=flat-square)](https://insetface.pages.dev/)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-lightgrey?style=flat-square)](https://github.com/xiangao0904/InsetFace)

`InsetFace` is an independent plugin for **Metasequoia 4** that provides interactive face insetting functionality, complete with a visual installer.

---

## ✨ Features

- **Inset Modes:** Supports both `Region` (inset as a group) and `Individual` (inset each face separately).
- **Geometric Control:** Adjust `Thickness` (inset width) and `Depth` (height/extrusion along normals).
- **Offset Even:** Toggle to maintain uniform margins on irregular polygons.
- **Multilingual UI:** Supports Chinese, Japanese, and English.
- **Windows Installer:** Includes a dedicated installer for easy setup.
- **Interactive Workflow:** Supports real-time preview, drag-to-adjust in the viewport, apply, cancel, and undo.

---

## 📂 Repository Structure

- `plugin/`  
  Plugin source code, resources, and localization files.
- `installer/`  
  Source code and assets for the Windows installer.
- `vendor/mqsdk/`  
  Minimal dependencies extracted from the official Metasequoia SDK.
- `InsetFace.sln`  
  Visual Studio solution file.
- `InsetFace.vcxproj`  
  Plugin project file.
- `InsetFaceInstaller.vcxproj`  
  Installer project file.

---

## 🛠️ Build Environment

- **IDE:** Visual Studio 2022  
- **Platform:** `x64`  
- **Windows SDK:** Version matching your current VS installation.

### How to Build
1. Open `InsetFace.sln` in Visual Studio.
2. Select `x64` and either `Debug` or `Release` configuration.
3. Build the solution.

**Main Output Files:**
- **Plugin:** `build/x64/<Config>/InsetFace.dll`
- **Resources:** `build/x64/<Config>/InsetFace.resource.xml`
- **Installer:** `build/x64/<Config>/InsetFaceInstaller.exe`

---

## 🚀 Installation

### Recommended Method
Run the visual installer directly:
- **[InsetFaceInstaller.exe](https://github.com/xiangao0904/InsetFace/releases)**

The installer will automatically:
1. Copy `InsetFace.dll` and `InsetFace.resource.xml`.
2. Deploy light/dark theme icons.
3. **Modify Metasequoia layout files** to integrate the plugin.
4. Create an installation manifest for easy repair or uninstallation.

### Manual Method (For Debugging)
If you prefer manual deployment, copy the following files to your Metasequoia plugin directory:
- `InsetFace.dll`
- `InsetFace.resource.xml`
- `cmd_insetface.svg` (White/Black icons)

---

## 💡 Usage

1. Launch `InsetFace` from the Metasequoia tool panel.
2. Select one or more faces.
3. **Choose Mode:**
   - `Region`: Treats connected faces as a single unit.
   - `Individual`: Insets each face independently.
4. **Adjust Parameters:**
   - `Thickness`: Controls the inset margin.
   - `Depth`: Controls the elevation or indentation along the normal.
   - `Offset Even`: When enabled, prioritizes uniform edge distance.
5. **Preview & Interaction:** View results in real-time. You can also **drag directly in the viewport** to quickly adjust `Thickness`.
6. **Submit:** Click `Apply` to commit changes or `Cancel` to exit.

---

## 🌍 Localization

Plugin text is managed via `InsetFace.resource.xml`. Currently supported languages:
- **English**
- **Japanese** (日本語)
- **Chinese** (简体中文)

The plugin automatically detects and matches the language settings of your Metasequoia installation.
