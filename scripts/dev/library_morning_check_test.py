#!/usr/bin/env python3
"""Focused tests for library_morning_check.py."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import library_morning_check as check


def write_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def write_manifest(root: Path, shelf: str, rows: list[dict]) -> None:
    target = root / "focus" / shelf / "manifest.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(rows), encoding="utf-8")


class LibraryMorningCheckTest(unittest.TestCase):
    def test_placeholder_work_titles_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp)
            write_manifest(
                corpus,
                "Work",
                [
                    {
                        "id": "file-paper",
                        "title": "paper",
                        "kind": "pdf",
                        "source": "loose-file",
                        "reason": "highdimensional project file",
                        "section": "00-beyond-bayes-revision",
                    }
                ],
            )

            report = check.build_report(corpus, limit=5)
            work = next(shelf for shelf in report["shelves"] if shelf["name"] == "Work")
            tile = work["tiles"][0]
            messages = [issue["message"] for issue in tile["issues"]]
            self.assertTrue(any("placeholder" in message for message in messages), messages)

    def test_book_pdf_is_not_a_paper_author_year_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp)
            write_manifest(
                corpus,
                "Reading",
                [
                    {
                        "id": "book-pdf",
                        "title": "The Dawn of Everything",
                        "kind": "pdf",
                        "source": "book:pdf",
                        "reason": "anthropology/nonfiction",
                        "section": "02-nonfiction-anthropology",
                    }
                ],
            )

            report = check.build_report(corpus, limit=5)
            books = next(shelf for shelf in report["shelves"] if shelf["name"] == "Books")
            tile = books["tiles"][0]
            messages = [issue["message"] for issue in tile["issues"]]
            self.assertFalse(any("paper tile has no authors" in message for message in messages), messages)
            self.assertFalse(any("paper tile has no year" in message for message in messages), messages)

    def test_nonfiction_does_not_leak_into_fiction_tab(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp)
            write_manifest(
                corpus,
                "Reading",
                [
                    {
                        "id": "nonfiction-epub",
                        "title": "Everything Was Forever Until It Was No More",
                        "kind": "epub",
                        "source": "app-recent",
                        "reason": "current nonfiction; recently opened",
                        "section": "03-nonfiction-current",
                    }
                ],
            )

            report = check.build_report(corpus, limit=5)
            fiction = next(shelf for shelf in report["shelves"] if shelf["name"] == "Fiction")
            nonfiction = next(shelf for shelf in report["shelves"] if shelf["name"] == "Non-fiction")
            self.assertEqual(fiction["visible_count"], 0)
            self.assertEqual(nonfiction["visible_count"], 1)

    def test_fiction_uses_catalog_books_without_novel_paper_leak(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp)
            write_jsonl(
                corpus / "catalog.jsonl",
                [
                    {
                        "slug": "got-catalog",
                        "title": "A Game Of Thrones",
                        "authors": "George R. R. Martin",
                        "year": "1996",
                        "kind": "epub",
                        "source": "book:epub",
                    },
                    {
                        "slug": "dune",
                        "title": "Dune",
                        "authors": "Herbert, Frank",
                        "year": "1965",
                        "kind": "epub",
                        "source": "book:epub",
                    },
                    {
                        "slug": "green-mars",
                        "title": "Green Mars",
                        "authors": "Robinson, Kim Stanley",
                        "year": "1993",
                        "journal": "(book)",
                        "source": "book:epub",
                    },
                    {
                        "slug": "hobbit",
                        "title": "The Hobbit",
                        "authors": "Tolkien, J. R. R.",
                        "year": "1937",
                        "journal": "(book)",
                        "source": "aa_book",
                    },
                    {
                        "slug": "carpentaria",
                        "title": "Carpentaria: A Novel",
                        "authors": "Alexis Wright",
                        "year": "2006",
                        "kind": "epub",
                        "source": "book:epub",
                    },
                    {
                        "slug": "novel-biomarker",
                        "title": "Candidate marker for diagnosis or staging: a novel biomarker",
                        "authors": "K. M. Fisher",
                        "year": "2012",
                        "journal": "Brain",
                        "source": "harvest:laptop-audit",
                    },
                ],
            )
            write_manifest(
                corpus,
                "Reading",
                [
                    {
                        "id": "got-focus",
                        "title": "A Game Of Thrones 52314094",
                        "kind": "epub",
                        "source": "app-recent",
                        "reason": "current fiction; recently opened",
                        "section": "00-fiction-current",
                    }
                ],
            )

            report = check.build_report(corpus, limit=12)
            fiction = next(shelf for shelf in report["shelves"] if shelf["name"] == "Fiction")
            titles = [tile["display_title"] for tile in fiction["tiles"]]
            self.assertEqual(titles.count("A Game Of Thrones"), 1)
            self.assertIn("Dune", titles)
            self.assertIn("Green Mars", titles)
            self.assertIn("The Hobbit", titles)
            self.assertIn("Carpentaria", titles)
            self.assertNotIn("Fisher 2012", titles)
            self.assertEqual(fiction["source"], "Reading focus + catalog fiction books")

    def test_broken_et_al_citation_falls_back_to_title(self) -> None:
        record = check.Record(
            key="mnd-placeholder-author",
            title="Diagnostic delay in amyotrophic lateral sclerosis",
            authors="et al.",
            year="2025",
            journal="Amyotrophic Lateral Sclerosis and Frontotemporal Degeneration",
            kind="pdf",
            source="harvest:laptop-audit",
        )

        tile = check.build_tile(record, "MND", "fixture")
        check.audit_tile(tile)
        self.assertEqual(tile.display_title, "Diagnostic delay in amyotrophic lateral sclerosis")
        self.assertFalse(any("broken citation" in issue["message"] for issue in tile.issues))

    def test_pubmed_initials_et_al_author_uses_surname(self) -> None:
        record = check.Record(
            key="mnd-pubmed-author",
            title="Contributions of neurologists to diagnostic timelines of ALS",
            authors="Dave KD et al.",
            year="2025",
            journal="Amyotrophic Lateral Sclerosis and Frontotemporal Degeneration",
            kind="pdf",
            source="md-project-review-set",
        )

        tile = check.build_tile(record, "MND", "fixture")
        self.assertEqual(tile.display_title, "Dave et al. 2025")

    def test_focus_paper_uses_reason_summary_as_subtitle(self) -> None:
        record = check.Record(
            key="mnd-reason",
            title="A deliberately verbose amyotrophic lateral sclerosis title that should not be the tile subtitle",
            authors="Dave KD et al.",
            year="2025",
            journal="Amyotrophic Lateral Sclerosis and Frontotemporal Degeneration",
            kind="pdf",
            source="md-project-review-set",
            reason="MD project review set; ALS/MND title or metadata; core disease term; diagnostic pathway",
        )

        tile = check.build_tile(record, "MND", "fixture")
        self.assertEqual(tile.subtitle, "core disease term / diagnostic pathway")

    def test_focus_manifest_overrides_stale_catalog_title(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp)
            write_jsonl(
                corpus / "catalog.jsonl",
                [
                    {
                        "slug": "book-1",
                        "title": "Raw Archive Title 0123456789abcdef",
                        "kind": "epub",
                        "source": "book:epub",
                    }
                ],
            )
            write_manifest(
                corpus,
                "Reading",
                [
                    {
                        "id": "book-1",
                        "title": "Curated Title",
                        "authors": "Curated Author",
                        "year": "2024",
                        "kind": "epub",
                        "source": "book:epub",
                        "reason": "current nonfiction",
                        "section": "03-nonfiction-current",
                    }
                ],
            )

            report = check.build_report(corpus, limit=5)
            books = next(shelf for shelf in report["shelves"] if shelf["name"] == "Books")
            self.assertEqual(books["tiles"][0]["display_title"], "Curated Title")
            self.assertEqual(books["tiles"][0]["metadata_line"], "Curated Author | 2024 | Non-fiction | EPUB")

    def test_title_slash_is_not_filenameish(self) -> None:
        self.assertFalse(check.is_filenameish("MRI/Ultrasound Accuracy for Neuropathy Mimics"))
        self.assertTrue(check.is_filenameish("/Users/example/PaperLibrary/file.pdf"))


if __name__ == "__main__":
    unittest.main()
