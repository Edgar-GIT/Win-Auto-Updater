"""Versioned macro.json schema (load / save / list)."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from . import MACRO_FORMAT_VERSION


@dataclass
class UiaInfo:
    automation_id: str = ""
    name: str = ""
    control_type: str = ""

    def useful(self) -> bool:
        return bool(self.automation_id or self.name)


@dataclass
class WindowInfo:
    process: str = ""
    class_name: str = ""
    title: str = ""
    monitor: int = 0
    dpi: int = 96
    # Outer window size at capture time (physical px) — used to scale clicks
    # across DPI/scale changes (100% ↔ 150%, etc.). 0 = unknown (old macros).
    width: int = 0
    height: int = 0


@dataclass
class Step:
    type: str  # "click" | "key"
    window: WindowInfo = field(default_factory=WindowInfo)
    dt_ms: int = 0
    # click
    button: str = "left"  # left | right
    rel_x: int = 0
    rel_y: int = 0
    # Fraction of window size at capture (0..1). Preferred for cross-DPI replay.
    # -1 = unset (legacy macros).
    nx: float = -1.0
    ny: float = -1.0
    uia: UiaInfo | None = None
    # key
    vk: int = 0
    key: str = ""
    ctrl: bool = False
    shift: bool = False
    alt: bool = False
    win: bool = False

    def summary(self) -> str:
        proc = self.window.process or "?"
        if self.type == "click":
            side = "R" if self.button == "right" else "L"
            uia = ""
            if self.uia and self.uia.useful():
                label = self.uia.name or self.uia.automation_id
                uia = f", UIA '{label}'"
            return f"{proc}: clique {side} em ({self.rel_x},{self.rel_y} relativo){uia}"
        mods = []
        if self.ctrl:
            mods.append("Ctrl")
        if self.shift:
            mods.append("Shift")
        if self.alt:
            mods.append("Alt")
        if self.win:
            mods.append("Win")
        combo = "+".join(mods + [self.key or f"VK_{self.vk}"])
        return f"{proc}: tecla {combo}"


@dataclass
class MacroDocument:
    version: int = MACRO_FORMAT_VERSION
    created_at: str = ""
    dpi_awareness: str = ""
    steps: list[Step] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        data: dict[str, Any] = {
            "version": self.version,
            "created_at": self.created_at,
            "dpi_awareness": self.dpi_awareness,
            "steps": [],
        }
        for step in self.steps:
            item: dict[str, Any] = {
                "type": step.type,
                "dt_ms": step.dt_ms,
                "window": {
                    "process": step.window.process,
                    "class_name": step.window.class_name,
                    "title": step.window.title,
                    "monitor": step.window.monitor,
                    "dpi": step.window.dpi,
                    "width": step.window.width,
                    "height": step.window.height,
                },
            }
            if step.type == "click":
                item["button"] = step.button
                item["rel_x"] = step.rel_x
                item["rel_y"] = step.rel_y
                if step.nx >= 0.0 and step.ny >= 0.0:
                    item["nx"] = round(step.nx, 6)
                    item["ny"] = round(step.ny, 6)
                if step.uia and step.uia.useful():
                    item["uia"] = {
                        "automation_id": step.uia.automation_id,
                        "name": step.uia.name,
                        "control_type": step.uia.control_type,
                    }
            else:
                item["vk"] = step.vk
                item["key"] = step.key
                item["modifiers"] = {
                    "ctrl": step.ctrl,
                    "shift": step.shift,
                    "alt": step.alt,
                    "win": step.win,
                }
            data["steps"].append(item)
        return data


def _window_from_dict(raw: dict[str, Any] | None) -> WindowInfo:
    raw = raw or {}
    return WindowInfo(
        process=str(raw.get("process", "")),
        class_name=str(raw.get("class_name", "")),
        title=str(raw.get("title", "")),
        monitor=int(raw.get("monitor", 0) or 0),
        dpi=int(raw.get("dpi", 96) or 96),
        width=int(raw.get("width", 0) or 0),
        height=int(raw.get("height", 0) or 0),
    )


def _uia_from_dict(raw: dict[str, Any] | None) -> UiaInfo | None:
    if not raw:
        return None
    info = UiaInfo(
        automation_id=str(raw.get("automation_id", "")),
        name=str(raw.get("name", "")),
        control_type=str(raw.get("control_type", "")),
    )
    return info if info.useful() else None


def step_from_dict(raw: dict[str, Any]) -> Step:
    kind = str(raw.get("type", "")).lower()
    step = Step(type=kind, window=_window_from_dict(raw.get("window")), dt_ms=int(raw.get("dt_ms", 0) or 0))
    if kind == "click":
        step.button = "right" if str(raw.get("button", "left")).lower().startswith("r") else "left"
        step.rel_x = int(raw.get("rel_x", 0) or 0)
        step.rel_y = int(raw.get("rel_y", 0) or 0)
        if "nx" in raw and "ny" in raw:
            step.nx = float(raw.get("nx", -1) if raw.get("nx") is not None else -1)
            step.ny = float(raw.get("ny", -1) if raw.get("ny") is not None else -1)
        elif step.window.width > 0 and step.window.height > 0:
            step.nx = step.rel_x / float(step.window.width)
            step.ny = step.rel_y / float(step.window.height)
        step.uia = _uia_from_dict(raw.get("uia"))
    elif kind == "key":
        mods = raw.get("modifiers") or {}
        step.vk = int(raw.get("vk", 0) or 0)
        step.key = str(raw.get("key", ""))
        step.ctrl = bool(mods.get("ctrl", raw.get("ctrl", False)))
        step.shift = bool(mods.get("shift", raw.get("shift", False)))
        step.alt = bool(mods.get("alt", raw.get("alt", False)))
        step.win = bool(mods.get("win", raw.get("win", False)))
    else:
        raise ValueError(f"Unknown step type: {kind!r}")
    return step


def load_macro(path: Path) -> MacroDocument:
    with path.open("r", encoding="utf-8") as fh:
        raw = json.load(fh)
    if not isinstance(raw, dict):
        raise ValueError("macro root must be a JSON object")
    version = int(raw.get("version", 1))
    if version > MACRO_FORMAT_VERSION:
        raise ValueError(f"Unsupported macro version {version} (max {MACRO_FORMAT_VERSION})")
    doc = MacroDocument(
        version=version,
        created_at=str(raw.get("created_at", "")),
        dpi_awareness=str(raw.get("dpi_awareness", "")),
    )
    for item in raw.get("steps") or []:
        doc.steps.append(step_from_dict(item))
    return doc


def save_macro(path: Path, doc: MacroDocument) -> None:
    if not doc.created_at:
        doc.created_at = datetime.now(timezone.utc).isoformat()
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = doc.to_dict()
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=2)
        fh.write("\n")


def empty_document(dpi_awareness: str = "") -> MacroDocument:
    return MacroDocument(
        version=MACRO_FORMAT_VERSION,
        created_at=datetime.now(timezone.utc).isoformat(),
        dpi_awareness=dpi_awareness,
        steps=[],
    )


def format_list(doc: MacroDocument) -> str:
    lines = [
        f"macro version={doc.version} steps={len(doc.steps)} dpi={doc.dpi_awareness or '?'}",
        f"created_at={doc.created_at or '?'}",
        "",
    ]
    for i, step in enumerate(doc.steps, start=1):
        lines.append(f"{i:3d}. {step.summary()}")
    return "\n".join(lines)
