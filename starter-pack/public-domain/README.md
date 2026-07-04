# Public Domain Starter Pack

This directory defines a starter pack for PaperLibrary without committing book
binaries to the source tree.

The pack is a manifest of public-domain-in-the-USA works from Project Gutenberg.
The fetch script downloads EPUB files into a local PaperLibrary-style seed
directory and writes a `catalog.jsonl` describing what was installed. It is
intended for demos, onboarding, and release packaging experiments.

Why a manifest instead of vendored files:

- keeps the source repository small;
- lets the installer fetch current Project Gutenberg EPUBs;
- avoids implying Project Gutenberg endorses PaperLibrary;
- makes the US-public-domain basis explicit for every item.

Project Gutenberg notes that most of its ebooks are public domain in the United
States, and that public-domain status can differ outside the United States:
https://www.gutenberg.org/policy/permission.html

Run from the repo root:

```bash
scripts/dev/fetch-public-domain-starter.sh
```

By default this writes to:

```text
~/Projects/PaperLibrary/starter-public-domain
```

Set `PAPERLIBRARY_STARTER_DIR` or pass a target directory to choose another
location:

```bash
scripts/dev/fetch-public-domain-starter.sh /path/to/starter-public-domain
```

Current limitation: PaperLibrary can open these EPUBs directly, but it does
not yet auto-register a starter directory into the Books shelf. The next app
step is a first-run "Install Starter Pack" action that downloads this manifest
and records the EPUBs in `LibraryStore`.
