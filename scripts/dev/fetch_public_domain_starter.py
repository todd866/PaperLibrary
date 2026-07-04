#!/usr/bin/env python3
"""Download the public-domain starter pack described by manifest.json."""

from __future__ import annotations

import datetime as _dt
import json
import pathlib
import sys
import urllib.error
import urllib.request


def fetch(url: str, target: pathlib.Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "PaperLibrary starter pack installer"})
    with urllib.request.urlopen(request, timeout=60) as response:
        target.write_bytes(response.read())


def catalog_record(item: dict, epub_path: pathlib.Path, root: pathlib.Path) -> dict:
    return {
        "slug": item["slug"],
        "title": item["title"],
        "authors": item["authors"],
        "year": item["year"],
        "source": item["source"],
        "source_id": item["source_id"],
        "source_url": item["source_landing_url"],
        "rights": item["rights"],
        "format": "epub",
        "epub_path": str(epub_path.relative_to(root)),
        "tags": item.get("tags", []),
        "added_ts": _dt.datetime.now(_dt.UTC).isoformat(),
    }


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: fetch_public_domain_starter.py MANIFEST TARGET_DIR", file=sys.stderr)
        return 2

    manifest_path = pathlib.Path(sys.argv[1])
    root = pathlib.Path(sys.argv[2]).expanduser().resolve()
    books_dir = root / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    records = []
    failures = []

    for item in manifest["items"]:
        epub_path = books_dir / f"{item['slug']}.epub"
        if not epub_path.exists():
            print(f"fetch {item['source_id']}: {item['title']}")
            try:
                fetch(item["download_url"], epub_path)
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                failures.append((item["slug"], str(exc)))
                continue
        else:
            print(f"keep  {item['source_id']}: {item['title']}")
        records.append(catalog_record(item, epub_path, root))

    catalog_path = root / "catalog.jsonl"
    with catalog_path.open("w", encoding="utf-8") as catalog:
        for record in records:
            catalog.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")

    readme_path = root / "README.md"
    readme_path.write_text(
        "# PaperLibrary Public Domain Starter Pack\n\n"
        "Generated from the PaperLibrary starter-pack manifest.\n\n"
        f"Items installed: {len(records)}\n"
        f"Catalog: `{catalog_path.name}`\n"
        "Rights: Project Gutenberg records these works as public domain in the USA; verify status for other jurisdictions.\n",
        encoding="utf-8",
    )

    if failures:
        print("\nSome downloads failed:", file=sys.stderr)
        for slug, error in failures:
            print(f"- {slug}: {error}", file=sys.stderr)
        return 1

    print(f"\nInstalled {len(records)} items into {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
