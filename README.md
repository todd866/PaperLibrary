# PaperLibrary

PaperLibrary is a macOS-first PDF/EPUB reader and local document-library front
end. It is built for people who want a fast reading app, tile-first library
browsing, local metadata, and optional bring-your-own-token AI workflows without
sending their library to a hosted service by default.

This project is derived from KDE Okular and remains GPL-2.0-or-later. The active
application has been trimmed to a PaperLibrary-owned shell: no legacy viewer
plugin, no Okular core library, no document-generator stack, no upstream
mobile/manual/translation tree, and no old Okular config identity.

## Current Shape

- Chrome-style tabs and toolbar in the macOS titlebar.
- Shell-owned EPUB reader using QtWebEngine.
- Shell-owned PDF reader using QtPdf/PDFium.
- Library shelves for local PDFs/EPUBs and catalog-backed corpus rows.
- Tile-first browsing with generated covers, metadata, reading progress, and
  downranking.
- Optional local `claude` CLI hooks for metadata and semantic navigation.

AI features are BYO-token and local-process based. Auto-tagging is opt-in. The
app has no bundled API keys and does not include a hosted AI dependency.

## Build

The current supported development build uses KDE Craft on macOS.

```bash
source ~/CraftRoot/craft/craftenv.sh
cmake -S "$PWD" -B "$PWD/build" -DCMAKE_PREFIX_PATH="$HOME/CraftRoot" -DBUILD_TESTING=ON
cmake --build "$PWD/build" --target paperlibrary
```

The visible app bundle is `build/bin/PaperLibrary.app`.

## Run

Use the wrapper during development so Qt/KF libraries and QtWebEngine runtime
paths are present:

```bash
scripts/dev/run-paperlibrary.sh [file]
```

Opening the `.app` directly from Finder is not equivalent during development
because LaunchServices does not preserve the shell environment.

## Tests

```bash
ctest --test-dir build --output-on-failure -R '^(shelltest|librarystoretest|libraryautotaggertest|paperlibrarymodeltest|coverheuristictest|covergeneratortest|epubcovertest|epubimportertest|epubreadercorpustest|chromestriptest)$'
```

## Public Domain Starter Pack

The starter-pack manifest lives in:

```text
starter-pack/public-domain/
```

Install it into a local seed directory with:

```bash
scripts/dev/fetch-public-domain-starter.sh
```

This downloads Project Gutenberg EPUBs into
`~/Projects/PaperLibrary/starter-public-domain` by default and writes a
`catalog.jsonl` for future import/UI wiring. It does not commit book binaries to
this repository.

Project Gutenberg records these starter-pack works as public domain in the USA.
Public-domain status differs by jurisdiction, so release packaging keeps source
URLs and rights notes with every item.

## Roadmap

- Private web/PWA companion: a Vercel-hosted, authenticated PaperLibrary web
  reader for phones and tablets, backed by a sanitized sync manifest,
  reading-position sync, metadata/thumbnails/navigation maps, and private
  signed document access where the user has the right to store the file.
- E-ink companion: an experimental reader/sync client for owned e-ink devices,
  including jailbroken Kindle hardware, focused on PaperLibrary metadata,
  reading state, and local documents. This is not a DRM-circumvention goal.
- Sync-first architecture: sync metadata, shelves, navigation, annotations,
  progress, and user signals before attempting full document streaming.

## Public Release Rules

The product rules for public shelves, focus manifests, BYO-token AI behavior,
privacy, performance, and release tests live in:

```text
docs/public-release-rules.md
```

The current readiness audit against those rules lives in:

```text
docs/public-release-audit.md
```

## Attribution

PaperLibrary is based on KDE Okular:

https://invent.kde.org/graphics/okular.git

Upstream Okular copyright and GPL attribution are preserved in the app about
data and license notices.
