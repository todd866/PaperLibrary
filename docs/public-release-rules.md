<!--
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
-->

# Public Release Rules

These are the product rules for turning the current personal PaperLibrary into a
public release. The public app should preserve the architecture and philosophy,
but must not ship Ian-specific reading goals, private paths, private corpora, or
medical project assumptions as defaults.

## Core Rule

PaperLibrary is not a static taxonomy of documents. It is a local reader plus a
set of contracts that let a user's own agent turn a messy personal corpus into
useful shelves, tiles, navigation maps, and reading queues.

The public release ships the mechanism. The user's local corpus and agent supply
the taste, goals, and priorities.

## What Ships Publicly

- The reader: PDF, EPUB, tabs, outlines, search, navigation, reading position,
  local reading-history events, and local state.
- The library UI: tile-first browsing, keyboard navigation, search, downranking,
  generated cards, metadata captions, and focus shelves.
- Documented file contracts: catalog rows, focus manifests, cover metadata,
  navigation maps, and future evidence artifacts.
- Starter-pack metadata and downloader scripts for public-domain books.
- Tests that prove the public behavior is useful without a private corpus.

## What Must Stay Local

- Personal corpus data, downloaded PDFs/EPUBs, generated catalogs, local
  databases, local paths, reading-history logs, and app caches.
- Private prompts, private scoring scripts, acquisition machinery, and project
  notes that reveal sensitive reading/work context.
- Any hard-coded "Ian" shelves or goals. Medicine/MND/Work can exist as examples
  in a private local manifest, but the public release needs generic rules.

## Shelf Semantics

Shelves should be intents, not crude file types.

- `Keep reading`: long-form reading in progress — books/EPUBs started but
  unfinished (or opened more than once), newest-read first; excludes one-off
  ad-hoc PDFs. This is the default landing tab.
- `History`: everything the user actually opened recently.
- `Books`: current long-form EPUB reading, not every book-shaped file.
- `Fiction`: active fiction and series reading.
- `Non-fiction`: biography, history, social theory, essays, and other long-form
  nonfiction, including PDFs when they are book-like.
- `Papers`: research papers arranged by project, topic, source, year, or journal.
- `Work`: current active projects supplied by a focus manifest.
- Domain shelves: user-defined focus shelves such as Medicine, Law, Finance,
  Literature, or a thesis/project name.

File type is metadata. It is not the main organizing principle.

## Focus Manifests

A focus manifest is the public mechanism for agent-curated shelves. It is a
plain JSON array under `focus/<ShelfName>/manifest.json`. Each item should
carry enough context for PaperLibrary to render a useful tile without needing
to inspect private source text at runtime.

The versioned public contract lives in
`docs/contracts/focus-manifest-v1.md`.

Recommended fields:

- `id`: stable slug, DOI-derived id, file hash, or local-file id.
- `title`: display title.
- `path`: local file path, relative path, or resolved catalog path.
- `kind`: `pdf`, `epub`, `book`, `paper`, `note`, `draft`, or similar.
- `authors`, `year`, `journal`, `source`, `doi`: bibliographic context.
- `score`: optional ranking signal from the agent or backend.
- `reason`: short human-readable reason, split with semicolons when useful.
- `section`: stable numbered section such as `00-current`, `01-background`.
- `focus_link`: optional generated link inside the focus folder.

The app can fall back to catalog heuristics, but focus manifests are the
preferred interface for serious user goals.

## Ranking Rules

- Current, pinned, recently opened, or explicitly curated items outrank broad
  keyword matches.
- Numbered focus-manifest sections outrank raw score when the shelf is a work or
  project shelf. Raw score can order items inside a section.
- Broad topic classifiers are fallback behavior only.
- Every surfaced item should have an explainable reason visible through tile
  metadata, tooltip, or detail view.
- Downranking should suppress an item from the feed without deleting it from the
  library.

## Tile Rules

The public release must remain tile-first.

- No top-level shelf should degrade to a list of bare titles.
- Every tile needs a title, kind, useful caption, hover/detail metadata, and a
  cover or generated card.
- Generated cards are acceptable defaults. They should encode title, kind,
  topic/intent, and relation, not decorative filler.
- Real covers should be kept when visually meaningful. First-page manuscript
  renders should be replaced by generated cards.
- Double-click opens. Single-click selects. Arrow keys move selection. Enter
  opens. Context menus and downranking must work from selection.

## AI Rules

- AI is optional, local-process, BYO-token, and user-controlled.
- The app must be useful with AI unavailable.
- AI-generated metadata should be persisted as inspectable artifacts, not hidden
  state.
- Never persist raw source-document text into navigation or metadata caches
  unless the user explicitly requests it.
- The app should expose seams for agents to write manifests and metadata; it
  should not require a hosted AI service.

## Privacy And Security Rules

- Local files stay local by default.
- Public code must not contain private corpus paths, API keys, generated
  catalogs, user databases, or private prompts.
- Backends integrate through documented files, not imported source code.
- All subprocess calls need explicit arguments, timeouts, failure handling, and
  clear opt-in.
- Release examples must use public-domain or synthetic data.

## Performance Rules

PaperLibrary is mostly pictures and text. Shelf switching should feel instant.

- Build row caches ahead of first interaction where possible.
- Do not block shelf switching on thumbnail generation, AI, database scans, or
  content search.
- Render useful generated cards immediately while real covers warm in the
  background.
- Cache by stable design/version keys so visual changes can invalidate old
  artifacts safely.
- Coalesce rapid tab/shelf switching, but do not add visible latency to normal
  clicks.

## Test Rules

Public tests should encode behavior, not personal taste.

- Use synthetic fixture records for classifiers and ranking tests.
- Use starter-pack/public-domain fixtures for end-to-end public demos.
- Test that focus manifests drive shelves for generic Reading and Work cases.
- Test that broad classifiers do not produce known bad false positives, such as
  `nonfiction` matching Fiction.
- Test section ordering, downranking, missing-file behavior, generated cover
  fallback, and keyboard/open interactions.
- For release gates, verify disabled-AI behavior as carefully as enabled-AI
  behavior.

## Public Defaults

The clean public release should start with generic shelves plus starter-pack
content. User-specific shelves should appear only after a local manifest creates
them or the user configures them.

Recommended public defaults:

- Keep reading
- History
- Books
- Finished
- Fiction
- Non-fiction
- Work
- Starter Pack

Textbooks and Papers appear only when a corpus is configured; Medicine, MND and
other domain shelves appear only when the corpus provides a matching focus
manifest.

Domain shelves can be added later through focus manifests and user config.
