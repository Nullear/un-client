# Un Client (Godot C++)

Independent Godot 4.4.1 GDExtension migration of `D:\Un Falsus Online`.
The original Cocos project is the behavioral reference and is not modified
by this project. All scene logic currently in use is C++ (`native/client`);
there is no C# or GDScript runtime.

## Preview

Open `D:\Un Client\project.godot` in Godot 4.4.1, wait for resource imports,
then run `Godot/Scenes/Startup.tscn`, `MainMenu.tscn`, or `Konzetsu.tscn`.
Restart any open Godot editor after replacing the native DLLs: Windows locks
Godot's hot-reload copies. The project's debug/release Windows DLLs are in
`native/client/bin`. GoZen's corresponding libraries are in
`addons/gde_gozen/bin`.

The C++ scene classes implement the Startup video and white flash, MainMenu,
Konzetsu loading sequence and hub, plus a persistent triangle-grid scene
transition. Startup -> MainMenu -> Konzetsu -> MainMenu is the intended
offline-preview navigation. Cocos originally gates Startup on online content
validation; this preview still defaults to an offline title gate. No extra
Startup prompt has been added.

## Content And Video

The original MP4s are in `assets/resources/video/startup`. The C++ video node
copies packed MP4 data to `user://` before opening GoZen's native `GoZenVideo`
resource on Windows/Android. The iOS path calls the existing `UnFalsusVideo`
AVPlayer plugin. Title and menu BGM use the original Ogg files.

Downloaded content is read only from `user://assets`. `cb/` is the old Cocos
hot-update folder and is not a runtime source.

## Building

Use `native/client/SConstruct` with godot-cpp matching Godot 4.4.1:

```
scons -j4 platform=windows target=template_debug arch=x86_64 godot_cpp=/path/to/godot-cpp
scons -j4 platform=windows target=template_release arch=x86_64 godot_cpp=/path/to/godot-cpp
ANDROID_NDK_ROOT=/path/to/linux/ndk scons -j4 platform=android target=template_debug arch=arm64 godot_cpp=/path/to/godot-cpp
ANDROID_NDK_ROOT=/path/to/linux/ndk scons -j4 platform=android target=template_release arch=arm64 godot_cpp=/path/to/godot-cpp
```

The prebuilt Windows and Android arm64 debug/release binaries in this checkout
were compiled using godot-cpp at `b0e3b1e4b78a606f48d162898afb5eeda533d2a9`.
The source GDE GoZen revision is
`f9448619324ad7d0d4e79d3bd501bde477ea4b7f`. The native binaries are
gitignored and must be copied or rebuilt for a fresh checkout.

GoZen upstream has no iOS target. On macOS/Xcode, build the iOS video plugin
with `GODOT_SOURCE=/path/to/godot bash ios/build-video-plugin.sh` and build the
client GDExtension for iOS before exporting. No iOS native client library has
been produced here. Godot 4.4.1 export templates are not installed on this
machine, so no Android/iOS package or on-device test is claimed.

## Gaps

This is a visual/offline migration milestone, not full feature parity. Login,
companion-package validation, server updater, download dialogs, real song
selection, and server-confirmed character changes are still unported. The
character dialog previews local content only. MainMenu and the character
dialog need further comparison with the Cocos project for full layout,
shading, and responsive behavior. iOS playback and native transition coverage
over AVPlayer have not been tested on a Mac/device.
