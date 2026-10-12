# PS4Mount

Play PS4 fake PKGs on a jailbroken PS5 straight from a USB drive, without installing them.

Put the game (and its update) `.pkg` files on a USB drive, plug it in and load `ps4mount.elf`.
PS4Mount finds the packages, adds the games to the home screen and lets you play them from the
drive. Nothing is copied to the console storage, and deleting a game on the console only removes
it from the library: the `.pkg` stays on the drive.

![PS4Mount icon](assets/icon0.png)

## Screenshots

Notifications on the home screen while PS4Mount scans the USB drives:

![Scan notifications on the PS5 home screen](docs/notifications.png)

The web panel, opened from the **Media** tab:

![PS4Mount web panel](docs/web-panel.png)

## Features

- Detects PS4 fake PKGs (game and update) in the root of any connected USB drive.
- Adds the games to the home screen with their icon, name and update version.
- Applies updates: put the update `.pkg` next to the game and it is picked up automatically.
- Games run from the drive; no internal storage is used for the packages.
- Deleting a game from the PS5 removes it from the library only. The `.pkg` is never deleted.
- Notifications while scanning (how many games were found on each drive) and for added and removed
  games.
- A web panel, also available as **PS4Mount** in the **Media** tab, to see the games and add or
  remove them from the library.

## Requirements

- A jailbroken PS5 with fake PKG support (for example etaHEN with kstuff).
  Tested on firmware **13.20**. Other firmware versions are untested.
- An ELF loader listening on port **9021** to send the payload.
- Access to the kernel log: either **klogsrv** running on port 3232, or no other payload reading
  `/dev/klog`. PS4Mount reads the system log to know when a game starts and when a delete is
  confirmed. Without it, games cannot start (see [How it works](#how-it-works)).
- A USB drive formatted as **exFAT**.
- PS4 **fake** PKGs (game, and optionally its update). Retail PKGs and DLC are not supported.

## Usage

Download `ps4mount.elf` from the [Releases](https://github.com/gasb07/ps4mount/releases) page
(or [build it](#building)).

1. Copy the PS4 game `.pkg` (and its update `.pkg`, if you have one) to the **root** of the USB drive.
   The file names do not matter.
2. Plug the drive into the PS5.
3. Send `ps4mount.elf` to the ELF loader (port 9021). Do this after every console boot, or add it to
   the payloads your jailbreak loads automatically.
4. Wait for the notifications:
   - `Scanning usb0 for PS4 games...`
   - `usb0: 2 PS4 game(s) found, 2 new or updated`
   - `Adding 1/2: <game name> (CUSAxxxxx)`
   - `<game name> ready to play`
5. Start the game from the home screen.

On the first run PS4Mount moves every package it uses into a folder per game on the same drive:

```
/ps4/CUSAxxxxx/app.pkg      game
/ps4/CUSAxxxxx/patch.pkg    update (optional)
```

Keep this layout. To add more games later, copy their `.pkg` files to the root of the drive again.

### Web panel

Open **PS4Mount** in the **Media** tab, or browse to `http://<PS5 IP>:8770` from any device on
the same network. The panel shows every game found on the connected drives, with its state:

| State | Meaning |
| --- | --- |
| Ready | In the library and ready to play |
| Running | The game is open |
| Not in library | Removed from the library; the `.pkg` is still on the drive |
| Adding | Being added to the library |
| Installed on console | A normal installation of this game exists; PS4Mount leaves it alone |

Buttons:

- **Remove from library**: removes the game from the home screen. The `.pkg` stays on the drive.
- **Add to library**: adds a removed game back.
- **Scan for games**: checks the drives now and shows how many games were found on each one.
- **Log**: shows the PS4Mount log.
- **Stop**: stops PS4Mount. Its games cannot start until it is loaded again.

The panel has no password. Anyone on your local network can open it.

### Removing games

Deleting a game from the PS5 (Options > Delete), or using **Remove from library** in the panel,
removes the game from the library and leaves the `.pkg` on the drive. A removed game comes back
when you reconnect the drive or click **Add to library**.

To delete the game for good, delete its `/ps4/CUSAxxxxx` folder from the drive on a computer.

## Good to know

- Load PS4Mount soon after every boot. While it is not running, the console does not see the
  packages and may hide the games; they come back as soon as PS4Mount runs again.
- Do not unplug the drive while a game is running.
- When a game with an update is added, the PS5's own "installed" notification may show the base
  version (for example 01.00). The game runs with the update; the library shows the right version.
- Games already installed the normal way on the console are never touched.
- If a game fails to start right after being added, start it again. The log (panel > **Log**) shows
  what happened.

## How it works

A PS4 PKG install on the PS5 is mostly a byte copy of the package to `/user/app/<TITLE_ID>/app.pkg`
plus some metadata. PS4Mount skips the copy:

1. It mounts the game folder from the USB drive over `/user/app/<TITLE_ID>` (and over
   `/user/patch/<TITLE_ID>` when there is an update) with `nullfs`.
2. It writes the files an install would leave on the console, generated from the package itself:
   `app.json`, `app.pbm`, `app.xml`, `patch.*`, the PlayGo records (`/user/playgo/...`,
   `/system_data/playgo/<TITLE_ID>/pfs.pbm`), the app metadata, and the fake license (`.rif`/`.idx`)
   decrypted from the package.
3. It asks the system to register the title (`sceAppInstUtilAppInstallAll`), which adds it to the
   library and refreshes the home screen. When an update is present it also writes the library rows
   with the update's version.
4. The drive folder stays **read-only**, so a delete from the console cannot remove the package.
   It becomes writable only while a game is selected or running, because the system needs write
   access to start it. When a delete is confirmed, PS4Mount unmounts the drive folders first, so
   the delete only removes the library entry.

State and log are kept in `/data/ps4mount/`. The Media tab shortcut is `PMNT00001`
(`/user/app/PMNT00001`).

## Building

Requirements: the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) in `/opt/ps5-payload-sdk`
(or set `PS5_PAYLOAD_SDK`), `xxd` and bash.

```sh
./build.sh          # builds build/ps4mount.elf
```

Optional tools:

```sh
python3 tools/make_icon.py                                 # redraws assets/icon0.png (needs Pillow)
python3 tools/gen_keys.py <LibOrbisPkg>/LibOrbisPkg/Util/Keys.cs   # regenerates src/keys.h
```

### Releases

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request with the PS5
Payload SDK release set in `SDK_VERSION`, and keeps `ps4mount.elf` as a build artifact.

To publish a release:

1. Set the new version in `src/main.c` (`#define VERSION "PS4Mount X.Y.Z"`) and commit it.
2. Tag the commit and push the tag:
   ```sh
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

The workflow checks that the tag matches `VERSION`, builds the payload and creates the GitHub
release with `ps4mount.elf` and its SHA-256 checksum attached.

| Path | Content |
| --- | --- |
| `src/main.c` | Daemon: drive scan, registration, mounts, klog events, panel actions |
| `src/pkg.c` | PKG reader and fake-key entry decryption |
| `src/build.c` | Generators for the install files (app/patch metadata, PlayGo records, license) |
| `src/db.c` | Library rows (`app.db`, `appinfo.db`) |
| `src/crypto.c` | SHA-256, AES-128-CBC, CRC32C, RSA-2048 |
| `src/web.c`, `web/index.html` | Web panel |
| `tools/pdb_*_template.bin` | PlayGo record layouts (no title data) |
| `third_party/sqlite/` | SQLite amalgamation |

## Credits

PS4Mount does not contain code copied from other projects except where noted below.

| Project | Author | License | Use |
| --- | --- | --- | --- |
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) | John Törnblom | GPL-3.0-or-later | Toolchain, headers and runtime linked into the payload |
| [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) | maxton | LGPL-3.0 | Source of the fake PKG key (`PkgDerivedKey3Keyset`, in `src/keys.h`) and reference for the PKG entry encryption, entry IDs and the `playgo-chunk.dat` layout |
| [SQLite](https://sqlite.org) | SQLite authors | Public domain | `third_party/sqlite/` (amalgamation 3.46.1), used to write the library databases |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | drakmor | GPL-3.0 | Inspiration for auto-mounting games from USB drives and for registering titles with `sceAppInstUtilAppInstallAll` on firmware 12.00+. No code was used. |
| [ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper) | EchoStretch | see project | Reference for the Media tab shortcut (`applicationCategoryType` 65536 with `deeplinkUri`). No code was used. |

## License

PS4Mount is licensed under the GNU General Public License v3.0 or later, see [LICENSE](LICENSE).
It links the PS5 Payload SDK runtime (GPL-3.0-or-later). `src/keys.h` carries data published by
LibOrbisPkg under the LGPL-3.0. SQLite is in the public domain.

This project is not affiliated with or endorsed by Sony Interactive Entertainment. Only use it with
games you own.
