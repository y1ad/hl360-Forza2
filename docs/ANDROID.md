# Forza Motorsport 2 on Android (arm64)

Work in progress; nothing here has run on a phone yet.

## Install

The `Android (arm64)` workflow builds `forza2-android-arm64.apk` and publishes it
as the `android-latest` pre-release. Install it from the phone (allow installs from
your browser). It needs Android 13+, an ARMv8.2 CPU and Vulkan 1.1.

## Game files

No game files are included. Extract your own *Forza Motorsport 2* disc (the
European or American release) so that `default.xex` sits in:

```
/sdcard/Forza2/game/default.xex      (after you allow all-files access)
Android/data/io.github.y1ad.forza2/files/game/   (before that)
```

The app asks for all-files access on its first start. Allow it, then start the
app again.

## Settings

The app copies `forza2.toml` and `forza2.android.toml` next to the game folder
on every start. Put your own choices in `forza2.user.toml` in the same folder; it
loads last. Logs are in `logs/forza2.log`, saves and caches in `state/`.

## Controls

A Bluetooth or USB gamepad. There is no touch overlay yet.

## Not ported

The launcher, System Link and online play, and the scripted-input automation are
Windows only.
