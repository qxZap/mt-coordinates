# Coordinates

Saved spots, return spots and notes for Motor Town.

| | |
|---|---|
| Ctrl+F11 | save where you are as `spot<N>` |
| `#save <name>` in chat | save a spot with that name |
| `#return [name]` in chat | put you, or the vehicle you drive, back on that spot (no name: the last one saved) |
| `#note <text>` in chat | save the text with where you are |

Chat lines starting with `#` are handled by the plugin and never sent, so other players never see them.
Everything goes to `MotorTown\Binaries\Win64\plugins\coordinates.csv`
(`time, x, y, z, yaw, in, name, note`; cm and degrees, the game's world). A high beep means saved; a low one means it failed.

On someone else's server, the server may pull a character back after `#return`.

## Install

Needs [Plugin Loader](https://github.com/qxZap/mt-plugin-loader) (`version.dll`) in
`MotorTown\Binaries\Win64\`. Put `mt_plugin_Coordinates.dll` in `MotorTown\Binaries\Win64\plugins\`.
Settings: `plugins\coordinates.ini`, `[Coordinates] Hotkey=11` (Ctrl+F1..F12), `[General] Disabled=1` to turn it off.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_Coordinates.dll`.
