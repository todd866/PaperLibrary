# Testing

PaperLibrary needs three kinds of proof:

1. Unit tests for deterministic logic.
2. Real-input tests for actual PDFs, ZIP EPUBs, Apple Books directory bundles,
   DRM refusals, iCloud placeholders, and catalog rows.
3. Visual verification for UI and reader work by launching the built app and
   inspecting the actual rendered result.

## Self-Run Loop

```text
configure/build -> launch PaperLibrary -> drive the real path -> inspect result -> iterate
```

Use:

```bash
scripts/dev/run-paperlibrary.sh [file]
```

Screenshots are useful for visual changes, but a failed/black screenshot is a
macOS Screen Recording permission issue, not proof of correctness.

## Runtime Path Matrix

| Path | Real input to exercise | Assert |
|---|---|---|
| Open PDF | a real local PDF; a large one | renders, scrolls, no crash |
| Open EPUB ZIP | a standard `.epub` | renders |
| Import Apple Books EPUB | a real directory bundle | imports and renders |
| DRM EPUB | bundle with `META-INF/encryption.xml` | graceful refusal |
| iCloud placeholder | not-downloaded file | clear "download first" behavior |
| Catalog row | slug resolving to a file | opens when local, degrades when missing |
| Library shelves | real local library | rows, covers, and filters populate |
| Titlebar tabs | windowed and fullscreen | strip sits in the titlebar |

## Regression Baseline

Focused test gate:

```bash
ctest --test-dir build --output-on-failure -R '^(shelltest|librarystoretest|libraryautotaggertest|paperlibrarymodeltest|coverheuristictest|covergeneratortest|epubcovertest|epubimportertest|epubreadercorpustest|chromestriptest)$'
```

The old Okular part/generator tests were removed with the old runtime stack.
New tests should target the PaperLibrary shell and supported PDF/EPUB/library
paths.

## Library Morning Check

Before trusting a local build, audit the visible first screen of every library
tab against the real configured corpus:

```bash
python3 scripts/dev/library_morning_check.py --limit 28
```

The command writes:

- `build/reports/library-morning-check.md` for a human shelf-by-shelf review.
- `build/reports/library-morning-check.json` for automation.

Use `--no-fail` when you want a report without stopping on current known
metadata debt. The check should fail on visible placeholder titles, filename or
DOI-slug fallbacks, missing paper author/year/topic metadata, duplicate
first-screen labels, and tile text that cannot fit the browsing surface.

The harness has a small regression test:

```bash
python3 scripts/dev/library_morning_check_test.py
```
