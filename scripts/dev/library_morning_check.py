#!/usr/bin/env python3
"""Audit the visible PaperLibrary shelf payload before opening the app.

This is a lightweight "morning check" for the library surface.  It reads the
same local corpus files that drive the main shelves, builds the first-screen tile
payload for each tab, and reports metadata that would be useless or misleading
on screen.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sqlite3
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CORPUS = Path(os.environ.get("PAPERLIBRARY_CORPUS", str(Path.home() / "PaperLibrary")))
DEFAULT_REPORT_DIR = REPO_ROOT / "build" / "reports"

SHELVES = (
    "Recent",
    "Books",
    "Finished",
    "Fiction",
    "Non-fiction",
    "Work",
    "Starter Pack",
    "Medicine",
    "MND",
    "Papers",
)

GENERIC_TITLES = {
    "document",
    "file",
    "paper",
    "paper anonymous",
    "pdf",
    "untitled",
    "unknown",
    "unknown title",
    "beyond bayesian",
    "beyond bayesian anonymous",
}

GENERIC_METADATA = {
    "",
    "book",
    "books",
    "paper",
    "pdf",
    "epub",
    "work",
    "unknown",
}

BROKEN_CITATION_RE = re.compile(r"^(?:al\.?|et\s+al\.?|unknown|anon(?:ymous)?|tw)\s+(?:19|20)\d{2}$", re.I)

BOOKISH_KINDS = {"book", "epub", "mobi", "azw", "azw3"}
PAPERISH_KINDS = {"paper", "pdf", "article"}


@dataclass
class Record:
    key: str
    title: str = ""
    authors: str = ""
    year: str = ""
    journal: str = ""
    kind: str = ""
    source: str = ""
    path: str = ""
    doi: str = ""
    reason: str = ""
    section: str = ""
    shelf: str = ""
    score: float = 0.0
    added_ts: str = ""
    last_accessed: str = ""
    access_count: int = 0
    pinned: int = 0
    cited_by_count: int = 0
    pdf_evicted: int = 0
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass
class Tile:
    shelf: str
    source: str
    record_source: str
    key: str
    display_title: str
    subtitle: str
    metadata_line: str
    topic: str
    kind: str
    authors: str
    year: str
    journal: str
    path: str
    reason: str
    issues: list[dict[str, str]] = field(default_factory=list)


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()


def clean_space(value: Any) -> str:
    return re.sub(r"\s+", " ", str(value or "").replace("\u00a0", " ")).strip()


def normal_key(value: str) -> str:
    value = clean_space(value).lower()
    value = re.sub(r"['`]", "", value)
    value = re.sub(r"[^a-z0-9]+", " ", value)
    return clean_space(value)


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                payload = json.loads(line)
            except json.JSONDecodeError as exc:
                rows.append({"_parse_error": f"{path}:{line_no}: {exc}"})
                continue
            if isinstance(payload, dict):
                rows.append(payload)
    return rows


def read_json_array(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [{"_parse_error": f"{path}: {exc}"}]
    if isinstance(payload, list):
        return [row for row in payload if isinstance(row, dict)]
    return [{"_parse_error": f"{path}: expected JSON array"}]


def record_from_payload(payload: dict[str, Any], fallback_key: str = "") -> Record:
    key = clean_space(payload.get("slug") or payload.get("id") or payload.get("doi") or fallback_key)
    title = clean_space(payload.get("title"))
    path = clean_space(payload.get("path") or payload.get("pdf_path"))
    source = clean_space(payload.get("source"))
    kind = clean_space(payload.get("kind")).lower()
    if not kind:
        suffix = Path(path).suffix.lower().lstrip(".")
        if suffix:
            kind = suffix
        elif "pdf" in source.lower():
            kind = "pdf"
        elif "book" in source.lower():
            kind = "book"
    if not key:
        key = path or title or "<missing-key>"
    return Record(
        key=key,
        title=title,
        authors=clean_space(payload.get("authors")),
        year=clean_space(payload.get("year")),
        journal=clean_space(payload.get("journal")),
        kind=kind,
        source=source,
        path=path,
        doi=clean_space(payload.get("doi")),
        reason=clean_space(payload.get("reason")),
        section=clean_space(payload.get("section") or payload.get("section_label")),
        shelf=clean_space(payload.get("shelf")),
        score=float(payload.get("score") or 0.0),
        added_ts=clean_space(payload.get("added_ts")),
        last_accessed=clean_space(payload.get("last_accessed")),
        access_count=int(payload.get("access_count") or 0),
        pinned=int(payload.get("pinned") or 0),
        cited_by_count=int(payload.get("cited_by_count") or 0),
        pdf_evicted=int(payload.get("pdf_evicted") or 0),
        raw=dict(payload),
    )


def load_catalog_records(corpus: Path) -> dict[str, Record]:
    records: dict[str, Record] = {}
    path_index: dict[str, str] = {}
    doi_index: dict[str, str] = {}

    for payload in read_jsonl(corpus / "catalog.jsonl"):
        record = record_from_payload(payload)
        records[record.key] = record
        if record.path:
            path_index[os.path.realpath(record.path)] = record.key
        if record.doi:
            doi_index[record.doi.lower()] = record.key

    db_path = corpus / "catalog.db"
    if not db_path.exists():
        return records

    try:
        with sqlite3.connect(str(db_path)) as con:
            con.row_factory = sqlite3.Row
            cols = [row[1] for row in con.execute("PRAGMA table_info(papers)").fetchall()]
            if "slug" not in cols:
                return records
            select_cols = ", ".join(cols)
            for row in con.execute(f"SELECT {select_cols} FROM papers"):
                payload = {col: row[col] for col in cols}
                record = record_from_payload(payload)
                existing = records.get(record.key)
                if existing:
                    merge_record(existing, record)
                    record = existing
                else:
                    records[record.key] = record
                if record.path:
                    path_index[os.path.realpath(record.path)] = record.key
                if record.doi:
                    doi_index[record.doi.lower()] = record.key
    except sqlite3.Error as exc:
        records[f"<db-error:{db_path}>"] = Record(key=f"<db-error:{db_path}>", title=str(exc), kind="error")

    return records


def merge_record(base: Record, update: Record) -> None:
    for field_name in (
        "title",
        "authors",
        "year",
        "journal",
        "kind",
        "source",
        "path",
        "doi",
        "reason",
        "section",
        "shelf",
        "added_ts",
        "last_accessed",
    ):
        value = getattr(update, field_name)
        if value and not getattr(base, field_name):
            setattr(base, field_name, value)
    for field_name in ("score", "access_count", "pinned", "cited_by_count", "pdf_evicted"):
        value = getattr(update, field_name)
        if value:
            setattr(base, field_name, value)
    base.raw.update({key: value for key, value in update.raw.items() if value not in ("", None)})


def overlay_record(base: Record, update: Record) -> None:
    for field_name in (
        "title",
        "authors",
        "year",
        "journal",
        "kind",
        "source",
        "path",
        "doi",
        "reason",
        "section",
        "shelf",
        "added_ts",
        "last_accessed",
    ):
        value = getattr(update, field_name)
        if value:
            setattr(base, field_name, value)
    for field_name in ("score", "access_count", "pinned", "cited_by_count", "pdf_evicted"):
        value = getattr(update, field_name)
        if value:
            setattr(base, field_name, value)
    base.raw.update({key: value for key, value in update.raw.items() if value not in ("", None)})


def build_indexes(records: dict[str, Record]) -> tuple[dict[str, str], dict[str, str]]:
    path_index: dict[str, str] = {}
    doi_index: dict[str, str] = {}
    for key, record in records.items():
        if record.path:
            path_index[os.path.realpath(record.path)] = key
        if record.doi:
            doi_index[record.doi.lower()] = key
    return path_index, doi_index


def load_focus_records(corpus: Path, shelf: str, records: dict[str, Record]) -> list[Record]:
    manifest_path = corpus / "focus" / shelf / "manifest.json"
    payloads = read_json_array(manifest_path)
    path_index, doi_index = build_indexes(records)
    out: list[Record] = []
    for pos, payload in enumerate(payloads):
        if "_parse_error" in payload:
            out.append(record_from_payload(payload, f"{shelf}:parse-error:{pos}"))
            continue
        manifest_record = record_from_payload(payload, f"{shelf}:{pos}")
        match_key = ""
        if manifest_record.key in records:
            match_key = manifest_record.key
        elif manifest_record.doi and manifest_record.doi.lower() in doi_index:
            match_key = doi_index[manifest_record.doi.lower()]
        elif manifest_record.path and os.path.realpath(manifest_record.path) in path_index:
            match_key = path_index[os.path.realpath(manifest_record.path)]

        if match_key:
            record = Record(**vars(records[match_key]))
            overlay_record(record, manifest_record)
        else:
            record = manifest_record
        record.shelf = shelf
        out.append(record)
    return out


def starter_pack_records() -> list[Record]:
    installed = REPO_ROOT / "starter-pack" / "public-domain" / "catalog.jsonl"
    rows = [record_from_payload(row, f"starter:{idx}") for idx, row in enumerate(read_jsonl(installed))]
    if rows:
        for record in rows:
            record.shelf = "Starter Pack"
            if not record.reason:
                record.reason = "Public-domain starter pack"
        return rows
    return [
        Record(
            key="starter-pack-setup",
            title="Install the public-domain starter pack",
            kind="setup",
            source="setup",
            reason="Run scripts/dev/fetch-public-domain-starter.sh to populate starter-pack/public-domain/catalog.jsonl",
            shelf="Starter Pack",
            section="Setup",
        )
    ]


def split_authors(authors: str) -> list[str]:
    parts = re.split(r"\s*;\s*|\s+\band\b\s+|\s*,\s+(?=[A-Z][a-z]+(?:\s|$))", authors)
    return [clean_space(part) for part in parts if clean_space(part)]


def author_surname(authors: str) -> str:
    parts = split_authors(authors)
    if not parts:
        return ""
    first = parts[0]
    first = re.sub(r"\bet\s+al\.?$", "", first, flags=re.I).strip()
    first = re.sub(r"\bal\.?$", "", first, flags=re.I).strip()
    if not first:
        return ""
    if "," in first:
        return clean_space(first.split(",", 1)[0])
    tokens = [tok for tok in re.split(r"\s+", first) if tok]
    if len(tokens) >= 2 and re.fullmatch(r"[A-Z]{1,4}\.?", tokens[-1]):
        return clean_space(" ".join(tokens[:-1]))
    return tokens[-1] if tokens else first


def citation_label(record: Record) -> str:
    surname = author_surname(record.authors)
    year = extract_year(record)
    if not surname or not year:
        return ""
    suffix = " et al." if len(split_authors(record.authors)) > 1 or re.search(r"\bet\s+al\.?\b", record.authors, re.I) else ""
    return f"{surname}{suffix} {year}"


def extract_year(record: Record) -> str:
    if re.fullmatch(r"\d{4}", record.year or ""):
        return record.year
    haystack = " ".join([record.title, record.source, record.path])
    match = re.search(r"\b(19|20)\d{2}\b", haystack)
    return match.group(0) if match else ""


def looks_paperish(record: Record) -> bool:
    source = record.source.lower()
    journal = record.journal.lower()
    if source.startswith("book:") or "book:" in source or source == "aa_book" or journal == "(book)":
        return False
    if record.doi or record.journal:
        return True
    if record.kind in {"paper", "article"}:
        return True
    if record.kind == "pdf":
        return any(token in source for token in ("scihub", "harvest", "unpaywall", "doi", "journal", "article"))
    return False


def looks_bookish(record: Record) -> bool:
    text = " ".join([record.kind, record.source, record.path]).lower()
    return any(token in text for token in ("book", "epub", "mobi", "kindle", "azw"))


def curated_display_title(text: str) -> str:
    lower = clean_space(text).lower()
    bookish = any(token in lower for token in ("book:", "aa_book", "(book)", ".epub", ".azw3", ".mobi", "anna", "isbn", "bantam books"))
    if "game of thrones" in lower and bookish:
        return "A Game of Thrones"
    if "years of rice and salt" in lower:
        return "The Years of Rice and Salt"
    if "green mars" in lower:
        return "Green Mars"
    if "blue mars" in lower:
        return "Blue Mars"
    if "red mars" in lower:
        return "Red Mars"
    if "new york 2140" in lower:
        return "New York 2140"
    if "antarctica" in lower and ("novel" in lower or "kim stanley robinson" in lower):
        return "Antarctica"
    if "best of kim stanley robinson" in lower:
        return "The Best of Kim Stanley Robinson"
    if "one day in the life of ivan denisovich" in lower:
        return "One Day in the Life of Ivan Denisovich"
    if "ones who walk away from omelas" in lower:
        return "The Ones Who Walk Away from Omelas"
    if "dune, dune messiah" in lower or ("dune messiah" in lower and "children of dune" in lower):
        return "Dune / Dune Messiah / Children of Dune"
    if re.search(r"\bdune\b", lower) and bookish:
        return "Dune"
    if "water knife" in lower:
        return "The Water Knife"
    if "windup girl" in lower:
        return "The Windup Girl"
    if "carpentaria" in lower:
        return "Carpentaria"
    if "books of earthsea" in lower:
        return "The Books of Earthsea"
    if "parable of the sower" in lower:
        return "Parable of the Sower"
    return ""


KNOWN_FICTION_PATTERNS = (
    "game of thrones",
    "song of ice and fire",
    "george r. r. martin",
    "george rr martin",
    "frank herbert",
    "dune messiah",
    "children of dune",
    "god emperor of dune",
    "kim stanley robinson",
    "years of rice and salt",
    "green mars",
    "blue mars",
    "red mars",
    "new york 2140",
    "mars trilogy",
    "octavia butler",
    "parable of the sower",
    "ursula k. le guin",
    "ursula k le guin",
    "earthsea",
    "the dispossessed",
    "omelas",
    "alexis wright",
    "carpentaria",
    "praiseworthy",
    "the hobbit",
    "lord of the rings",
    "the silmarillion",
    "unfinished tales",
    "one day in the life of ivan denisovich",
    "the water knife",
    "the windup girl",
)


def is_fiction_record(record: Record) -> bool:
    text = " ".join([record.section, record.reason, record.title, record.authors, record.journal, record.source, record.kind, record.path]).lower()
    if any(token in text for token in ("nonfiction", "non-fiction", "non fiction")):
        return False
    if any(pattern in text for pattern in KNOWN_FICTION_PATTERNS):
        return True
    if looks_bookish(record) and re.search(r"\bdune\b", text):
        return True
    if not looks_bookish(record):
        return False
    return (
        re.search(r"\bfiction\b", text) is not None
        or re.search(r"\bfantasy\b", text) is not None
        or re.search(r"\bnovel\b", text) is not None
        or "speculative fiction" in text
        or "science fiction" in text
    )


def clean_title(title: str) -> str:
    title = clean_space(title)
    curated = curated_display_title(title)
    if curated:
        return curated
    title = re.sub(r"\s+[0-9a-f]{8,32}$", "", title, flags=re.I)
    title = re.sub(r"\s+Anna.?s Archive$", "", title, flags=re.I)
    title = re.sub(r"\bisbn(?:-?1[03])?\s*[0-9xX -]{10,20}\b.*$", "", title, flags=re.I)
    title = re.sub(r"\b[0-9a-f]{24,32}\b.*$", "", title, flags=re.I)
    title = re.sub(r"\s*\((?:FSG|A Wind's Twelve).*$", "", title, flags=re.I)
    title = title.replace("_", " ")
    title = re.sub(r"\s+", " ", title)
    title = title.strip(" -_")
    curated = curated_display_title(title)
    return curated or title


def topic_from_record(record: Record, fallback: str = "") -> str:
    text = " ".join([record.section, record.reason, record.title, record.journal, record.source]).lower()
    if any(token in text for token in ("beyond bayes", "high-dimensional", "high dimensional", "fep", "free energy")):
        return "Beyond Bayes"
    if any(token in text for token in ("coherence dynamics", "entropy convergence", "thermodynamics/information")):
        return "Coherence dynamics"
    if any(token in text for token in ("neural dynamics", "oscillatory", "alpha oscillation", "cortical space", "ephaptic")):
        return "Neural dynamics"
    if any(token in text for token in ("reviewer", "peer review", "revision")):
        return "Peer review"
    if any(token in text for token in ("amyotrophic", "als", "motor neuron", "mnd", "neurofilament", "cortical hyperexcit")):
        return "MND / ALS"
    if any(token in text for token in ("psychiat", "mental health", "neuropsych")):
        return "Psychiatry"
    if is_fiction_record(record):
        return "Fiction"
    if any(token in text for token in ("paed", "pediatric", "paediatric", "child")):
        return "Paediatrics"
    if any(token in text for token in ("obgyn", "obstetric", "gynaec", "gynec")):
        return "OBGYN"
    if any(token in text for token in ("medicine", "clinical", "pathology", "physiology", "pharmacology", "anatomy")):
        return "Medicine"
    if any(token in text for token in ("anthropology", "graeber", "sahlins")):
        return "Anthropology"
    if any(token in text for token in ("nonfiction", "non-fiction", "politics", "history", "biography")):
        return "Non-fiction"
    if fallback:
        return fallback
    return clean_space(record.section) or clean_space(record.shelf)


def thesis_line(record: Record) -> str:
    title = clean_title(record.title)
    if not title:
        return ""
    title = re.sub(r"^(article|paper|pdf)\s*[:.-]\s*", "", title, flags=re.I)
    title = re.sub(r"\s*\((?:p|pp?)?[\w.-]+\)\s*$", "", title, flags=re.I)
    return title


def reason_summary(reason: str) -> str:
    pieces: list[str] = []
    for raw in re.split(r"\s*;\s*", reason or ""):
        piece = clean_space(raw)
        key = piece.lower()
        if not piece:
            continue
        if key in {"md project review set", "als/mnd title or metadata", "opened before; keep warm"}:
            continue
        pieces.append(piece)
    return " / ".join(pieces[:3])


def display_title(record: Record) -> str:
    if looks_paperish(record):
        label = citation_label(record)
        if label and not BROKEN_CITATION_RE.match(label):
            return label
    return clean_title(record.title) or Path(record.path).stem


def metadata_line(record: Record, topic: str) -> str:
    pieces: list[str] = []
    if record.authors and not looks_paperish(record):
        pieces.append(record.authors)
    elif looks_paperish(record):
        pieces.append(topic)
    if extract_year(record) and not any(extract_year(record) in piece for piece in pieces):
        pieces.append(extract_year(record))
    if record.journal and looks_paperish(record):
        pieces.append(compact_journal(record.journal))
    if looks_bookish(record) and topic:
        pieces.append(topic)
    if record.kind and not any(record.kind.lower() == piece.lower() for piece in pieces):
        pieces.append(record.kind.upper() if len(record.kind) <= 4 else record.kind.title())
    return " | ".join(clean_space(piece) for piece in pieces if clean_space(piece))


def compact_journal(journal: str) -> str:
    journal = clean_space(journal)
    lowered = journal.lower()
    replacements = {
        "amyotrophic lateral sclerosis & frontotemporal degeneration": "ALS and FTD",
        "amyotrophic lateral sclerosis and frontotemporal degeneration": "ALS and FTD",
        "neurological sciences : official journal of the italian neurological society and of the italian society of clinical neurophysiology": "Neurological Sciences",
        "neurological sciences: official journal of the italian neurological society and of the italian society of clinical neurophysiology": "Neurological Sciences",
        "cliodynamics: the journal of quantitative history and cultural evolution": "Cliodynamics",
    }
    for key, value in replacements.items():
        if lowered == key:
            return value
    if ";" in journal:
        journal = journal.split(";", 1)[0]
    return journal


def build_tile(record: Record, shelf: str, source: str) -> Tile:
    topic = topic_from_record(record, shelf)
    title = display_title(record)
    if looks_paperish(record):
        subtitle = reason_summary(record.reason) or thesis_line(record)
    else:
        subtitle = topic
    if looks_bookish(record) and record.authors:
        subtitle = record.authors
    return Tile(
        shelf=shelf,
        source=source,
        record_source=record.source,
        key=record.key,
        display_title=title,
        subtitle=subtitle,
        metadata_line=metadata_line(record, topic),
        topic=topic,
        kind=record.kind or ("paper" if looks_paperish(record) else "item"),
        authors=record.authors,
        year=extract_year(record),
        journal=record.journal,
        path=record.path,
        reason=record.reason,
    )


def severity(tile: Tile, level: str, message: str, field: str = "") -> None:
    tile.issues.append({"severity": level, "field": field, "message": message})


def is_filenameish(value: str) -> bool:
    candidate = clean_space(value)
    lowered = candidate.lower()
    if re.search(r"\.(pdf|epub|mobi|azw3?)$", lowered):
        return True
    if re.search(r"\bmd5[-_][0-9a-f]{8,}\b", lowered):
        return True
    if re.search(r"\b[0-9a-f]{16,}\b", lowered):
        return True
    if re.match(r"^10[-_.]\d{4}[-_.]", lowered):
        return True
    if "\\" in candidate:
        return True
    if candidate.startswith("/") or re.search(r"/(?:Users|Volumes|home|tmp|var|private|[^/\s]+\.(?:pdf|epub|mobi|azw3?))", candidate, re.I):
        return True
    return False


def audit_tile(tile: Tile) -> None:
    display_key = normal_key(tile.display_title)
    subtitle_key = normal_key(tile.subtitle)
    meta_key = normal_key(tile.metadata_line)

    if not tile.display_title:
        severity(tile, "fail", "visible title is empty", "display_title")
    elif display_key in GENERIC_TITLES:
        severity(tile, "fail", f"visible title is a placeholder: {tile.display_title!r}", "display_title")
    elif BROKEN_CITATION_RE.match(tile.display_title):
        severity(tile, "fail", f"visible title is a broken citation label: {tile.display_title!r}", "display_title")
    elif is_filenameish(tile.display_title):
        severity(tile, "fail", f"visible title looks like a filename or DOI slug: {tile.display_title!r}", "display_title")

    if looks_paperish_record(tile):
        if not tile.authors:
            severity(tile, "fail", "paper tile has no authors, so it cannot render an author/year label", "authors")
        if not tile.year:
            severity(tile, "fail", "paper tile has no year", "year")
        if subtitle_key in GENERIC_TITLES or subtitle_key in GENERIC_METADATA:
            severity(tile, "fail", "paper subtitle does not explain the paper", "subtitle")
        if not tile.topic or normal_key(tile.topic) in GENERIC_METADATA:
            severity(tile, "fail", "paper has no useful topic/focus label", "topic")

    if looks_bookish_tile(tile):
        if meta_key in GENERIC_METADATA:
            severity(tile, "warn", "book tile metadata is only a format label; add author/genre/shelf", "metadata_line")
        if not tile.authors and not tile.topic:
            severity(tile, "warn", "book tile lacks both author and useful topic", "authors")

    if len(tile.display_title) > 48 and not looks_paperish_record(tile):
        severity(tile, "warn", "visible title exceeds the tile budget; curate a shorter display title", "display_title")
    if len(tile.subtitle) > 96:
        severity(tile, "warn", "subtitle exceeds the tile budget; curate a one-sentence thesis/role", "subtitle")
    if len(tile.metadata_line) > 96:
        severity(tile, "warn", "metadata line exceeds the tile budget; choose the most useful fields", "metadata_line")


def looks_paperish_record(tile: Tile) -> bool:
    source = tile.record_source.lower()
    journal = tile.journal.lower()
    if source.startswith("book:") or "book:" in source or source == "aa_book" or journal == "(book)":
        return False
    if tile.journal:
        return True
    if tile.kind.lower() in {"paper", "article"}:
        return True
    if tile.kind.lower() == "pdf":
        return any(token in source for token in ("scihub", "harvest", "unpaywall", "doi", "journal", "article"))
    return False


def looks_bookish_tile(tile: Tile) -> bool:
    text = " ".join([tile.kind, tile.record_source, tile.path]).lower()
    return any(token in text for token in BOOKISH_KINDS)


def dedupe(records: list[Record]) -> list[Record]:
    out: list[Record] = []
    seen: set[str] = set()
    for record in records:
        if looks_bookish(record) and clean_title(record.title):
            key = "book-title:" + normal_key(clean_title(record.title))
        else:
            key = record.key or record.path or normal_key(record.title)
        if key in seen:
            continue
        seen.add(key)
        out.append(record)
    return out


def rank_recent(record: Record) -> tuple[str, str, str]:
    return (record.last_accessed or "", record.added_ts or "", record.key)


def rank_paper(record: Record) -> tuple[float, str, str]:
    score = record.score + record.pinned * 100 + record.access_count * 2 + min(record.cited_by_count, 200) * 0.05
    return (score, record.last_accessed or record.added_ts or "", record.key)


def records_for_shelf(shelf: str, corpus: Path, records: dict[str, Record]) -> tuple[str, list[Record]]:
    if shelf in {"Work", "MND", "Medicine"}:
        return "focus manifest", load_focus_records(corpus, shelf, records)
    if shelf == "Starter Pack":
        return "starter pack catalog", starter_pack_records()
    if shelf == "Recent":
        rows = sorted(records.values(), key=rank_recent, reverse=True)
        return "catalog.db recency", rows
    if shelf == "Papers":
        rows = [record for record in records.values() if looks_paperish(record)]
        return "catalog.db paper ranking", sorted(rows, key=rank_paper, reverse=True)
    if shelf == "Books":
        reading = load_focus_records(corpus, "Reading", records)
        rows = [record for record in reading if looks_bookish(record)]
        if rows:
            return "Reading focus manifest", rows
        return "catalog book rows", [record for record in records.values() if looks_bookish(record)]
    if shelf == "Fiction":
        reading = load_focus_records(corpus, "Reading", records)
        rows = [record for record in reading if is_fiction_record(record)]
        rows.extend(record for record in records.values() if is_fiction_record(record))
        return "Reading focus + catalog fiction books", rows
    if shelf == "Non-fiction":
        reading = load_focus_records(corpus, "Reading", records)
        rows = [record for record in reading if not is_fiction_record(record)]
        return "Reading focus manifest", rows
    if shelf == "Finished":
        rows = [record for record in load_focus_records(corpus, "Reading", records) if "finished" in " ".join([record.section, record.reason]).lower()]
        return "Reading focus manifest", rows
    return "catalog", list(records.values())


def audit_duplicates(tiles: list[Tile]) -> None:
    def duplicate_key(tile: Tile) -> str:
        return normal_key(tile.display_title) + " :: " + normal_key(tile.subtitle)

    counts = Counter(duplicate_key(tile) for tile in tiles if normal_key(tile.display_title))
    for tile in tiles:
        key = duplicate_key(tile)
        if key and counts[key] > 1:
            severity(tile, "warn", f"same visible tile appears {counts[key]} times in this first screen", "display_title")


def build_report(corpus: Path, limit: int) -> dict[str, Any]:
    records = load_catalog_records(corpus)
    shelves: list[dict[str, Any]] = []
    for shelf in SHELVES:
        source, shelf_records = records_for_shelf(shelf, corpus, records)
        tiles = [build_tile(record, shelf, source) for record in dedupe(shelf_records)[:limit]]
        for tile in tiles:
            audit_tile(tile)
        audit_duplicates(tiles)
        shelves.append(
            {
                "name": shelf,
                "source": source,
                "record_count": len(shelf_records),
                "visible_count": len(tiles),
                "tiles": [tile_to_json(tile) for tile in tiles],
            }
        )
    failures = sum(1 for shelf in shelves for tile in shelf["tiles"] for issue in tile["issues"] if issue["severity"] == "fail")
    warnings = sum(1 for shelf in shelves for tile in shelf["tiles"] for issue in tile["issues"] if issue["severity"] == "warn")
    return {
        "generated_at": now_iso(),
        "corpus": str(corpus),
        "limit": limit,
        "record_count": len(records),
        "failures": failures,
        "warnings": warnings,
        "shelves": shelves,
    }


def tile_to_json(tile: Tile) -> dict[str, Any]:
    return {
        "key": tile.key,
        "record_source": tile.record_source,
        "display_title": tile.display_title,
        "subtitle": tile.subtitle,
        "metadata_line": tile.metadata_line,
        "topic": tile.topic,
        "kind": tile.kind,
        "authors": tile.authors,
        "year": tile.year,
        "journal": tile.journal,
        "path": tile.path,
        "reason": tile.reason,
        "issues": tile.issues,
    }


def render_markdown(report: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append("# PaperLibrary Morning Check")
    lines.append("")
    lines.append(f"- Corpus: `{report['corpus']}`")
    lines.append(f"- Records loaded: {report['record_count']}")
    lines.append(f"- First-screen limit per tab: {report['limit']}")
    lines.append(f"- Failures: {report['failures']}")
    lines.append(f"- Warnings: {report['warnings']}")
    lines.append("")

    for shelf in report["shelves"]:
        fail_count = sum(1 for tile in shelf["tiles"] for issue in tile["issues"] if issue["severity"] == "fail")
        warn_count = sum(1 for tile in shelf["tiles"] for issue in tile["issues"] if issue["severity"] == "warn")
        status = "FAIL" if fail_count else ("WARN" if warn_count else "PASS")
        lines.append(f"## {shelf['name']} - {status}")
        lines.append("")
        lines.append(f"Source: {shelf['source']} | visible: {shelf['visible_count']} / records: {shelf['record_count']}")
        lines.append("")
        for idx, tile in enumerate(shelf["tiles"], 1):
            issue_prefix = "OK"
            if any(issue["severity"] == "fail" for issue in tile["issues"]):
                issue_prefix = "FAIL"
            elif tile["issues"]:
                issue_prefix = "WARN"
            lines.append(f"{idx}. **{issue_prefix}** `{tile['display_title']}`")
            if tile["subtitle"]:
                lines.append(f"   - subtitle: {tile['subtitle']}")
            if tile["metadata_line"]:
                lines.append(f"   - metadata: {tile['metadata_line']}")
            if tile["reason"]:
                lines.append(f"   - why: {tile['reason']}")
            for issue in tile["issues"]:
                lines.append(f"   - {issue['severity'].upper()}: {issue['message']}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def write_reports(report: dict[str, Any], report_dir: Path) -> tuple[Path, Path]:
    report_dir.mkdir(parents=True, exist_ok=True)
    json_path = report_dir / "library-morning-check.json"
    md_path = report_dir / "library-morning-check.md"
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    md_path.write_text(render_markdown(report), encoding="utf-8")
    return json_path, md_path


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit first-screen PaperLibrary shelf tiles.")
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS, help="PaperLibrary corpus directory")
    parser.add_argument("--limit", type=int, default=28, help="visible tile count to inspect per shelf")
    parser.add_argument("--report-dir", type=Path, default=DEFAULT_REPORT_DIR, help="directory for JSON/Markdown reports")
    parser.add_argument("--no-fail", action="store_true", help="write reports but exit 0 even when issues are found")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = build_report(args.corpus.expanduser(), max(1, args.limit))
    json_path, md_path = write_reports(report, args.report_dir)
    print(f"PaperLibrary morning check: {report['failures']} failures, {report['warnings']} warnings")
    print(f"JSON: {json_path}")
    print(f"Markdown: {md_path}")
    for shelf in report["shelves"]:
        fail_count = sum(1 for tile in shelf["tiles"] for issue in tile["issues"] if issue["severity"] == "fail")
        warn_count = sum(1 for tile in shelf["tiles"] for issue in tile["issues"] if issue["severity"] == "warn")
        if fail_count or warn_count:
            print(f"- {shelf['name']}: {fail_count} failures, {warn_count} warnings")
    return 0 if args.no_fail or report["failures"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
