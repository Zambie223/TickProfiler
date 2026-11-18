#!/usr/bin/env python3
"""Turns a Tick Profiler session folder into a readable report.

Usage:
  python analyze.py <session folder>            e.g. .../Saved/Logs/TickProfiler/2026-11-18_12-00-00_server
  python analyze.py <TickProfiler folder>       picks the newest session in it
  python analyze.py <folder> --out report.md    also writes the report to a file

Only the Python standard library is used. The report is a starting point for diagnosis, not a verdict:
it points at where the frame time goes and flags host-side starvation, so a fix can be chosen on evidence.
"""
import csv
import glob
import json
import os
import statistics
import sys
from collections import Counter, defaultdict


def parts(folder, base, ext):
    return sorted(glob.glob(os.path.join(folder, f"{base}.*.{ext}")))


def read_csv(folder, base):
    rows = []
    for path in parts(folder, base, "csv"):
        with open(path, newline="", encoding="utf-8", errors="replace") as handle:
            rows.extend(csv.DictReader(handle))
    return rows


def read_jsonl(folder, base):
    items = []
    for path in parts(folder, base, "jsonl"):
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    try:
                        items.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass  # a line cut off by a crash
    return items


def num(row, key, default=0.0):
    try:
        return float(row.get(key, default))
    except (TypeError, ValueError):
        return default


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


def find_session(path):
    if os.path.exists(os.path.join(path, "session.json")) or parts(path, "frames", "csv"):
        return path
    sessions = sorted(d for d in glob.glob(os.path.join(path, "*")) if os.path.isdir(d))
    if not sessions:
        sys.exit(f"No Tick Profiler session found in {path}")
    return sessions[-1]


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    out_path = None
    if "--out" in args:
        index = args.index("--out")
        out_path = args[index + 1]
        del args[index:index + 2]
    folder = find_session(args[0])
    lines = []
    say = lines.append

    session = {}
    session_path = os.path.join(folder, "session.json")
    if os.path.exists(session_path):
        with open(session_path, encoding="utf-8") as handle:
            session = json.load(handle)
    frames = read_csv(folder, "frames")
    classes = read_csv(folder, "classes")
    hitches = read_jsonl(folder, "hitches")
    host = read_csv(folder, "host")
    net = read_csv(folder, "net")
    snapshots = read_jsonl(folder, "snapshots")
    marks = read_csv(folder, "marks")

    say(f"# Tick Profiler report: {os.path.basename(os.path.normpath(folder))}")
    say("")
    host_info = session.get("host", {})
    target = session.get("net_server_max_tick_rate") or 0
    say("## Session")
    say(f"- Role: {session.get('role', '?')}, platform {session.get('platform', '?')}, map {session.get('map', '?')}, mod {session.get('mod_version', '?')}")
    say(f"- Target tick rate (NetServerMaxTickRate): {target or 'n/a (not a dedicated server)'}; MaxClientRate {session.get('max_client_rate', '?')}, MaxInternetClientRate {session.get('max_internet_client_rate', '?')}")
    say(f"- CPU: {host_info.get('cpu_brand', '?')}; cores physical/logical {host_info.get('cores_physical', '?')}/{host_info.get('cores_logical', '?')}, "
        f"allowed by affinity {host_info.get('cpus_allowed_by_affinity', '?')}, cgroup CPU quota {host_info.get('cgroup_cpu_quota_cores', 'n/a')} cores, "
        f"task-graph workers {host_info.get('taskgraph_worker_threads', '?')}")
    say(f"- Memory: {host_info.get('total_physical_mb', 0):.0f} MB total, cgroup limit {host_info.get('cgroup_mem_limit_mb', 'n/a')} MB")
    if session.get("mods"):
        say(f"- Mods: {', '.join(session['mods'])}")
    if session.get("ticks_on_game_thread_only"):
        say("- Actor and component ticks all run on the game thread here (the engine does this on every dedicated server), "
            "so their time adds directly to the frame; per-core speed matters more than core count for them.")
    quota = host_info.get("cgroup_cpu_quota_cores") or 0
    logical = host_info.get("cores_logical") or 0
    if quota and logical and quota < logical:
        say(f"- **Note:** the container may use {quota:g} cores but the engine sees {logical}; it sizes its worker pool for {logical}, "
            "so parallel work (factory tick, physics) competes for a much smaller CPU budget.")
    say("")

    if frames:
        seconds = len(frames)
        hz = [num(r, "tick_hz") for r in frames]
        work = [num(r, "work_avg_ms") for r in frames]
        say("## Tick rate")
        say(f"- {seconds} seconds recorded. Average {statistics.mean(hz):.1f} Hz, median {statistics.median(hz):.1f} Hz, worst second {min(hz):.1f} Hz.")
        if target:
            for share in (0.9, 0.5, 0.25):
                below = sum(1 for h in hz if h < target * share)
                say(f"- Below {share:.0%} of target ({target * share:.1f} Hz): {pct(below, seconds):.1f}% of seconds")
        say(f"- Frame work (excluding the tick-rate sleep): average {statistics.mean(work):.1f} ms; per-second p95 {statistics.quantiles(work, n=20)[-1] if len(work) >= 20 else max(work):.1f} ms; worst frame {max(num(r, 'work_max_ms') for r in frames):.1f} ms")
        idle = statistics.mean(num(r, "idle_avg_ms") for r in frames)
        say(f"- Idle (sleeping to hold the tick rate): {idle:.1f} ms/frame. Near zero means the server never catches up; the work itself is the limit.")
        say("")

        say("## Where the frame time goes (average ms per frame over the session)")
        phase_cols = [c[:-7] for c in frames[0].keys() if c.endswith("_avg_ms") and c not in ("period_avg_ms", "work_avg_ms", "idle_avg_ms")]
        totals = {c: statistics.mean(num(r, c + "_avg_ms") for r in frames) for c in phase_cols}
        worst = {c: max(num(r, c + "_max_ms") for r in frames) for c in phase_cols}
        avg_work = statistics.mean(work) or 1.0
        say("| phase | avg ms/frame | share of work | worst frame ms |")
        say("|---|---:|---:|---:|")
        for column, value in sorted(totals.items(), key=lambda kv: -kv[1]):
            if value >= 0.01 or worst[column] >= 1.0:
                say(f"| {column} | {value:.2f} | {pct(value, avg_work):.0f}% | {worst[column]:.1f} |")
        say("")
        say("Nesting: `world` contains the tick groups (`tg_*`), timers, tickables, latent actions, streaming, net send/receive and GC. "
            "`factory` and the actor/component ticks run inside a tick group. `net_replicate` runs inside `net_send`. Shares therefore overlap; compare siblings. "
            "`core_ticker` (HTTP, the server API, online services) and `untimed` (engine-loop work no hook sees, including map loads) are outside `world`; "
            "`world` + `core_ticker` + `untimed` = all frame work.")
        actor = statistics.mean(num(r, "actor_gt_ms") for r in frames)
        actor_w = statistics.mean(num(r, "actor_worker_ms") for r in frames)
        comp = statistics.mean(num(r, "comp_gt_ms") for r in frames)
        comp_w = statistics.mean(num(r, "comp_worker_ms") for r in frames)
        say(f"- Actor ticks: {actor:.2f} ms/frame on the game thread, {actor_w:.2f} on workers; component ticks {comp:.2f} / {comp_w:.2f}.")
        say(f"- Replication: {statistics.mean(num(r, 'repl_ms') for r in frames):.2f} ms/frame, {statistics.mean(num(r, 'repl_actors') for r in frames):.0f} actor updates/s, "
            f"{statistics.mean(num(r, 'repl_kbits') for r in frames):.0f} kbit/s.")
        gcs = sum(num(r, "gc_count") for r in frames)
        say(f"- Garbage collections: {gcs:.0f}; players (max) {max(num(r, 'players') for r in frames):.0f}.")
        say("")

        say("## Worst minutes")
        minutes = defaultdict(list)
        for row in frames:
            minutes[int(num(row, "uptime_s") // 60)].append(row)
        ranked = sorted(minutes.items(), key=lambda kv: statistics.mean(num(r, "tick_hz") for r in kv[1]))[:5]
        for minute, rows in ranked:
            biggest = max(phase_cols, key=lambda c: statistics.mean(num(r, c + "_avg_ms") for r in rows) if c != "world" else -1)
            say(f"- minute {minute} ({rows[0].get('time_utc', '?')[:19]}Z): {statistics.mean(num(r, 'tick_hz') for r in rows):.1f} Hz, "
                f"work {statistics.mean(num(r, 'work_avg_ms') for r in rows):.1f} ms, biggest phase {biggest}, players {max(num(r, 'players') for r in rows):.0f}")
        say("")

    if classes:
        say("## Top classes over the session (average ms per second)")
        acc = defaultdict(lambda: [0.0, 0.0, 0.0, 0.0, 0.0])
        total_window = defaultdict(float)
        for row in classes:
            key = (row["kind"], row["class"], row["origin"])
            window = num(row, "window_s")
            acc[key][0] += num(row, "total_ms_per_s") * window
            acc[key][1] += num(row, "gt_ms_per_s") * window
            acc[key][2] += num(row, "calls_per_s") * window
            acc[key][3] = max(acc[key][3], num(row, "max_frame_ms"))
            acc[key][4] += num(row, "kbits_per_s") * window
        windows = {}
        for row in classes:
            windows[(row["uptime_s"], row["kind"])] = num(row, "window_s")
        for (uptime, kind), window in windows.items():
            total_window[kind] += window
        say("| # | kind | class | origin | ms/s | of which game thread | calls/s | worst frame ms |")
        say("|---:|---|---|---|---:|---:|---:|---:|")
        # Rank by the same per-second value that is printed (each kind is averaged over the windows it appears in).
        ranked = sorted(acc.items(), key=lambda kv: -kv[1][0] / (total_window[kv[0][0]] or 1.0))[:30]
        for rank, ((kind, name, origin), values) in enumerate(ranked, 1):
            span = total_window[kind] or 1.0
            say(f"| {rank} | {kind} | {name} | {origin} | {values[0] / span:.2f} | {values[1] / span:.2f} | {values[2] / span:.0f} | {values[3]:.1f} |")
        by_origin = Counter()
        for (kind, name, origin), values in acc.items():
            by_origin[origin] += values[0] / (total_window[kind] or 1.0)
        say("")
        say("By origin (FactoryGame/Engine = native code, Game = base-game Blueprints, anything else = that mod): "
            + ", ".join(f"{origin} {value:.1f} ms/s" for origin, value in by_origin.most_common(10)))
        say("")

    if hitches:
        say(f"## Hitches ({len(hitches)})")
        dominant = Counter()
        top_classes = Counter()
        for hitch in hitches:
            phases = {k: v for k, v in hitch.get("phases", {}).items() if k != "world"}
            if hitch.get("untimed_ms"):
                phases["untimed"] = hitch["untimed_ms"]
            if phases:
                dominant[max(phases, key=phases.get)] += 1
            for entry in hitch.get("top", [])[:3]:
                top_classes[f"{entry['class']} ({entry['kind']})"] += 1
        say("- Largest phase in each hitch: " + ", ".join(f"{k} x{v}" for k, v in dominant.most_common(8)))
        say("- Classes most often among a hitch's top 3: " + ", ".join(f"{k} x{v}" for k, v in top_classes.most_common(8)))
        say(f"- Hitches with a garbage collection: {sum(1 for h in hitches if h.get('gc_count'))}; with a save: {sum(1 for h in hitches if h.get('phases', {}).get('save'))}")
        worst = sorted(hitches, key=lambda h: -h.get("work_ms", 0))[:5]
        for hitch in worst:
            items = dict(hitch.get("phases", {}))
            if hitch.get("untimed_ms"):
                items["untimed"] = hitch["untimed_ms"]
            phases = sorted(((k, v) for k, v in items.items() if k != "world"), key=lambda kv: -kv[1])[:4]
            say(f"  - {hitch.get('time_utc', '?')[:19]}Z {hitch.get('work_ms', 0):.0f} ms: " + ", ".join(f"{k} {v:.0f}" for k, v in phases))
        say("")

    if host:
        say("## Host")
        cpu = [num(r, "proc_cpu_pct") for r in host]
        steal = [num(r, "steal_pct") for r in host]
        throttled = [num(r, "cg_throttled_ms") for r in host]
        say(f"- Process CPU: average {statistics.mean(cpu):.0f}% of one core, max {max(cpu):.0f}%")
        say(f"- System busy {statistics.mean(num(r, 'sys_busy_pct') for r in host):.0f}%, steal {statistics.mean(steal):.1f}% avg / {max(steal):.1f}% max, "
            f"iowait {statistics.mean(num(r, 'iowait_pct') for r in host):.1f}%")
        say(f"- cgroup throttling: {sum(1 for t in throttled if t > 0)} of {len(host)} seconds throttled, {sum(throttled) / 1000:.1f} s in total")
        swap = max(num(r, "swap_mb") for r in host)
        say(f"- Memory: RSS max {max(num(r, 'rss_mb') for r in host):.0f} MB, swap max {swap:.0f} MB, available min {min(num(r, 'avail_phys_mb') for r in host):.0f} MB")
        flags = []
        if statistics.mean(steal) > 5:
            flags.append("high CPU steal: the host machine is oversold; other tenants take CPU time from this server")
        if sum(1 for t in throttled if t > 0) > len(host) * 0.05:
            flags.append("frequent cgroup throttling: the container's CPU quota is used up; the plan's CPU limit is the bottleneck")
        if quota and statistics.mean(cpu) > 90 * quota:
            flags.append(f"process CPU sits near the {quota:g}-core quota")
        if swap > 100:
            flags.append("the process is swapping: memory is too small")
        say("- **Flags:** " + ("; ".join(flags) if flags else "none (the host does not look starved; the work itself is the cost)"))
        say("")

    if net:
        say("## Network (per connection)")
        per = defaultdict(list)
        for row in net:
            per[row["player"]].append(row)
        for player, rows in per.items():
            say(f"- {player}: ping {statistics.mean(num(r, 'ping_ms') for r in rows):.0f} ms avg, out {statistics.mean(num(r, 'out_kbytes_s') for r in rows):.1f} KB/s "
                f"(cap {statistics.mean(num(r, 'net_speed_kbytes_s') for r in rows):.0f}), saturated {pct(sum(num(r, 'saturated') for r in rows), len(rows)):.0f}% of seconds, "
                f"lost out {sum(num(r, 'out_lost') for r in rows):.0f}")
        say("Saturation means the server had more to send than the connection's rate cap allowed: that is lag without a low tick rate.")
        say("")

    if snapshots:
        last = snapshots[-1]
        say(f"## Entities (last snapshot, {last.get('time_utc', '?')[:19]}Z)")
        say(f"- {last.get('actors')} actors, {last.get('actors_ticking')} with ticking enabled, {last.get('classes')} classes")
        for entry in last.get("top", [])[:15]:
            say(f"  - {entry['class']} ({entry['origin']}): {entry['count']} ({entry['ticking']} ticking)")
        say("")

    if marks:
        say("## Marks")
        for row in marks:
            say(f"- {row.get('time_utc', '?')[:19]}Z [{row.get('source')}] {row.get('text')}")
        say("")

    report = "\n".join(lines)
    print(report)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as handle:
            handle.write(report + "\n")


if __name__ == "__main__":
    main()
