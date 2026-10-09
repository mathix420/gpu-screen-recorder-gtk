![](https://dec05eba.com/images/gpu_screen_recorder_logo_small.png)

# GPU Screen Recorder GTK (capture panel fork)
This is a fork of [GPU Screen Recorder GTK](https://git.dec05eba.com/gpu-screen-recorder-gtk/about/) that opens a capture panel in the style of the GNOME Shell screenshot UI instead of the settings window. It only records the screen (no screenshots).

* **Selection**: drag to select an area. Drag inside the selection to move it and drag its edges or corners to resize it.
* **Screen**: click on the monitor to record.
* **Window**: press record and pick the window (with the desktop portal on Wayland, by clicking on it on X11).
* **Pointer** (button on the right): show or hide the cursor in the video.
* **Settings** (gear button on the left): opens the original GPU Screen Recorder GTK window with all its options (codec, quality, audio, save location, streaming, replay, hotkeys). Closing it goes back to the panel. `gpu-screen-recorder-gtk settings` opens it directly.

Keyboard shortcuts in the panel: <kbd>Enter</kbd>/<kbd>Space</kbd> to record, <kbd>Esc</kbd> to close, <kbd>S</kbd>/<kbd>C</kbd>/<kbd>W</kbd> for selection/screen/window and <kbd>P</kbd> to toggle the pointer.

While recording a small indicator with the recording time and a stop button is shown at the top of the screen (on another monitor than the one being recorded, if there is one). Launching the application again also stops the recording, so a single hotkey can start and stop recordings, for example in sway:
```
bindsym Print exec flatpak run com.dec05eba.gpu_screen_recorder
```
Once the recording is stopped the video is saved (in `~/Videos` by default) and the application closes.

On Wayland compositors that support wlr-layer-shell (sway, Hyprland, KDE Plasma, etc) the panel is shown with [gtk-layer-shell](https://github.com/wmww/gtk-layer-shell), which is an optional build dependency. On GNOME and X11 fullscreen windows are used instead.

---

# GPU Screen Recorder GTK
GTK frontend for [GPU Screen Recorder](https://git.dec05eba.com/gpu-screen-recorder/about/).\
There is a new alternative UI for GPU Screen Recorder in the style of ShadowPlay available here: [GPU Screen Recorder UI](https://git.dec05eba.com/gpu-screen-recorder-ui/).

## Notes
The program has to be launched from your application launcher or hotkeys may not work properly in your Wayland compositor (this is the case with GNOME).

## Deprecation
This project is no longer being developed as it has been superseded by [GPU Screen Recorder UI](https://git.dec05eba.com/gpu-screen-recorder-ui/) which has more features. This means that this project will only receive bug fixes and no feature updates. This project will remain available until GPU Screen Recorder UI can run as a regular window, just like GPU Screen Recorder GTK does.\
The `com.dec05eba.gpu_screen_recorder.appdata.xml` file has been moved to the [AppData](https://git.dec05eba.com/gpu-screen-recorder-appdata/) repository.

# Installation
If you are using an Arch Linux based distro then you can find gpu screen recorder gtk on aur under the name gpu-screen-recorder-gtk (`yay -S gpu-screen-recorder-gtk`).\
If you are running another distro then you can run `sudo ./install.sh`, but you need to manually install the dependencies, as described below.\
You can also install gpu screen recorder from [flathub](https://flathub.org/apps/details/com.dec05eba.gpu_screen_recorder) which includes this UI.

# Dependencies
GPU Screen Recorder GTK uses meson build system so you need to install `meson` to build GPU Screen Recorder GTK.

## Build dependencies
These are the dependencies needed to build GPU Screen Recorder GTK:

* gtk3
* libx11
* ayatana-appindicator3-0.1
* desktop-file-utils
* gtk-layer-shell (optional, for the capture panel on Wayland compositors other than GNOME)

## Runtime dependencies
There are also additional dependencies needed at runtime:

* [GPU Screen Recorder](https://git.dec05eba.com/gpu-screen-recorder/)

# Reporting bugs, contributing patches, questions or donation
See [https://git.dec05eba.com/?p=about](https://git.dec05eba.com/?p=about).

# Screenshots
![](https://www.dec05eba.com/images/gpu-screen-recorder.png)
