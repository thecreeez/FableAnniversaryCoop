# AlbionTogether — Fable Anniversary Multiplayer Mod

Shared-campaign multiplayer mod for Fable Anniversary, forked from [AlbionTogether](https://github.com/kvoeten/AlbionTogether) v0.1.0-alpha.14.

Join the [AlbionTogether Discord](https://discord.gg/5JSKmjKd85) for playtests, updates, and support.

![Six Heroes together in Albion](media/six-player-alpha.png)

## Current alpha

One host can accept up to 64 guests (65 players total); the current alpha has been manually tested with six players. Players can load their own Heroes, explore Albion independently, and meet up anywhere. Remote Heroes keep their appearance, weapons, combat animations, emotes, and most Will spell effects.

This fork adds shared story progression, cutscenes, world interactions, and expanded NPC and combat synchronization.

### Shared campaign: completed

- Shared quest steps, counters, world flags, and quest screens.
- Shared cutscenes with player gathering, waiting/cancellation, world pause, and skipping by any player.
- Complete combat and NPC synchronization: dynamic NPC rosters, nearest-player simulation ownership, ranged aggro handoffs, actions, animations, damage, health, and death.
- Shared chest opening and rewards: consumables for everyone; weapons, armour, and gold for the opener.
- Shared demon-door opening and experience rewards: the killer receives 1x XP and other players receive 0.3x.
- Choosing **Stay** at a quest-area boundary preserves control of both Heroes.
- Synchronized time of day, weather, player and weapon effects, and destructible objects.
- Shared checkpoint recovery and weapon restoration after travelling.

### Work in progress and known issues

- **Theresa as a playable character:** clothing, equipment previews, body shaping, and cutscene presentation are still being refined.
- **New game and childhood:** starting a campaign together, child models, introductory scenes, and early saves remain WIP.
- **Shared-state persistence:** saving and loading shared chest/door states and experience remains WIP.
- **Quest map:** currently broken for the guest in the Heroes' Guild.
- Shared story morality, houses and shops, minigames, and individual Hero names/NPC name calls remain planned.

During alpha, expect some jank. Systems are being roughly implemented to make a full multiplayer playthrough possible. Once the full game can be played by two or more people, development will move into beta and focus on polishing the experience toward release.

### Done

- Load your own save character and see each other.
- Walk around Albion independently and meet in any region.
- Hero appearance, clothing, movement, rotation, and locomotion.
- Equipped weapons, including drawing and returning them to the Hero's back.
- Synchronized melee and ranged combat, including bow aiming, projectiles, damage, and hit reactions.
- Player death, resurrection phials, and Guild respawning.
- Synchronized Hero emotes and expression effects.
- NPC presence, position, movement, health, and death.
- NPC ownership when players split across different regions.
- Player health synchronization during combat.
- Connected-player health bars.
- Stable map transitions and background rendering.
- Reliable character, equipment, action, and NPC synchronization.
- Native launcher for hosting, joining, connection checks, and firewall setup.

### Next

- Equipment, loot, trading, currency, shops, and property ownership.
- Player names, interaction menus, door/key permissions, and RP tools.
- Proximity voice chat.
- Slow Time and Raise Dead synchronization (optional; may be omitted).

This is an early development release. Back up ordinary saves before testing.

Some antivirus tools may flag the launcher or client because AlbionTogether
injects the mod into the running game. This is expected to be a false positive
for official releases downloaded from this repository.

## Install and play

AlbionTogether currently targets the 32-bit Steam build of Fable Anniversary.

1. Download and extract the release into Fable Anniversary's `Binaries\Win32` folder.
2. Run `AlbionTogether.Launcher.exe`.
3. The host enters a display name and selects **Host Game**. Accept the firewall prompt if Windows shows one.
4. Other players enter a display name, the host's IP address, and the same port, then select **Join Game**.
5. Each player selects the save containing the Hero they want to use.

The default port is `38171`. The launcher's **Network** page can test the connection and repair the Windows Firewall rule.

![AlbionTogether launcher](media/launcher.png)

## Build

Build `AlbionTogether.sln` as `Release | Win32`. Deployable files are written to `bin\Release`.
Create an allowlisted release ZIP with `tools\Package-Release.ps1 -Version vX.Y.Z-alpha.N`.

The project also includes an embedded AngelScript runtime and native scripting APIs for future gameplay mods.

## License

AlbionTogether's original source code is available under the [MIT License](LICENSE).
Components under [`ThirdParty/`](ThirdParty/) retain their respective licenses.
The MIT License only covers code that this project has the right to license.

## Disclaimer

AlbionTogether is an unofficial fan project and is not affiliated with,
authorized, sponsored, or endorsed by Microsoft.

AlbionTogether requires a legally obtained Steam copy of Fable Anniversary
with modding support. It does not distribute Microsoft game files or provide
access to the game itself.

Fable, Fable Anniversary, and all related names, trademarks, characters,
imagery, and game content belong to Microsoft and/or their respective rights
holders. The AlbionTogether license applies only to this project's original
source code and does not grant rights to any Fable or third-party content.
