# Public Release Checklist

PaperLibrary-dev is the private working repository. Do not publish it as the
public `todd866/PaperLibrary` repository until these gates are complete.

## Repository Boundary

- Review `docs/public-release-rules.md` before changing shelf names, AI hooks,
  focus manifests, starter-pack behavior, or public defaults.
- Keep `paperlibrary-local` private; it contains the personal corpus, generated
  catalogs, and local pipeline artifacts.
- Publish only code, release docs, and public-domain starter metadata/scripts.
- Remove or rewrite private planning notes that mention local corpus counts,
  private filesystem paths, personal workflow details, or unreleased prompts.
- Replace machine-specific build examples with clone-relative examples.
- Verify the public repo has no remotes, scripts, submodules, or docs that point
  back to private backend repositories.

## Security And Privacy

- Verify no `.env`, API key, OAuth token, private key, local database,
  downloaded corpus, generated cache, or personal document path is tracked.
- Keep AI features opt-in, BYO-token, and local-process based.
- Keep auto-tagging disabled by default.
- Do not persist source document text in AI navigation caches.
- Review every subprocess call for explicit arguments, timeouts, and failure
  behavior.
- Audit user-controlled file handling for path traversal, unexpected writes, and
  unsafe deletion.

## Packaging

- Build from a fresh clone and fresh build directory.
- Run the focused PaperLibrary test gate.
- Launch the app from the produced bundle, not only the build-tree binary.
- Verify PDF open, EPUB open, Apple Books import, library shelf, starter-pack
  import, and disabled/no-CLI AI states.
- Verify top-level shelves remain tile-first, keyboard navigable, explainable,
  and useful without private focus manifests.
- Confirm app metadata, icons, bundle identifiers, repository URLs, and license
  notices say PaperLibrary while preserving KDE Okular/GPL attribution.

## Starter Pack

- Use only works whose source records them as public domain in the United
  States.
- Keep source landing URLs and rights notes with every item.
- Do not imply Project Gutenberg endorsement.
- Make jurisdiction limits visible: public-domain status can differ outside the
  United States.
- Prefer a downloader/install action over committing downloaded EPUB payloads
  unless release packaging has been reviewed for source terms and trademark
  requirements.
