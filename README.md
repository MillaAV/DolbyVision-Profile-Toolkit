# DolbyVision Profile Toolkit

**Experimental Dolby Vision playback support for Windows media players**

DolbyVision Profile Toolkit is an independent, project designed to extend Dolby Vision playback capabilities in Windows applications using Microsoft Media Foundation, Movies&Tv..

The toolkit focuses on Dolby Vision profile detection, HEVC decoding integration, RPU metadata processing, experimental Profile 7 Enhancement Layer reconstruction, and GPU-assisted video processing.

**Current version:** `1.0-beta`  
**Platform:** Windows 11 (x64)  
**Status:** Experimental Public Beta 

> [!WARNING]
> This is beta software. Dolby Vision activation, player compatibility, and Profile 7 FEL reconstruction are still experimental. Successful profile detection does not necessarily guarantee reference-accurate playback.

---

## 1. Overview

Dolby Vision is a dynamic HDR technology that uses metadata and, depending on the profile, additional video information to optimize the final image.

Windows media players do not necessarily expose the same Dolby Vision capabilities for every video profile or container format.

DolbyVision Profile Toolkit attempts to extend these capabilities through runtime integration with compatible Windows media players.

The project provides:

- Dolby Vision profile recognition and playback integration.
- Experimental support for Dolby Vision Profile 7, including MEL and FEL.
- GPU-assisted processing using Direct3D 11.
- Integration with multiple Microsoft Media Foundation-based players.
- Automatic background attachment of the playback hook.
- Playback diagnostics and profile reporting.
- Optional HDR10 vs. Dolby Vision split-screen comparison.
- Installation, disabling, and uninstallation utilities.

The toolkit is not a video converter, an encoder, or a standalone media player.

---

## 2. Dolby Vision profiles

| Profile | Description | Beta status |
|---|---|---|
| **Profile 5 (P5)** | Single-layer Dolby Vision | Integration included |
| **Profile 7 MEL** | Base Layer + Minimal Enhancement Layer + RPU | Integration included |
| **Profile 7 FEL** | Base Layer + Full Enhancement Layer + RPU | Experimental GPU reconstruction |
| **Profile 8 (P8)** | HDR-compatible single-layer Dolby Vision | Integration included |
| **Profile 9 (P9)** | Dolby Vision profile based on an AVC video stream | Integration included |

### Profile 5

Profile 5 uses a single video layer with Dolby Vision metadata and requires appropriate Dolby Vision decoding and display processing.

The toolkit integrates with the available Windows Dolby Vision playback components where supported.

### Profile 7 MEL

Profile 7 MEL contains a Base Layer, Dolby Vision metadata, and a Minimal Enhancement Layer.

The toolkit detects Profile 7 streams and provides an experimental processing path for Profile 7 content.

### Profile 7 FEL

Profile 7 FEL is one of the project's primary experimental features.

A Profile 7 FEL stream contains three major components:

- **BL — Base Layer:** Primary HEVC video layer.
- **EL — Enhancement Layer:** Additional video data used for full enhancement reconstruction.
- **RPU — Reference Processing Unit metadata:** Dolby Vision metadata and mapping information.

The toolkit attempts to capture and synchronize these components, decode the Enhancement Layer, and apply GPU-assisted reconstruction.

The current implementation uses a post-Dolby processing approach.

### Profile 8

Profile 8 uses a single video layer with Dolby Vision metadata and may be based on HDR10, HLG, or other compatible base-layer configurations depending on the profile variant.

The toolkit works with existing HEVC and Dolby Vision playback components to support compatible Profile 8 content.

### Profile 9

Profile 9 uses an AVC-based video stream.

Profile 9 is included in the project's intended compatibility scope, but end-to-end support has not yet been fully validated.

---

## 3. Supported media players

| Media player | Integration |
|---|---|
| Microsoft Media Player (Windows 11) | Implemented |
| Movies & TV | Implemented |
| Microsoft Photos | Implemented |
| MPC-HC | In development |
| Energy Media Player | In development |
| PotPlayer | In development |

### Microsoft Media Player

The toolkit integrates with the Windows 11 Media Player application and its Media Foundation playback pipeline.

### Movies & TV

Movies & TV uses a related Windows video playback architecture.

The toolkit includes an automatic integration mechanism intended to load the hook into the player without requiring users to start a separate command every time they watch a video.

---

## 4. How it works

The toolkit interacts with several parts of the Windows video playback pipeline.

```text
Dolby Vision video
        |
        v
Windows Media Player / Movies & TV / Photos
        |
        v
Media Foundation
        |
        v
HEVC / Dolby Vision decoding
        |
        +---- Base Layer (BL)
        |
        +---- Enhancement Layer (EL, P7)
        |
        +---- RPU metadata
        |
        v
Experimental synchronization and processing
        |
        v
Direct3D 11 GPU processing
        |
        v
Video output
```

The implementation uses C++ and Windows Media Foundation APIs, with additional processing through Direct3D 11.

### Media Foundation integration

The runtime hook intercepts selected Media Foundation operations involved in decoder discovery, media type negotiation, and video processing.

This enables the toolkit to observe and adapt supported playback sessions.

### Enhancement Layer handling

For Profile 7 streams, the toolkit attempts to:

1. Identify the Enhancement Layer and RPU data.
2. Decode the Enhancement Layer using an available HEVC decoder.
3. Synchronize Base Layer, Enhancement Layer, and metadata using timestamps.
4. Process the matched frame data using the experimental reconstruction pipeline.
5. Submit the resulting frames to the video output path.

### GPU processing

The project uses Direct3D 11 compute shaders for its experimental enhancement processing.

The implementation includes GPU frame processing, timestamp-based pairing, and runtime diagnostics.

### Playback synchronization

Correct frame synchronization is essential for Profile 7 FEL.

The project includes logic for matching enhancement data with the corresponding Base Layer and for resetting processing state after seeking or decoder flush operations.

Some synchronization and initialization issues may remain in the beta.

---

## 5. System requirements

### Operating system

- Windows 11, 64-bit.
- Compatible Microsoft Media Foundation components.
- Supported HEVC decoding capabilities.

### Hardware

- GPU supporting the required Direct3D 11 compute features.
- An Dolby Vision-compatible display for actual Dolby Vision output.

### Software

Depending on the installed Windows configuration and target media player, the following components may be needed:

- Microsoft HEVC Video Extensions.
- Dolby Vision-related Windows extensions.
- Microsoft Media Player or another supported application.
- Microsoft Visual C++ build components where runtime injector compilation is required.

The installer attempts to detect and prepare required development components. Automatic dependency installation may require administrator privileges and internet access.

**A successful installation does not guarantee Dolby Vision support on every GPU, display, driver, or Windows configuration.**

---

## 6. Installation

### Step 1 — Download

Open the GitHub **Releases** section and download the latest archive.

Extract the archive to a local directory.

### Step 2 — Install

Run:

`INSTALL.cmd`

Approve the Windows administrator permission request.

The installer attempts to:

- Prepare the required files.
- Detect required build tools.
- Build the injector when necessary.
- Configure automatic player integration.
- Preserve compatible existing settings where possible.

### Step 3 — Play a video

Open a supported media player and start a compatible Dolby Vision video.

The toolkit is designed to attach automatically during playback.

You should not need to run a separate injector manually for every movie.

### Step 4 — Check playback

Run:

`CHECK-DOLBY-VISION.cmd`

The diagnostic report can identify available information such as:

- Active or previously detected playback sessions.
- Dolby Vision profile.
- Profile 7 MEL/FEL information, when available.
- Whether the playback hook was loaded.
- Dolby processing activity.
- Additional GPU or Enhancement Layer processing data when available.

The report is based on runtime logs. It cannot independently verify the final image displayed on your monitor.

---

## 7. HDR10 vs. Dolby Vision split-screen

The toolkit includes an optional split-screen comparison mode for supported Profile 7 FEL processing.

When active:

| Left half | Right half |
|---|---|
| HDR10 Base Layer processing | Dolby Vision processing with experimental FEL enhancement |

Enable the comparison:

`SPLIT-ON.cmd`

Disable the comparison:

`SPLIT-OFF.cmd`

This feature is intended for visual inspection and development. It is not a calibrated reference comparison, and the two output paths may differ in tone mapping, color handling, and processing accuracy.

---

## 8. Playback diagnostics

Use `CHECK-DOLBY-VISION.cmd` to inspect the most recent available playback information.

An example of the type of information it may display:

```text
Dolby Vision Playback Report

Player: Movies & TV
Hook: LOADED
Dolby transform hooks: READY
Detected profile: P8
Dolby transform processing: ACTIVE
```

For Profile 7 FEL, additional information may include:

```text
Detected profile: P7
Enhancement Layer: FEL
EL decoder: READY
GPU reconstruction: ACTIVE
```

### Understanding the results

**Hook loaded:** The toolkit DLL is present in the player process.

**Transform hooks ready:** The relevant Media Foundation interception points have been initialized.

**Profile detected:** Available metadata identifies a Dolby Vision profile.

**EL decoder ready:** An Enhancement Layer decoder has been initialized.

**GPU reconstruction active:** The GPU processing path was invoked for matched frame data.

**Dolby transform processing active:** The Dolby processing component has handled input/output operations.

A loaded hook does not necessarily mean Dolby Vision was activated on the display.

---

## 9. Available commands

| Command | Function |
|---|---|
| `INSTALL.cmd` | Install or update the toolkit |
| `CHECK-DOLBY-VISION.cmd` | View playback diagnostics |
| `SPLIT-ON.cmd` | Enable HDR10 / Dolby Vision comparison |
| `SPLIT-OFF.cmd` | Disable split-screen comparison |
| `DISABLE.cmd` | Disable the toolkit integration |
| `UNINSTALL.cmd` | Remove the toolkit and its installed components |

### Disabling the toolkit

`DISABLE.cmd` is intended to stop automatic integration without permanently removing the project files.

### Uninstalling

`UNINSTALL.cmd` is intended to remove the toolkit's installed components, background tasks, settings, logs, and project files.

Uninstallation is still being validated across different system configurations. Close supported media players before removing the toolkit to reduce file-locking problems.

The uninstall operation should not remove shared Microsoft components or unrelated Visual Studio installations.

---

## 10. Project structure

```text
DolbyVision-Profile-Toolkit/
├── bin/
├── scripts/
├── src/
├── third_party/
├── CHECK-DOLBY-VISION.cmd
├── DISABLE.cmd
├── INSTALL.cmd
├── SPLIT-OFF.cmd
├── SPLIT-ON.cmd
├── UNINSTALL.cmd
└── README.md
```

**bin/** — Binary components and build outputs.

**scripts/** — PowerShell scripts for installation, integration, diagnostics, and removal.

**src/** — C++ implementation of the hook, injector, and video processing components.

**third_party/** — Third-party source components and dependencies included with the project.

The actual file layout may change during development.

---

## 11. Known limitations

Dolby Vision initialization

Automatic player integration depends on the player startup sequence and the timing of Media Foundation initialization.

Some sessions may initialize before the toolkit has attached successfully.

### Hardware and display support

Dolby Vision availability depends on the operating system, drivers, display capabilities, available codecs, and installed Dolby components.

### Playback diagnostics

Runtime logs cannot fully validate displayed colors, brightness, panel-specific Dolby Vision processing, or the correctness of every output pixel.

### Installation and removal

Automatic dependency installation, background integration, and uninstallation require further testing across clean Windows installations.

---
## 12. Reporting bugs

Bug reports are welcome through GitHub Issues.

Please include:

- Windows version and build.
- Media player name and version.
- Dolby Vision profile (P5, P7 MEL, P7 FEL, P8, or P9).
- GPU and graphics driver version.
- A short description of the expected and actual behavior.
- Diagnostic logs when available.

For Profile 7 problems, please also mention whether the issue concerns profile detection, missing Enhancement Layer information, synchronization, GPU processing, or displayed image quality.

---
## 13. Development status

The project is under active experimental development.

Current priorities include improving automatic player initialization, validating profile-specific behavior, increasing FEL reconstruction reliability, and expanding verified compatibility.
<br>
<br>
<br>
## Why does a Command Prompt window appear when Windows starts?

> [!NOTE]
> **This is expected behavior and is not necessarily a sign of a problem.**

After installing DolbyVision Profile Toolkit, you may briefly see a Command Prompt or PowerShell window when starting or restarting Windows.

**Why does this happen?**

The toolkit uses a background startup component to integrate with supported Windows media players.

When you sign in to Windows, the startup component may launch through a scheduled task to prepare the Dolby Vision integration and monitor supported media player processes.

This allows the toolkit to automatically attach its playback components when a supported player starts, without requiring you to manually launch the injector every time you watch a movie.

**Can disable it?**

Run `DISABLE.cmd` to disable the toolkit's automatic integration, or `UNINSTALL.cmd` to remove the toolkit.

We are working toward a completely silent startup experience in future releases.
<br>
<br>
<br>
>Disclaimer
>
>DolbyVision Profile Toolkit is an independent, unofficial software project.
>
>It is not affiliated with, sponsored by, endorsed by, or certified by Dolby Laboratories or Microsoft Corporation.
>
>Dolby, Dolby Vision, and related marks are trademarks of Dolby Laboratories. Windows and Microsoft are trademarks of Microsoft Corporation.
