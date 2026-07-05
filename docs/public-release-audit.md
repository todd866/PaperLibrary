# Public Release Audit

Date: 2026-07-05
Last updated: 2026-07-05

This audit checks the current public export against the public release rules in
`docs/public-release-rules.md`.

## Overall Verdict

PaperLibrary is publishable as an early public alpha, but it is not yet a
fully polished public product by its own rules.

The reader, privacy posture, local-first architecture, tile-first direction,
starter-pack metadata, and focus-manifest mechanism are all real. The main
release risk is narrower than before: public app chrome now starts generic, but
some fallback classifiers and labels still carry private/local assumptions that
should move behind manifests, config, examples, or private local rules.

## What Is Working

- The public repo is separated from the private working tree and does not ship
  the personal corpus, generated catalogs, downloaded books, local databases, or
  API keys.
- The README now explains the actual shape of the app: macOS-first PDF/EPUB
  reader, tile-first library, optional BYO-token local AI.
- Okular attribution and GPL lineage are preserved while the old Okular app
  identity has been removed from the active product framing.
- The library UI is substantially tile-first. It has generated cover cards,
  real cover loading, metadata roles, downranking, context menus, keyboard open
  shortcuts, and new-tab document opening from library tiles.
- Public default tabs are now generic. Domain shelves such as Medicine, MND, or
  Textbooks appear only when the configured corpus provides a matching focus
  manifest.
- Focus manifests are implemented enough to be the public abstraction for
  agent-curated shelves. `Reading`, `Work`, `MND`, and `Medicine` manifests can
  drive rows, reasons, sections, metadata, and ranking.
- Focus manifests now have a versioned public contract:
  `docs/contracts/focus-manifest-v1.md`.
- Recent model tests cover several important failures that came up in use:
  reading manifests, Work section order, Non-fiction vs Fiction, Caro not being
  Psychiatry, generated covers, and downranking.
- AI auto-tagging is opt-in by default, uses the local `claude` CLI, has tests
  for disabled/no-CLI states, and avoids sending corpus documents through the
  auto-tagging path.
- The public-domain starter pack is represented as metadata plus a downloader
  instead of committed book binaries, and an installed starter catalog renders
  into a Starter Pack shelf.

## Partial Alignment

### Mechanism Over Taste

The focus-manifest mechanism is now the right public mechanism and the public
tabs no longer hard-code `Textbooks`, `Medicine`, and `MND` as defaults.

The remaining problem is that some fallback corpus/local classifiers still
contain private-domain concepts such as `Beyond Bayes`, `MND Project`, `paeds`,
`OBGYN`, `Graeber`, `Caro`, and `Game of Thrones`.

Those are reasonable local examples, but they should move behind user config,
sample fixtures, or local manifests before a serious public release.

### Intent Shelves Over File Type

`Reading`, `Work`, and focus-manifest sections now express intent better than
raw file type. The public UI no longer exposes `Textbooks` as a default
top-level shelf. The remaining issue is that broad keyword heuristics still
exist as fallbacks; those need to become generic, sample-only, or configurable.

### Tile-First UX

The app no longer fundamentally depends on bare title lists. That is a major
improvement. The remaining work is quality: hover/detail metadata, generated
cards, empty states, and per-tile reasoning need to feel intentionally designed,
not merely present.

### Performance

The code now coalesces rapid shelf switching, prewarms corpus shelves, uses
generated cards before real covers, and avoids reloading the corpus on every
tab switch. That matches the rules in spirit.

The missing piece is proof. There is no release gate that measures first shelf
paint, rapid shelf switching, cover warmup cost, or fresh-start behavior. The
user experience has already shown that "eventually fast" is not good enough.

### Starter Pack

The starter pack has a manifest, rights notes, a downloader, and a visible app
shelf once installed. It still needs an obvious "Install Starter Pack" or
first-run seed action so users do not need manual filesystem work.

## Main Release Gaps

1. Finish moving private taxonomy out of fallback code paths.
   Public chrome is now generic, but owner-specific classifier terms should
   become private local config, sample manifests, or generic fixtures.

2. Make focus shelves fully configurable.
   Domain shelves appear from manifests today. The next step is a config file
   or UI that can add arbitrary shelf names without code changes.

3. Add validation for data contracts.
   Focus Manifest v1 is documented, but it needs a validation script and tests
   for missing paths, relative paths, DOI/id matching, section order, reason
   rendering, and privacy.

4. Wire the starter pack into the app.
   Starter records now render as tiles after installation. The missing piece is
   a visible setup/import flow.

5. Add performance budgets.
   Release tests should fail when shelf switching, first corpus render, or
   generated-card fallback exceeds a budget on fixture data. The target should
   be instant-feeling shelf switches even under rapid clicking.

6. Expand UI behavior tests.
   Keep the model tests, but add tests for arrow-key movement, context-menu
   downranking, empty corpus shelves, no-corpus startup, starter-pack catalog
   rendering, and disabled-AI startup.

7. Audit subprocess and file handling.
   Current subprocess calls use explicit arguments and timeouts in the key AI
   paths, but the release needs a documented pass over `claude`, `pdftotext`,
   `mdfind`, `qlmanage`, and Finder reveal behavior.

8. Document navigation-map artifacts.
   PDF and EPUB navigation generation is central to the product idea, but the
   public contract for generated nav maps is not yet as explicit as the focus
   manifest contract.

## Practical Release Standard

Before calling the public release polished, a fresh clone with no private corpus
should demonstrate this:

- launch to generic shelves, not empty private-domain shelves;
- install starter-pack books from a visible app flow and see tiles afterward;
- open PDF and EPUB documents in new tabs while preserving the library tab;
- switch shelves instantly on repeated clicks;
- work with AI disabled and with no `claude` CLI installed;
- let an agent create a focus shelf by writing one documented manifest file;
- show why an item is surfaced without needing to read source code;
- pass privacy scans for local paths, keys, downloaded corpora, and generated
  caches.

## Current Grade

- Architecture direction: good.
- Public/private boundary: good but needs repeated scanning.
- Reader functionality: good for early release.
- Tile UX: promising, not polished.
- Generality: improved; remaining fallback taxonomy needs cleanup.
- Performance proof: weak.
- Starter-pack integration: partial.
- Release confidence: early alpha, not polished public release.
