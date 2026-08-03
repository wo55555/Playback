# Playback Installation and Usage

[Back to README](../README.md) | [简体中文](getting-started.zh-CN.md)

## Before Installing

Playback is a client-only LeviLamina mod for Windows x64. Choose the release that matches the Minecraft and LeviLamina version used by your instance.

| Minecraft / LeviLamina | Playback release |
| --- | --- |
| `26.10.*` | [`v0.1.2-mc26.10`](https://github.com/wo55555/Playback/releases/tag/v0.1.2-mc26.10) |
| `26.20.*` | [`v0.1.1-mc26.20`](https://github.com/wo55555/Playback/releases/tag/v0.1.1-mc26.20) |

> [!IMPORTANT]
> Use a clean LeviLamina instance without other third-party mods whenever possible. Broad mod compatibility is not currently guaranteed.

> [!CAUTION]
> `v0.1.2-mc26.10` changes the replay snapshot format, so replays created by `v0.1.1` or earlier must be recorded again. It also removes the previous UI resource-pack installation layout. Delete any old `playback-ui.mcpack` file and `mods/playback/resource_packs/playback-ui/` directory before installing, then perform a clean mod installation.

## Install with LeviLauncher and Lip

The screenshots below use a `26.10` instance. For `26.20`, follow the same steps with the matching Minecraft, LeviLamina, and `v0.1.1-mc26.20` release.

1. Select **Download** in the left sidebar, find the Minecraft version you want, and use its install menu to create an instance with the **LeviLamina** loader.

<p align="center">
  <img src="images/quick-start/en/install-instance.webp" alt="Open Download and install a Minecraft instance with LeviLamina" width="900">
</p>

2. Select **Instances**, open the new instance's settings, and confirm under **Loader** that the matching LeviLamina version is installed.

<p align="center">
  <img src="images/quick-start/en/verify-levilamina.webp" alt="Open Instances and verify the installed LeviLamina loader" width="900">
</p>

3. Select **Launch** to return to the main page. Choose the target instance, then select **lip** under **Content Download**.

<p align="center">
  <img src="images/quick-start/en/open-lip.webp" alt="Open the Launch page and select lip under Content Download" width="900">
</p>

4. Search for **Playback**, then open the package published by `wo55555`.

<p align="center">
  <img src="images/quick-start/en/search-playback.webp" alt="Search for Playback in lip" width="900">
</p>

5. Manually choose the release whose **LL Requirement** and **Game Versions** match your instance, then select **Install** in that version's row. Lip does not select a Playback version based on the installed LeviLamina version.

<p align="center">
  <img src="images/quick-start/en/install-playback.webp" alt="Install a compatible Playback version with lip" width="900">
</p>

Launch or restart the game. The **Playback** button should appear on the Minecraft main menu.

## Install with the Lip CLI

Run the matching command from the root directory of the target LeviLamina instance:

```powershell
# Minecraft / LeviLamina 26.10
lip install github.com/wo55555/Playback@0.1.2-mc26.10#client

# Minecraft / LeviLamina 26.20
lip install github.com/wo55555/Playback@0.1.1-mc26.20#client
```

> [!NOTE]
> The `#client` variant is required. Always specify and verify the matching release; Lip does not choose one from the installed LeviLamina version.

## Manual Installation

Download `Playback-client-windows-x64.zip` from the matching release, then extract its `playback` directory into the LeviLamina instance's `mods` directory and restart the client.

## Record

Join a world, open the client command console, and use:

```text
record start
record pause
record stop
```

`record start` begins or resumes recording, `record pause` pauses capture, and `record stop` finishes and exports the replay. Exported replay archives are stored under Playback's `data/replays` directory.

## Replay

1. Return to the main menu and select **Playback**.
2. Choose a `.playback` or compatible `.zip` replay in the browser.
3. Wait for the isolated replay world and initial chunks to load.
4. Use the timeline to play, pause, seek, change speed, or jump to either end. Use **File > Exit Replay** to leave.

For development instructions, see [Building Playback](building.md). Release history is available in the [changelog](../CHANGELOG.md).
