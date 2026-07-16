# Focus Manifest v1

Focus manifests are the public contract for agent-curated shelves.

PaperLibrary reads a manifest from:

```text
<corpus>/focus/<ShelfName>/manifest.json
```

The file is a JSON array. Each object describes one tile the shelf should
surface. The app treats these files as local, user-owned metadata. It never
requires a hosted AI service to create them.

## Shelf Creation

The public app starts with generic shelves:

- Keep reading
- History
- Books
- Finished
- Fiction
- Non-fiction
- Work
- Starter Pack
- Textbooks and Papers, when a corpus exists

Textbooks is a corpus smart shelf (like Fiction / Non-fiction), shown whenever a
Papers corpus is configured. Other domain shelves appear only when the corpus
contains a matching focus manifest. For example:

```text
focus/Medicine/manifest.json
focus/MND/manifest.json
```

This keeps the public app generic while allowing local libraries to grow
specialized shelves.

## Fields

Recommended object fields:

- `id`: stable slug or local id.
- `doi`: DOI when known.
- `title`: display title.
- `path`: local absolute path, corpus-relative path, or a path resolvable from
  the catalog.
- `kind`: `pdf`, `epub`, `book`, `paper`, `note`, `draft`, `guideline`, or
  another short local kind.
- `authors`: compact author string.
- `year`: publication year or local document year.
- `journal`: journal, publisher, venue, or `(book)`.
- `source`: source label such as `book:epub`, `project`, `review`, or
  `starter-pack`.
- `section`: sortable section id such as `00-current`, `01-background`, or
  `20-adjacent`.
- `score`: optional numeric ranking signal.
- `reason`: user-facing reason, with semicolon-separated clauses when useful.
- `focus_link`: optional path or URL to a local note, map, or generated
  evidence file.

Only `title` plus one resolver field (`id`, `doi`, or `path`) is required for a
useful tile. More fields make better metadata, search, sorting, and tooltips.

## Ordering

For project/domain shelves, numbered `section` prefixes are the main ordering
mechanism. Within a section, the manifest order is preserved unless the app has
a stronger explicit signal, such as a pinned/continue-reading item.

Recommended section pattern:

```text
00-current
01-core
02-background
03-methods
10-adjacent
90-archive
```

## Reasons

`reason` should explain why the tile is here, not merely repeat the title.

Good:

```json
"reason": "Current project anchor; defines diagnostic framing"
```

Weak:

```json
"reason": "MND paper"
```

The first semicolon-delimited clause is used as the primary tile intent. Later
clauses become secondary relation/detail text.

## Example

```json
[
  {
    "id": "paper-001",
    "doi": "10.0000/example.1",
    "title": "A Synthetic Review of Example Biomarkers",
    "path": "pdfs/paper-001.pdf",
    "kind": "paper",
    "authors": "Casey Researcher",
    "year": "2026",
    "journal": "Journal of Examples",
    "source": "project-reading-list",
    "section": "00-current",
    "score": 0.94,
    "reason": "Current project anchor; connects biomarker evidence to methods",
    "focus_link": "notes/paper-001.md"
  }
]
```

## Privacy

Manifests should contain metadata and short explanations, not full source text.
Do not write private document excerpts, API keys, tokens, raw prompts, or
unreviewed local paths into manifests intended for public examples.

