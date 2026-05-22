"""
Generate PDF references of the USB CDC command surface for each project
(MyWota, BigYellow, Central Control Hub).

Each PDF contains:
  - Per-module sections (sys, mod, card, fs, wifi, rtc, plus adapter modules)
  - Every command + subcommand with description and example usage
  - A "Missing / Inconsistent" report comparing this project against the others

Source of truth: hand-extracted from
  sevantica_drivers/Source/USB/USB_Command_Handler.c
  application/Source/Adapters/USB_Command_Adapter.c (per project)

Run:
  python gen_command_pdf.py
PDFs are written next to this script.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import (
    SimpleDocTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
    PageBreak,
    KeepTogether,
)
from html import escape as _esc


def esc(s: str) -> str:
    """HTML-escape user text for ReportLab's mini-HTML Paragraph parser."""
    return _esc(s, quote=False)


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class Cmd:
    name: str            # full command, e.g. "sys status" or "dispenser start [L]"
    desc: str            # short description
    example: str         # concrete usage example
    notes: str = ""      # optional notes (gating, build flag, etc.)


@dataclass
class Module:
    name: str            # "sys", "mod", "card", ...
    summary: str         # one-line summary
    origin: str          # "core" | "adapter"
    gating: str = ""     # build flag / condition
    commands: list[Cmd] = field(default_factory=list)


@dataclass
class Project:
    key: str             # "mywota" | "bigyellow" | "cch"
    name: str            # display name
    blurb: str
    modules: list[Module]


# ---------------------------------------------------------------------------
# Shared "core" modules (sevantica_drivers/Source/USB/USB_Command_Handler.c)
# ---------------------------------------------------------------------------

def core_help() -> Module:
    return Module(
        name="help / ?",
        summary="Show available top-level commands (adapter + core).",
        origin="core",
        commands=[
            Cmd("help", "List all registered commands with descriptions.", "help"),
            Cmd("?", "Alias of `help`.", "?"),
            Cmd("clear", "Clear the terminal screen and re-print the welcome banner.", "clear"),
        ],
    )


def core_sys() -> Module:
    return Module(
        name="sys",
        summary="System-level inspection and lifecycle control.",
        origin="core",
        commands=[
            Cmd("sys status",   "Print device ID, site ID, RTC, MIFARE state, free heap, etc.",
                "sys status"),
            Cmd("sys config",   "Dump the full live configuration (`Config_PrintToUSB`).",
                "sys config"),
            Cmd("sys set <param> <value>",
                "Set a single configuration parameter and persist to SD + flash.",
                "sys set mifare.card_init_default_balance 5000"),
            Cmd("sys version",  "Show firmware version and build metadata.",
                "sys version"),
            Cmd("sys reboot",   "Software reset via watchdog.",
                "sys reboot"),
            Cmd("sys bootloader",
                "Reboot into RP2040 BOOTSEL (USB mass-storage) mode for re-flashing.",
                "sys bootloader"),
            Cmd("sys tasks",    "Print FreeRTOS task list with min-free stack (words).",
                "sys tasks"),
            Cmd("sys wdt [save|erase|raw]",
                "Watchdog log: dump latest entries; `save` flushes RAM->flash, "
                "`erase` wipes the WDT sector, `raw` dumps the raw flash slots.",
                "sys wdt\nsys wdt save\nsys wdt erase"),
            Cmd("sys security <on|off>",
                "Enable / disable the full MIFARE security stack (encryption, HMAC, "
                "replay, challenge-response, custom sector keys).",
                "sys security on"),
            Cmd("sys factory-reset",
                "Reload immutable defaults from the flash factory sector.",
                "sys factory-reset"),
            Cmd("sys factory-save",
                "Snapshot current source defaults into the flash factory sector.",
                "sys factory-save"),
            Cmd("sys defaults-apply",
                "Overwrite live (user) settings with the immutable source defaults.",
                "sys defaults-apply"),
        ],
    )


def core_mod() -> Module:
    return Module(
        name="mod",
        summary="Runtime control of system modules (start / stop / list).",
        origin="core",
        commands=[
            Cmd("mod list",  "Print module table with current run state.", "mod list"),
            Cmd("mod start <module>",
                "Start a module. Known names: lcd, mifare, dispenser, buzzer, ioexp, rs485.",
                "mod start dispenser"),
            Cmd("mod stop <module>",
                "Stop a module by name (same names as `mod start`).",
                "mod stop buzzer"),
        ],
    )


def core_card() -> Module:
    return Module(
        name="card",
        summary="MIFARE card maintenance and recovery (gated by USE_DRIVERS_NFC).",
        origin="core",
        gating="USE_DRIVERS_NFC",
        commands=[
            Cmd("card format",
                "Initialize / re-format the next presented MIFARE card with default data.",
                "card format"),
            Cmd("card topup <amount>",
                "Add tokens / volume units to the next presented card.",
                "card topup 5000"),
            Cmd("card log <UID_HEX>",
                "Read and print the on-card transaction log for the given UID.",
                "card log 04A1B2C3D4"),
            Cmd("card recover <UID_HEX>",
                "Restore card balance from the SD-side recovery log for that UID.",
                "card recover 04A1B2C3D4"),
            Cmd("card decrypt",
                "Decrypt and dump the next presented card (10 s wait) using current keys.",
                "card decrypt"),
        ],
    )


def core_fs() -> Module:
    return Module(
        name="fs",
        summary="SD card file system access (gated by USE_DRIVERS_FATFS).",
        origin="core",
        gating="USE_DRIVERS_FATFS",
        commands=[
            Cmd("fs ls",
                "List files in the SD root and cache them for `fs cat #N` access.",
                "fs ls"),
            Cmd("fs cat <name|#N>",
                "Print contents of a file by name, or by index from the last `fs ls`.",
                "fs cat config.txt\nfs cat #2"),
            Cmd("fs find <UID_HEX>",
                "Search SD logs for entries belonging to that card UID.",
                "fs find 04A1B2C3D4"),
            Cmd("fs format",
                "Delete every file on the SD card (does NOT low-level format).",
                "fs format"),
            Cmd("fs mount",
                "Expose the SD card to the PC via USB Mass Storage; suspends application I/O.",
                "fs mount"),
            Cmd("fs unmount",
                "Detach USB MSC and resume application access to the SD card.",
                "fs unmount"),
        ],
    )


def core_wifi_esp() -> Module:
    """Core wifi command (ESP8285, used by MyWota / BigYellow when CONNECTIVITY=on)."""
    return Module(
        name="wifi (core / ESP8285)",
        summary="WiFi via the ESP8285 driver (gated by USE_DRIVERS_CONNECTIVITY).",
        origin="core",
        gating="USE_DRIVERS_CONNECTIVITY",
        commands=[
            Cmd("wifi scan",   "Trigger an asynchronous scan; results in 3-5 s.", "wifi scan"),
            Cmd("wifi list",   "Print SSID / RSSI / security from the last scan.", "wifi list"),
            Cmd("wifi status", "Show driver state and IP address.", "wifi status"),
        ],
    )


def core_rtc() -> Module:
    return Module(
        name="rtc",
        summary="Real-time clock inspection and manual set.",
        origin="core",
        commands=[
            Cmd("rtc status",
                "Print current time, sync flag, unix timestamp and compile-time fallback.",
                "rtc status"),
            Cmd("rtc set <YYYY-MM-DD> <HH:MM:SS>",
                "Set RTC date/time and persist to SD.",
                "rtc set 2026-05-04 14:30:00"),
        ],
    )


# ---------------------------------------------------------------------------
# Adapter modules (per project)
# ---------------------------------------------------------------------------

# ----- MyWota -----
def mywota_dispenser() -> Module:
    return Module(
        name="dispenser",
        summary="Volume-based water dispenser control (MyWota adapter).",
        origin="adapter",
        commands=[
            Cmd("dispenser start [L]",
                "Start a manual dispense. Without arg -> unlimited; with arg -> N litres "
                "(min 1 L, max 1000 L).",
                "dispenser start\ndispenser start 5"),
            Cmd("dispenser stop",
                "Stop an in-progress manual dispense.",
                "dispenser stop"),
            Cmd("dispenser topup <L>",
                "Top up the next presented card by N litres (min 1 L). "
                "Internally multiplied to mL.",
                "dispenser topup 10"),
        ],
    )


def mywota_clean() -> Module:
    return Module(
        name="clean",
        summary="Local self-clean cycle for the dispenser (MyWota adapter).",
        origin="adapter",
        commands=[
            Cmd("clean start [vol_ml] [max_sec]",
                "Start a self-clean cycle. Missing/zero args use the configured defaults.",
                "clean start\nclean start 2000 60"),
            Cmd("clean stop",
                "Abort a running self-clean cycle.",
                "clean stop"),
            Cmd("clean status",
                "Print whether a clean is active and the last-clean unix timestamp.",
                "clean status"),
        ],
    )


def mywota_fault() -> Module:
    return Module(
        name="fault",
        summary="Inspect and clear the dispenser fault state machine (MyWota adapter).",
        origin="adapter",
        commands=[
            Cmd("fault status",
                "Print current fault state, reason and incident counter (since boot).",
                "fault status"),
            Cmd("fault clear",
                "Clear a latched FAULT (operator action).",
                "fault clear"),
        ],
    )


# ----- BigYellow -----
def bigyellow_wash() -> Module:
    return Module(
        name="washstart / washstop  (top-level)",
        summary="Manual car-wash control (BigYellow adapter). Note: these are NOT "
                "subcommanded, they are top-level commands.",
        origin="adapter",
        commands=[
            Cmd("washstart [seconds] [option]",
                "Start a wash without a card. `seconds` <= 3600. "
                "`option`: 1=vacuum, 2=brush, 3=pressure (default vacuum).",
                "washstart\nwashstart 120 2"),
            Cmd("washstop",
                "Stop the active wash immediately.",
                "washstop"),
        ],
    )


# ----- Central Control Hub -----
def cch_dispenser() -> Module:
    return Module(
        name="dispenser",
        summary="Local dispenser control (only in BUILD_TYPE_WATER_DISPENSER builds of CCH).",
        origin="adapter",
        gating="BUILD_TYPE_WATER_DISPENSER",
        commands=[
            Cmd("dispenser start [L]",
                "Manual dispense, identical semantics to MyWota.",
                "dispenser start 5"),
            Cmd("dispenser stop", "Stop manual dispense.", "dispenser stop"),
            Cmd("dispenser topup <L>", "Top up the card by N litres.", "dispenser topup 10"),
        ],
    )


def cch_wifi() -> Module:
    return Module(
        name="wifi (CCH adapter)",
        summary="CYW43-based WiFi via the Network task. Overrides the core ESP8285 `wifi` "
                "because adapter commands are dispatched first.",
        origin="adapter",
        commands=[
            Cmd("wifi scan",
                "Start a background scan (results retrieved by `wifi connect <ID>`).",
                "wifi scan"),
            Cmd("wifi connect <ID> <pwd>",
                "Connect using the scan result index from the last scan.",
                "wifi connect 1 myPassword"),
            Cmd("wifi connect <pwd>",
                "Connect to the compiled-in DEFAULT_WIFI_SSID using <pwd>.",
                "wifi connect myPassword"),
            Cmd("wifi disconnect", "Disconnect from the current network.", "wifi disconnect"),
            Cmd("wifi saved",
                "List saved networks (auto-connect MRU list, max WIFI_MAX_SAVED_NETWORKS).",
                "wifi saved"),
            Cmd("wifi forget <index>",
                "Remove a saved network by index (from `wifi saved`).",
                "wifi forget 0"),
            Cmd("wifi reinit", "Re-initialize the WiFi driver.", "wifi reinit"),
            Cmd("wifi status",
                "Print network state and IP (refreshes IP if 0.0.0.0).",
                "wifi status"),
        ],
    )


def cch_sd() -> Module:
    return Module(
        name="sdinit",
        summary="Re-run SD card initialization (CCH adapter).",
        origin="adapter",
        commands=[
            Cmd("sdinit",
                "Request the SD logger task to re-mount and re-initialize the card.",
                "sdinit"),
        ],
    )


def cch_rs485() -> Module:
    return Module(
        name="rs485 / remote",
        summary="RS-485 master bus inspection and slave CLI bridging (CCH adapter).",
        origin="adapter",
        commands=[
            Cmd("rs485",
                "Print the RS-485 slave table (address, online, type, state, balance, volume).",
                "rs485"),
            Cmd("remote <addr> <command...>",
                "Send a CLI command string over RS-485 to a slave; the slave runs it locally.",
                "remote 2 sys status\nremote 5 dispenser stop"),
        ],
    )


def cch_webserver() -> Module:
    return Module(
        name="webserver",
        summary="HTTP server lifecycle (CCH adapter).",
        origin="adapter",
        commands=[
            Cmd("webserver start",
                "Start the embedded web server (requires WiFi connected).",
                "webserver start"),
            Cmd("webserver status",
                "Print WiFi connection state and the server IP.",
                "webserver status"),
        ],
    )


def cch_clean() -> Module:
    return Module(
        name="clean (master)",
        summary="Site-wide clean orchestration over RS-485 (CCH adapter, different "
                "semantics from MyWota's local `clean`).",
        origin="adapter",
        commands=[
            Cmd("clean now <addr> [vol_ml] [max_sec]",
                "Trigger an immediate clean on a specific slave dispenser.",
                "clean now 2 2000 60"),
            Cmd("clean abort <addr>",
                "Abort an in-progress clean on a slave.",
                "clean abort 2"),
            Cmd("clean status",
                "Print scheduler state for all slaves.",
                "clean status"),
        ],
    )


def cch_filter() -> Module:
    return Module(
        name="filter",
        summary="Site-shared filter life accounting (CCH adapter).",
        origin="adapter",
        commands=[
            Cmd("filter status",
                "Print capacity, used, remaining %, session totals.",
                "filter status"),
            Cmd("filter reset",
                "Zero the used counter (after a physical filter replacement).",
                "filter reset"),
            Cmd("filter set <capacity_ml>",
                "Set the filter capacity in mL (0 disables tracking).",
                "filter set 50000"),
        ],
    )


def cch_stats() -> Module:
    return Module(
        name="stats",
        summary="Per-slave throughput statistics, hourly buckets (CCH adapter).",
        origin="adapter",
        commands=[
            Cmd("stats",
                "Dump the rolling per-slave throughput buckets.",
                "stats"),
        ],
    )


# ---------------------------------------------------------------------------
# Project assembly
# ---------------------------------------------------------------------------

def project_mywota() -> Project:
    return Project(
        key="mywota",
        name="MyWota (Pico water dispenser)",
        blurb=(
            "Volume-based water dispenser firmware on RP2040. Uses ESP8285 for WiFi "
            "(if enabled), MIFARE for prepaid balance in millilitres, and a local "
            "self-clean cycle. Adapter file: application/Source/Adapters/USB_Command_Adapter.c"
        ),
        modules=[
            core_help(),
            core_sys(),
            core_mod(),
            core_card(),
            core_fs(),
            core_wifi_esp(),
            core_rtc(),
            mywota_dispenser(),
            mywota_clean(),
            mywota_fault(),
        ],
    )


def project_bigyellow() -> Project:
    return Project(
        key="bigyellow",
        name="BigYellow (Pico car-wash terminal)",
        blurb=(
            "Token-based car-wash terminal on RP2040. MIFARE balance is in tokens and "
            "drives wash time. Adapter exposes flat top-level `washstart` / `washstop` "
            "commands instead of a `wash <subcmd>` dispatcher. Adapter file: "
            "application/Source/Adapters/USB_Command_Adapter.c"
        ),
        modules=[
            core_help(),
            core_sys(),
            core_mod(),
            core_card(),
            core_fs(),
            core_wifi_esp(),
            core_rtc(),
            bigyellow_wash(),
        ],
    )


def project_cch() -> Project:
    return Project(
        key="cch",
        name="Central Control Hub (Pico 2 W master)",
        blurb=(
            "RS-485 master + WiFi gateway on Pico 2 W. The `wifi` command is overridden "
            "by the adapter (CYW43 stack) and shadows the core ESP8285 implementation. "
            "Optionally builds with BUILD_TYPE_WATER_DISPENSER for a co-located "
            "dispenser. Adapter file: application/Source/Adapters/USB_Command_Adapter.c"
        ),
        modules=[
            core_help(),
            core_sys(),
            core_mod(),
            core_card(),
            core_fs(),
            core_rtc(),
            cch_dispenser(),
            cch_wifi(),
            cch_sd(),
            cch_rs485(),
            cch_webserver(),
            cch_clean(),
            cch_filter(),
            cch_stats(),
        ],
    )


# ---------------------------------------------------------------------------
# Cross-project gap analysis
# ---------------------------------------------------------------------------

def collect_top_level(project: Project) -> set[str]:
    """Return the set of top-level command tokens in this project (e.g. 'sys', 'dispenser')."""
    tokens: set[str] = set()
    for mod in project.modules:
        for cmd in mod.commands:
            tokens.add(cmd.name.split()[0])
    return tokens


def gap_report(target: Project, others: list[Project]) -> list[tuple[str, str]]:
    """Return list of (heading, body) entries for the missing-commands chapter."""
    target_tokens = collect_top_level(target)
    union: dict[str, list[str]] = {}
    for p in others:
        for tok in collect_top_level(p):
            union.setdefault(tok, []).append(p.name)

    missing: list[tuple[str, str]] = []
    for tok, where in sorted(union.items()):
        if tok not in target_tokens:
            missing.append((
                f"`{tok}` — missing in {target.name}",
                f"Present in: {', '.join(where)}.",
            ))

    # Hand-curated structural / consistency notes
    notes: list[tuple[str, str]] = []

    if target.key == "mywota":
        notes += [
            ("`stats` — missing",
             "CCH exposes per-slave throughput buckets via `stats`. MyWota has no equivalent "
             "for its local dispenser (would need a Throughput_Stats adapter)."),
            ("`filter` — missing",
             "MyWota has its own filter but no CLI surface. Site-side filter accounting only "
             "exists on CCH. A local `filter status|reset|set` would parallel CCH."),
            ("`webserver` — missing",
             "ESP8285 wifi has no HTTP server hook. If a config/diag UI is wanted, a wrapper "
             "around an ESP-side server would be required."),
            ("`wifi connect/disconnect/saved/forget/reinit` — missing",
             "Core `wifi` exposes only `scan/list/status` for ESP8285. No way to join a "
             "network from the CLI without using `sys set` on raw config keys."),
        ]
    elif target.key == "bigyellow":
        notes += [
            ("`fault` — missing",
             "MyWota exposes a fault state machine via `fault status|clear`. BigYellow has "
             "no equivalent CLI even though the wash controller can fault."),
            ("`clean` — missing",
             "No self-clean concept (water dispenser feature). Acceptable for a car-wash, "
             "but worth confirming."),
            ("Inconsistent shape: `washstart` / `washstop` vs `wash <start|stop>`",
             "Every other project groups subcommands under a dispatcher. Consider exposing "
             "`wash start [seconds] [option]` / `wash stop` for symmetry with `dispenser` "
             "and `clean` on the other projects."),
            ("`stats` / `filter` / `webserver` — missing",
             "Same gap as MyWota — see CCH for the canonical surface."),
        ]
    elif target.key == "cch":
        notes += [
            ("`fault` — missing",
             "MyWota has `fault status|clear` for its dispenser. CCH could surface "
             "aggregated slave fault state (per-slave or master)."),
            ("`clean` semantics differ from MyWota",
             "On CCH, `clean now/abort/status` orchestrates slaves over RS-485. On MyWota, "
             "`clean start/stop/status` is local. The verb overlap is intentional but easy "
             "to confuse — document carefully."),
        ]

    return missing + notes


# ---------------------------------------------------------------------------
# PDF rendering
# ---------------------------------------------------------------------------

def build_styles():
    base = getSampleStyleSheet()
    styles = {
        "title": ParagraphStyle(
            "title", parent=base["Title"], fontSize=22, leading=26,
            spaceAfter=8, textColor=colors.HexColor("#1f3a93"),
        ),
        "subtitle": ParagraphStyle(
            "subtitle", parent=base["Normal"], fontSize=11, leading=14,
            textColor=colors.HexColor("#555555"), spaceAfter=14,
        ),
        "h1": ParagraphStyle(
            "h1", parent=base["Heading1"], fontSize=16, leading=20,
            spaceBefore=14, spaceAfter=6, textColor=colors.HexColor("#1f3a93"),
        ),
        "h2": ParagraphStyle(
            "h2", parent=base["Heading2"], fontSize=13, leading=16,
            spaceBefore=10, spaceAfter=4, textColor=colors.HexColor("#264653"),
        ),
        "body": ParagraphStyle(
            "body", parent=base["BodyText"], fontSize=10, leading=13,
            spaceAfter=4,
        ),
        "code": ParagraphStyle(
            "code", parent=base["Code"], fontSize=9, leading=11,
            textColor=colors.HexColor("#333333"),
            backColor=colors.HexColor("#f4f4f4"),
            borderPadding=4,
        ),
        "muted": ParagraphStyle(
            "muted", parent=base["Normal"], fontSize=9, leading=11,
            textColor=colors.HexColor("#888888"),
        ),
    }
    return styles


def cmd_table(commands: Iterable[Cmd], styles) -> Table:
    rows = [["Command", "Description", "Example"]]
    for c in commands:
        notes = f"<br/><font color='#888'>{esc(c.notes)}</font>" if c.notes else ""
        example_html = esc(c.example).replace("\n", "<br/>")
        rows.append([
            Paragraph(f"<b>{esc(c.name)}</b>", styles["body"]),
            Paragraph(esc(c.desc) + notes, styles["body"]),
            Paragraph(example_html, styles["code"]),
        ])
    tbl = Table(rows, colWidths=[55 * mm, 70 * mm, 55 * mm], repeatRows=1)
    tbl.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1f3a93")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, 0), 10),
        ("BOTTOMPADDING", (0, 0), (-1, 0), 6),
        ("TOPPADDING", (0, 0), (-1, 0), 6),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1),
         [colors.white, colors.HexColor("#f7f9fc")]),
        ("GRID", (0, 0), (-1, -1), 0.25, colors.HexColor("#cfd8e3")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
    ]))
    return tbl


def render_project(project: Project, others: list[Project], out_path: Path):
    styles = build_styles()
    doc = SimpleDocTemplate(
        str(out_path), pagesize=A4,
        leftMargin=15 * mm, rightMargin=15 * mm,
        topMargin=15 * mm, bottomMargin=15 * mm,
        title=f"USB CLI Reference — {project.name}",
        author="Sevantica",
    )

    story = []

    # Cover
    story.append(Paragraph(f"USB CLI Reference", styles["title"]))
    story.append(Paragraph(project.name, styles["h1"]))
    story.append(Paragraph(project.blurb, styles["subtitle"]))
    story.append(Paragraph(
        "Source: <code>sevantica_drivers/Source/USB/USB_Command_Handler.c</code> "
        "(core) + project adapter "
        "<code>application/Source/Adapters/USB_Command_Adapter.c</code>. "
        "Adapter commands are dispatched <i>before</i> core commands, so an adapter "
        "may shadow a core command of the same name.",
        styles["body"]))
    story.append(Spacer(1, 6))

    # Module sections
    for mod in project.modules:
        header = f"{mod.name}"
        story.append(Paragraph(header, styles["h1"]))
        meta = f"<b>Origin:</b> {mod.origin}"
        if mod.gating:
            meta += f" &nbsp;|&nbsp; <b>Build flag:</b> <code>{mod.gating}</code>"
        story.append(Paragraph(meta, styles["muted"]))
        story.append(Paragraph(mod.summary, styles["body"]))
        story.append(Spacer(1, 4))
        story.append(cmd_table(mod.commands, styles))
        story.append(Spacer(1, 8))

    # Gap report
    story.append(PageBreak())
    story.append(Paragraph("Missing / Inconsistent Commands", styles["title"]))
    story.append(Paragraph(
        f"Cross-checked against: {', '.join(o.name for o in others)}.",
        styles["subtitle"]))

    gaps = gap_report(project, others)
    if not gaps:
        story.append(Paragraph(
            "No top-level commands missing relative to the other projects.",
            styles["body"]))
    else:
        for heading, body in gaps:
            story.append(KeepTogether([
                Paragraph(heading, styles["h2"]),
                Paragraph(body, styles["body"]),
            ]))
            story.append(Spacer(1, 2))

    doc.build(story)


# ---------------------------------------------------------------------------
# Transport-coverage report (USB / RS-485 / Web)
# ---------------------------------------------------------------------------
#
# Source of truth:
#   - USB CLI:    sevantica_drivers/Source/USB/USB_Command_Handler.c
#                 application/Source/Adapters/USB_Command_Adapter.c (per project)
#   - RS-485:     sevantica_drivers RS485_Protocol.h (RS485_CMD_*),
#                 application/Source/Adapters/RS485_Command_Adapter.c (slave),
#                 Central-Control-Hub/.../RS485_Task.c, Clean_Scheduler.c (master)
#   - Web/HTTP:   Central-Control-Hub/.../CYW43_Web_Server.c
#                 (/api/status, /api/control, /api/cmd) + static SD-served files

# Each row is: (operation, usb, rs485, web, severity, note)
# Severity:
#   "ok"   - reachable on every transport that should carry it
#   "warn" - reachable but only via the DEBUG_CMD CLI tunnel (no native opcode/route)
#   "miss" - destination unreachable from at least one transport that should have it
#   "drop" - sent on a transport but no handler at the destination (silently dropped)
GAP_ROWS = [
    # --- RS-485 protocol housekeeping ---
    ("Ping device",          "—",                    "PING (0x01)",                "—",                                "drop",
     "Opcode defined; no slave handler."),
    ("Reset device",         "sys reboot (local)",   "RESET (0x03)",               "—",                                "drop",
     "RS-485 RESET has no slave handler. USB only resets the local device."),
    ("Get device info",      "sys version",          "GET_INFO (0x04)",            "via /api/cmd",                     "warn",
     "RS-485 returns FW/serial; web has no dedicated endpoint."),
    ("Set slave address",    "—",                    "SET_ADDRESS (0x05)",         "—",                                "drop",
     "Opcode defined; no slave handler."),
    ("Sync RTC to slave",    "—",                    "SYNC_TIME (0x06)",           "—",                                "drop",
     "Master sends SYNC_TIME (RS485_Task.c); slaves do not handle it. Use `remote <addr> rtc set` as workaround."),
    ("Discover slaves",      "—",                    "DISCOVER (0x07) / RSP (0x08)", "—",                              "drop",
     "Opcodes defined; no slave handler. CCH cannot auto-enumerate the bus."),
    ("Heartbeat",            "sys tasks (local)",    "HEARTBEAT (0x24)",           "—",                                "drop",
     "Opcode defined; no slave handler."),
    ("Busy notify",          "—",                    "BUSY (0x12)",                "—",                                "drop",
     "Opcode defined; no slave handler."),
    ("ACK / NAK",            "—",                    "ACK (0x10) / NAK (0x11)",    "—",                                "ok",
     "Implemented on both sides."),
    ("Poll status",          "sys status (local)",   "POLL_STATUS (0x20) / RSP (0x21)", "/api/status",                 "ok",
     "Master polls slaves; web aggregates."),
    ("Poll events",          "—",                    "POLL_EVENTS (0x22) / RSP (0x23)", "—",                          "drop",
     "Opcodes defined; no slave handler."),

    # --- Dispenser ---
    ("Start dispense",       "dispenser start [L]",  "DEBUG_CMD tunnel",           "/api/cmd addr=N cmd='dispenser start'", "warn",
     "No native DISPENSE_START opcode; routed through CLI bridge."),
    ("Stop dispense",        "dispenser stop",       "DEBUG_CMD tunnel",           "/api/cmd cmd='dispenser stop'",    "warn",
     "Same as above."),
    ("Topup card",           "dispenser topup <L> / card topup",
                                                     "DEBUG_CMD tunnel",           "/api/cmd cmd='dispenser topup N'", "warn",
     "No native CARD_UPDATE / topup opcode handler on slave."),
    ("Read balance",         "sys status, card log", "POLL_STATUS payload",        "/api/status",                      "ok",
     "Reachable everywhere."),

    # --- Self-clean ---
    ("Trigger clean",        "clean start [vol] [sec] (MyWota)",
                                                     "TRIGGER_CLEAN (0x09)",       "/api/cmd cmd='clean start ...'",   "drop",
     "Master queues TRIGGER_CLEAN (Clean_Scheduler.c) but slave RS485_Command_Adapter has no handler. Web works only because it falls back to DEBUG_CMD."),
    ("Abort clean",          "clean stop (MyWota)",  "—",                          "/api/cmd cmd='clean stop'",        "miss",
     "No native abort opcode; web works via DEBUG_CMD only."),
    ("Clean status",         "clean status (MyWota)","—",                          "/api/cmd cmd='clean status'",      "miss",
     "Master-side `clean status` is local; per-slave clean state must be tunneled."),

    # --- Car wash (BigYellow) ---
    ("Start wash",           "washstart [s] [opt]",  "DEBUG_CMD tunnel",           "/api/cmd cmd='washstart'",         "warn",
     "BigYellow uses flat top-level command; no native opcode."),
    ("Stop wash",            "washstop",             "DEBUG_CMD tunnel",           "/api/cmd cmd='washstop'",          "warn",
     "Same as above."),

    # --- Card / MIFARE management ---
    ("Format / init card",   "card format",          "—",                          "—",                                "miss",
     "USB only. Cannot remotely re-init a slave's card session."),
    ("Card log",             "card log <UID>",       "—",                          "—",                                "miss",
     "USB only."),
    ("Card recover",         "card recover <UID>",   "—",                          "—",                                "miss",
     "USB only."),
    ("Card decrypt",         "card decrypt",         "—",                          "—",                                "miss",
     "USB only."),
    ("Card transactions push","—",                   "TRANSACTION (0x30) / BATCH (0x31) / GET_TX (0x33)", "—",         "drop",
     "Whole transaction-sync family defined; no slave handlers. Currently substituted by DEBUG_CMD log capture."),
    ("Card detected event",  "—",                    "CARD_DETECTED (0x40)",       "—",                                "drop",
     "Opcode defined; no slave handler."),
    ("Card data transfer",   "—",                    "CARD_DATA (0x41)",           "—",                                "drop",
     "Opcode defined; no slave handler."),
    ("Card update (balance)","dispenser topup",      "CARD_UPDATE (0x42)",         "via /api/cmd",                     "drop",
     "Opcode defined; no slave handler."),
    ("Card blacklist",       "—",                    "CARD_BLACKLIST (0x43)",      "—",                                "drop",
     "Opcode defined; no slave handler."),

    # --- WiFi (CCH) ---
    ("WiFi scan",            "wifi scan",            "—",                          "—",                                "miss",
     "USB only on CCH."),
    ("WiFi connect",         "wifi connect <ID|pwd>","—",                          "—",                                "miss",
     "USB only. Cannot provision a slave's WiFi (slaves don't have WiFi anyway)."),
    ("WiFi disconnect / saved / forget / reinit",
                             "wifi disconnect|saved|forget|reinit",
                                                     "—",                          "—",                                "miss",
     "USB only."),
    ("WiFi status",          "wifi status",          "—",                          "/api/status (partial)",            "warn",
     "/api/status reflects connectivity but not network state machine."),

    # --- RTC ---
    ("RTC read",             "rtc status",           "—",                          "/api/status (current_time)",       "ok",
     "Reachable on USB and web; RS-485 lacks a dedicated read opcode."),
    ("RTC set",              "rtc set <date> <time>","—",                          "—",                                "miss",
     "USB only. To set slave RTC, must use `remote <addr> rtc set`."),

    # --- Configuration ---
    ("Get config (slave)",   "sys config (local)",   "GET_CONFIG (0x50)",          "via /api/cmd 'sys config'",        "drop",
     "Slave RS485_Command_Adapter::cmd_get_config is a TODO and returns no data."),
    ("Set config (slave)",   "sys set <param> <val> (local)",
                                                     "SET_CONFIG (0x51)",          "via /api/cmd 'sys set ...'",       "drop",
     "Opcode defined; no slave handler."),
    ("Config response",      "—",                    "CONFIG_RESPONSE (0x52)",     "—",                                "drop",
     "Counterpart to GET_CONFIG; never emitted."),
    ("Factory reset / save / defaults-apply",
                             "sys factory-reset|factory-save|defaults-apply",
                                                     "—",                          "—",                                "miss",
     "USB only. No remote way to factory-reset a slave."),

    # --- Module lifecycle ---
    ("Module list",          "mod list",             "—",                          "/api/status",                      "warn",
     "Web exposes module state read-only. RS-485 has no module-control opcode."),
    ("Module start / stop",  "mod start <m> / mod stop <m>",
                                                     "—",                          "/api/control",                     "miss",
     "/api/control toggles LOCAL CCH modules only. To start/stop a slave's module, must use `/api/cmd remote ...`."),

    # --- File system / SD ---
    ("List SD files",        "fs ls",                "—",                          "GET /<file> (read-only static)",   "miss",
     "Web serves static files; no listing endpoint. RS-485 has nothing."),
    ("Read SD file",          "fs cat <name>",       "—",                          "GET /<path>",                      "warn",
     "Web reads /www/* from SD; CLI reads any file. No RS-485 path."),
    ("Find by UID",          "fs find <UID>",        "—",                          "—",                                "miss",
     "USB only."),
    ("Format / wipe SD",     "fs format",            "—",                          "—",                                "miss",
     "USB only."),
    ("Mount / unmount MSC",  "fs mount / fs unmount","—",                          "—",                                "miss",
     "USB only (physically requires USB anyway)."),
    ("Re-init SD",           "sdinit (CCH)",         "—",                          "—",                                "miss",
     "USB only."),

    # --- Bus inspection (CCH) ---
    ("List RS-485 slaves",   "rs485",                "(implicit polling)",         "/api/status",                      "ok",
     "Master keeps slave table; web exposes JSON."),
    ("Forward CLI to slave", "remote <addr> <cmd>",  "DEBUG_CMD (0xF1) + DEBUG_FETCH (0xF2)",
                                                                                   "/api/cmd addr=N",                  "ok",
     "Universal escape hatch — every transport supports it."),
    ("Debug log channel",    "(USB log stream)",     "DEBUG_LOG (0xF0)",           "—",                                "warn",
     "RS-485 carries slave debug logs to master; web has no log-stream endpoint."),

    # --- Site / system services (CCH) ---
    ("Start web server",     "webserver start",      "—",                          "(self)",                           "ok",
     "Local-only command; bootstraps the web transport."),
    ("Web server status",    "webserver status",     "—",                          "/api/status (partial)",            "ok",
     ""),
    ("Filter status / reset / set",
                             "filter status|reset|set <ml> (CCH)",
                                                     "—",                          "—",                                "miss",
     "USB only on CCH."),
    ("Throughput stats",     "stats (CCH)",          "—",                          "—",                                "miss",
     "USB only on CCH."),
    ("Fault status / clear", "fault status|clear (MyWota)",
                                                     "—",                          "—",                                "miss",
     "USB only on MyWota. CCH cannot read or clear a slave fault remotely."),

    # --- WDT / diagnostics ---
    ("WDT log read / save / erase",
                             "sys wdt [save|erase|raw]",
                                                     "—",                          "—",                                "miss",
     "USB only."),
    ("Tasks / stack high-water",
                             "sys tasks",            "—",                          "—",                                "miss",
     "USB only."),
    ("BOOTSEL reflash",      "sys bootloader",       "—",                          "—",                                "miss",
     "USB only (intentional — physical re-flash path)."),

    # --- Firmware update over the wire ---
    ("FW update start",      "—",                    "FW_START (0x60)",            "—",                                "warn",
     "Master sends; handled inside RS485_Task on slave (not yet via adapter)."),
    ("FW data chunk",        "—",                    "FW_DATA (0x61)",             "—",                                "warn",
     "Same as FW_START."),
    ("FW verify",            "—",                    "FW_VERIFY (0x62)",           "—",                                "warn",
     "Same as FW_START."),
    ("FW apply",             "—",                    "FW_APPLY (0x63)",            "—",                                "warn",
     "Same as FW_START."),

    # --- Security ---
    ("Security mode toggle", "sys security <on|off>","—",                          "—",                                "miss",
     "USB only. Cannot toggle on a slave without `remote ...`."),
]


def render_transport_coverage(out_path: Path):
    styles = build_styles()
    doc = SimpleDocTemplate(
        str(out_path), pagesize=A4,
        leftMargin=12 * mm, rightMargin=12 * mm,
        topMargin=14 * mm, bottomMargin=14 * mm,
        title="USB / RS-485 / Web — Transport Coverage",
        author="Sevantica",
    )

    story = []
    story.append(Paragraph("Transport Coverage Report", styles["title"]))
    story.append(Paragraph(
        "Every operation that exists on at least one transport, mapped against the other two, "
        "with destination-reachability flagged.",
        styles["subtitle"]))
    story.append(Paragraph(
        "Sources: <code>USB_Command_Handler.c</code>, <code>USB_Command_Adapter.c</code> "
        "(per project), <code>RS485_Protocol.h</code>, <code>RS485_Command_Adapter.c</code>, "
        "<code>RS485_Task.c</code>, <code>Clean_Scheduler.c</code>, "
        "<code>CYW43_Web_Server.c</code>.",
        styles["body"]))

    # Legend
    story.append(Paragraph("Legend", styles["h2"]))
    legend = [
        ("ok",   colors.HexColor("#2ecc71"),
         "Reachable on every transport that should carry it."),
        ("warn", colors.HexColor("#f1c40f"),
         "Reachable, but only via the DEBUG_CMD CLI tunnel — no native opcode / no native route."),
        ("miss", colors.HexColor("#e67e22"),
         "Operation exists on at least one transport but is missing on another that needs it."),
        ("drop", colors.HexColor("#e74c3c"),
         "Command IS sent on a transport but the destination has no handler — silently dropped."),
    ]
    legend_rows = [["", "Tag", "Meaning"]]
    for tag, color, meaning in legend:
        legend_rows.append(["", tag, meaning])
    legend_tbl = Table(legend_rows, colWidths=[8 * mm, 18 * mm, 150 * mm])
    legend_style = [
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, -1), 9),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("GRID", (1, 0), (-1, -1), 0.25, colors.HexColor("#cfd8e3")),
    ]
    for i, (_, color, _m) in enumerate(legend, start=1):
        legend_style.append(("BACKGROUND", (0, i), (0, i), color))
    legend_tbl.setStyle(TableStyle(legend_style))
    story.append(legend_tbl)
    story.append(Spacer(1, 8))

    # Section A: RS-485 opcode summary (drops)
    story.append(Paragraph("RS-485 opcodes with no slave handler (silent drop)", styles["h1"]))
    story.append(Paragraph(
        "These opcodes are defined in <code>RS485_Protocol.h</code> and may be sent by the "
        "CCH master, but the slave-side <code>RS485_Command_Adapter</code> does not implement "
        "them. The bytes go on the wire and nothing happens at the destination.",
        styles["body"]))
    drop_ops = [
        "PING (0x01)", "RESET (0x03)", "SET_ADDRESS (0x05)", "SYNC_TIME (0x06)",
        "DISCOVER (0x07) / DISCOVER_RESPONSE (0x08)", "TRIGGER_CLEAN (0x09)",
        "BUSY (0x12)", "POLL_EVENTS (0x22) / EVENT_RESPONSE (0x23)", "HEARTBEAT (0x24)",
        "TRANSACTION (0x30) / TRANSACTION_BATCH (0x31) / TRANSACTION_ACK (0x32) / GET_TRANSACTIONS (0x33)",
        "CARD_DETECTED (0x40) / CARD_DATA (0x41) / CARD_UPDATE (0x42) / CARD_BLACKLIST (0x43)",
        "SET_CONFIG (0x51) / CONFIG_RESPONSE (0x52)",
        "GET_CONFIG (0x50) — handler is a TODO that returns no payload",
    ]
    for s in drop_ops:
        story.append(Paragraph("• " + esc(s), styles["body"]))

    story.append(Spacer(1, 6))

    # Section B: Web endpoints
    story.append(Paragraph("Web / HTTP endpoints (CCH)", styles["h1"]))
    web_rows = [["Method", "Path", "Handler / behaviour"]]
    web_data = [
        ("GET",  "/api/status",      "cgi_api_status — JSON snapshot: slave table, module states, RTC, SD."),
        ("POST", "/api/control",     "cgi_api_control — start/stop LOCAL CCH modules only (toggle)."),
        ("POST", "/api/cmd",         "cgi_api_cmd — addr=0 runs USB_Command_HandleString locally; "
                                     "addr=1..N tunnels via RS485_CMD_DEBUG_CMD (universal CLI bridge)."),
        ("GET",  "/, /<path>",       "fs_open_custom — static files from SD card /www/."),
    ]
    for m, p, h in web_data:
        web_rows.append([
            Paragraph(f"<b>{esc(m)}</b>", styles["body"]),
            Paragraph(f"<code>{esc(p)}</code>", styles["body"]),
            Paragraph(esc(h), styles["body"]),
        ])
    web_tbl = Table(web_rows, colWidths=[18 * mm, 36 * mm, 130 * mm], repeatRows=1)
    web_tbl.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1f3a93")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, 0), 10),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1),
         [colors.white, colors.HexColor("#f7f9fc")]),
        ("GRID", (0, 0), (-1, -1), 0.25, colors.HexColor("#cfd8e3")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]))
    story.append(web_tbl)

    story.append(PageBreak())

    # Section C: full matrix
    story.append(Paragraph("Operation × Transport matrix", styles["h1"]))
    story.append(Paragraph(
        "Each row is one logical operation. The colour chip on the left encodes the "
        "destination-reachability tag (see Legend). Cells marked “—” mean the operation "
        "has no surface on that transport.",
        styles["body"]))

    rows = [["", "Operation", "USB CLI", "RS-485", "Web / HTTP", "Note"]]
    severity_color = {
        "ok":   colors.HexColor("#2ecc71"),
        "warn": colors.HexColor("#f1c40f"),
        "miss": colors.HexColor("#e67e22"),
        "drop": colors.HexColor("#e74c3c"),
    }
    for (op, usb, rs, web, sev, note) in GAP_ROWS:
        rows.append([
            "",
            Paragraph(f"<b>{esc(op)}</b>", styles["body"]),
            Paragraph(esc(usb), styles["code"]),
            Paragraph(esc(rs),  styles["code"]),
            Paragraph(esc(web), styles["code"]),
            Paragraph(esc(note), styles["body"]),
        ])

    matrix = Table(
        rows,
        colWidths=[5 * mm, 38 * mm, 35 * mm, 35 * mm, 36 * mm, 35 * mm],
        repeatRows=1,
    )
    style_cmds = [
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1f3a93")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, 0), 9),
        ("ROWBACKGROUNDS", (1, 1), (-1, -1),
         [colors.white, colors.HexColor("#f7f9fc")]),
        ("GRID", (1, 0), (-1, -1), 0.25, colors.HexColor("#cfd8e3")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
        ("TOPPADDING", (0, 0), (-1, -1), 3),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
    ]
    for i, (_, _, _, _, sev, _) in enumerate(GAP_ROWS, start=1):
        style_cmds.append(("BACKGROUND", (0, i), (0, i), severity_color[sev]))
    matrix.setStyle(TableStyle(style_cmds))
    story.append(matrix)

    # Summary
    story.append(PageBreak())
    story.append(Paragraph("Where commands fail to reach their destination", styles["h1"]))

    counts = {"ok": 0, "warn": 0, "miss": 0, "drop": 0}
    for r in GAP_ROWS:
        counts[r[4]] += 1
    summary_rows = [
        ["Tag", "Count", "Meaning"],
        ["ok",   counts["ok"],   "Reachable on every transport that should carry it"],
        ["warn", counts["warn"], "Reachable only via DEBUG_CMD CLI tunnel (no native opcode/route)"],
        ["miss", counts["miss"], "Operation exists on USB but never plumbed to RS-485 or web"],
        ["drop", counts["drop"], "RS-485 opcode SENT but slave has no handler — silently dropped"],
    ]
    sumtbl = Table(summary_rows, colWidths=[18 * mm, 18 * mm, 145 * mm], repeatRows=1)
    sumstyle = [
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1f3a93")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("GRID", (0, 0), (-1, -1), 0.25, colors.HexColor("#cfd8e3")),
        ("FONTSIZE", (0, 0), (-1, -1), 10),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
    ]
    for i, key in enumerate(["ok", "warn", "miss", "drop"], start=1):
        sumstyle.append(("BACKGROUND", (0, i), (0, i), severity_color[key]))
        sumstyle.append(("TEXTCOLOR", (0, i), (0, i), colors.white))
        sumstyle.append(("FONTNAME", (0, i), (0, i), "Helvetica-Bold"))
    sumtbl.setStyle(TableStyle(sumstyle))
    story.append(sumtbl)
    story.append(Spacer(1, 8))

    story.append(Paragraph("Top action items", styles["h2"]))
    actions = [
        ("Implement TRIGGER_CLEAN (0x09) on the slave",
         "CCH master already queues this opcode in <code>Clean_Scheduler.c</code>; "
         "slave currently ignores it. The web `/api/cmd` workaround masks the bug."),
        ("Implement SYNC_TIME (0x06) on the slave",
         "RS485_Task on CCH sends SYNC_TIME but slaves do not accept it. RTC drift "
         "between master and slaves is currently only fixable via `remote <addr> rtc set`."),
        ("Finish GET_CONFIG / SET_CONFIG (0x50 / 0x51)",
         "<code>cmd_get_config</code> in the slave adapter is a TODO. There is no way "
         "to read or push a slave's config without a CLI tunnel."),
        ("Add transaction-sync handlers (0x30–0x33)",
         "Whole opcode family unused. Today the only way the master sees slave "
         "transactions is by parsing log output captured through DEBUG_CMD."),
        ("Add module-control opcodes (or a generic remote `mod start/stop`)",
         "/api/control only toggles LOCAL CCH modules. A slave's modules can only be "
         "stopped via DEBUG_CMD."),
        ("Define MISSING vs. WAIT-FOR-CARD semantics for `card *` opcodes",
         "<code>card format/log/recover/decrypt</code> have no remote pathway. "
         "If field service ever needs them, they currently require physical USB."),
        ("Consider deprecating DEBUG_CMD as the universal bridge",
         "Roughly half of the “working remote” operations only work because they "
         "fall back to a textual CLI parse on the slave — which defeats the structured protocol."),
    ]
    for title, body in actions:
        story.append(KeepTogether([
            Paragraph("• " + esc(title), styles["h2"]),
            Paragraph(body, styles["body"]),
        ]))

    doc.build(story)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    out_dir = Path(__file__).resolve().parent
    out_dir.mkdir(parents=True, exist_ok=True)

    projects = [project_mywota(), project_bigyellow(), project_cch()]

    for proj in projects:
        others = [p for p in projects if p.key != proj.key]
        out_path = out_dir / f"USB_CLI_Reference_{proj.key}.pdf"
        render_project(proj, others, out_path)
        print(f"Wrote {out_path}")

    coverage_path = out_dir / "USB_CLI_Transport_Coverage.pdf"
    render_transport_coverage(coverage_path)
    print(f"Wrote {coverage_path}")


if __name__ == "__main__":
    main()
