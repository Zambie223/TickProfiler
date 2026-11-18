# Tick Profiler

A diagnostic mod for Satisfactory that records, in detail, where a server's (or game's) frame time goes, so a low tick rate can be traced to its cause before anything is changed. It only measures: it changes nothing in the game and adds no network traffic of its own (apart from the chat replies to `/perf`).

**Status: early release (0.1.0).** Tested on game version 1.2.5.0 (build 517554) with SML 3.12: Windows and Linux dedicated servers, players joining with and without the mod, and single player. Game update 1.2.5.1 (build 520140) is not tested yet, and needs an SML release that supports it.

## Why a mod

The dedicated server ships as a Shipping build, and Unreal's own profilers (Unreal Insights tracing, `stat` commands, the CSV profiler) are compiled out of it. Tick Profiler times the frame itself, by hooking the engine and game functions that make up a frame.

## What it records

| Level | Records | Cost |
|---|---|---|
| 0 | nothing (paused) | none |
| 1 basic | time per frame stage: each tick group, timers, tickable objects, latent actions, level streaming, network receive/send/replication, factory tick, garbage collection, saving, the engine's core ticker (HTTP and the server API), and the frame work no hook sees ("untimed") | negligible |
| 2 detailed (default) | + time and call counts per class of actor tick, component tick and actor replication (with bits sent), per-connection network stats, periodic counts of actors by class | about two timestamps per ticking object; the mod reports its own cost |
| 3 extreme | + the same per individual object, with its location (finds one broken machine or stuck vehicle) | higher; for short captures |

At every level it also records:
- **Tick rate:** actual vs. target, and frame work time with the tick-rate sleep excluded.
- **Hitches:** frames well above normal, with the phases and classes that made them slow.
- **Host metrics, once per second:** process CPU, system CPU and **steal** (an oversold host), **cgroup CPU throttling** and memory (container limits), RSS, swap, page faults, load.
- **Once per session:** cores seen by the engine vs. the container's CPU quota, net and tick-rate settings, and the loaded mods.

Measured cost, on a late-game save on a Windows dedicated server at 30 Hz with about 21 ms of frame work: level 2 adds about 0.6 ms per frame (3%) and level 3 about 2 ms (10%) on the game thread, which is what limits the tick rate. Whole-process CPU, with the mod installed vs. not installed, differed by less than the run-to-run noise (about 2%). Your numbers will depend on your save.

On a dedicated server the engine runs every actor and component tick on the game thread, so the per-class times add up directly to the frame time.

Classes are tagged with their origin (`FactoryGame`/`Engine` native code, `Game` base-game Blueprints, or a mod reference), so a cost from another mod shows up as that mod.

## Where the data goes

- **Data files** in `FactoryGame/Saved/Logs/TickProfiler/<UTC start>_<server|game>/`:
  - `frames.NNN.csv`: one row per second.
  - `classes.NNN.csv`: top classes per 10 s.
  - `objects.NNN.csv`: level 3 only.
  - `hitches.NNN.jsonl`, `host.NNN.csv`, `net.NNN.csv`, `snapshots.NNN.jsonl`, `marks.NNN.csv`.
  - `session.json`: the session's settings and environment.

  The files are written by a background thread, flushed every second, split into parts, and capped at 256 MB in total by default (oldest sessions deleted first).
- **FactoryGame.log** (`LogTickProfiler`): a summary every 60 s and one line per hitch (at most 20 per minute).
- **Chat:** `/perf [summary [N] | hitches [N] | net | status | settings | level <0-3> [minutes] | set <setting> <value> | mark <text> | snapshot]`. On a dedicated server only administrators can use it; in single player only the host can. Players don't need the mod to use it: chat commands run on the server.
- **Console** (the Server Manager console, the HTTPS API's `RunCommand`, or the game console): `TickProfiler.Level`, `.Set`, `.Settings`, `.Summary`, `.Status`, `.Hitches`, `.Net`, `.Mark`, `.Snapshot`.

`/perf mark players report lag` or `TickProfiler.Mark ...` drops a bookmark into every output, so a report from players can be matched to the data.

## Reading the data

`Tools/analyze.py <session folder>` (in this repository, not in the zips; Python 3, standard library only) writes a report covering:
- the tick rate over time and the worst minutes,
- where the frame time goes,
- the top classes and which mods they come from,
- hitch causes,
- host starvation flags,
- per-player network load.

## Installation

Tick Profiler is not on the Satisfactory Mod Repository (so not in Satisfactory Mod Manager); install it by hand from the [releases page](https://github.com/Zambie223/TickProfiler/releases). It needs SML 3.12 or newer installed.

| Where it runs | Download | Extract into |
|---|---|---|
| Linux dedicated server (most rented hosts) | `TickProfiler-LinuxServer.zip` | `FactoryGame/Mods/TickProfiler/` in the server's install folder |
| Windows dedicated server | `TickProfiler-WindowsServer.zip` | `FactoryGame\Mods\TickProfiler\` in the server's install folder |
| The game (single player, or your own client) | `TickProfiler-Windows.zip` | `FactoryGame\Mods\TickProfiler\` in the game's install folder (Steam or Epic) |

The folder must end up holding `TickProfiler.uplugin` directly. Restart the server or the game afterwards. On a rented host, upload the extracted folder with the host's file manager or FTP.

**Players do not need Tick Profiler.**

They do still need **SML** if they don't already have it. A server running SML refuses clients without SML, unless the server sets `SML.SkipRemoteModListCheck 1`, which SML itself does not guarantee to be safe. If your server already runs mods, nothing changes for your players.

The defaults suit a first look, so after installing you only need to let the server run while it is slow, then collect `Saved/Logs/TickProfiler` (and `FactoryGame.log`).

## Settings

You don't need to edit any file to change a setting. Use whichever of these suits your setup:

1. **From chat, as a server admin:** `/perf settings` lists every setting with its value; `/perf set <setting> <value>` changes one at once and saves it (e.g. `/perf set HitchMs 100`). `/perf level <0-3>` is short for `/perf set Level`.
2. **For a while only:** `/perf level 3 10` records at level 3 for 10 minutes, then goes back to the saved level. Nothing is saved. Level 3 costs more, so use it this way while the lag is happening.
3. **In the game's Mods menu** (Mod Options, Tick Profiler): in single player, and for the copy on your own PC if you install it there too. It applies only to the game you set it in, not to a server you join.
4. **The settings file** `FactoryGame/Configs/TickProfiler.cfg` (JSON). It is created at the first start. On a rented server, edit it through the host's file manager **while the server is stopped**, because the server writes the file when settings change.
5. **Launch parameters**, if your host allows them. They win over the file at every start:
   `-TickProfilerLevel=N`, `-TickProfilerHitchMs=N`, `-TickProfilerSummarySec=N`, `-TickProfilerClassSec=N`, `-TickProfilerSnapshotSec=N`, `-TickProfilerTopN=N`, `-TickProfilerHitchLines=N`, `-TickProfilerDiskCapMB=N`, `-TickProfilerNoFiles`.

| Setting | Default | Meaning |
|---|---|---|
| `Level` | 2 | 0 paused, 1 basic, 2 detailed, 3 extreme |
| `HitchMs` | 0 | frames slower than this (ms) are hitches; 0 = automatic (2x the target frame time, at least 50 ms) |
| `SummaryIntervalSec` | 60 | seconds between summaries in FactoryGame.log |
| `ClassIntervalSec` | 10 | seconds per set of rows in classes.csv |
| `SnapshotIntervalSec` | 300 | seconds between object counts |
| `TopN` | 25 | classes per table |
| `MaxHitchLinesPerMinute` | 20 | hitch lines per minute in FactoryGame.log (hitches.jsonl gets all of them) |
| `DiskCapMB` | 256 | total size of `Logs/TickProfiler`; oldest data deleted first. Applies at the next start |
| `WriteFiles` | true | write the data files (log summaries are written either way). Applies at the next start |

Every change is also written to `marks.csv`, so the data shows when a setting changed.

For troubleshooting, the launch parameter `-TickProfilerSkipHooks=Name,...` turns off a single measurement if it ever causes a problem. Names: WorldTick, TickGroups, ActorTick, ComponentTick, FactoryTick, Timers, Tickables, Latent, Streaming, EndOfFrame, GC, Save, CoreTicker, ReplicateActor, Net, or All. `TickProfiler.Status` shows how often each stage has run since start.

## Privacy

The data files and log contain player names (in the network data) and object names and locations from your world. Look through them before sharing them publicly.

## Responsible AI use disclosure

Most of the code, tooling and documentation in this mod were written by an AI coding assistant (Claude, made by Anthropic, used through Claude Code), working under the maintainer's direction. The maintainer set the requirements and design decisions. Not every line has been reviewed by a person. AI-written code can contain mistakes: review it before relying on it, and back up your saves.
