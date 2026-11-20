#!/usr/bin/env python3
"""Write a tiny synthetic wiki dump for the CI smoke test.

Usage: make_test_wiki.py <data folder>

Creates testwiki-20260101-{page,linktarget,pagelinks,redirect}.sql.gz in the folder. The graph is small but covers
the cases the graph builder has to get right:

    Start -> Alpha -> Gamma's_Page -> Target
    Start -> Old_Name (a redirect to Beta) -> Gamma's_Page -> Target

- a link to a redirect page is folded into a link to its target (Old_Name -> Beta)
- a title with an apostrophe is escaped in the dump and must be kept
- a page outside the main namespace and a link to a page that does not exist are ignored
- `page` uses the old layout (all rows on one INSERT line), the other tables the one-row-per-line layout that
  Wikimedia publishes since October 2026

So "Start" -> "Target" has exactly 3 clicks and 2 shortest paths. The CI workflow checks for that line.
"""

import gzip
import sys
from pathlib import Path

PREFIX = "testwiki-20260101"

HEADER = """-- MariaDB dump 10.19 (synthetic test data for WikiGraph Explorer)
--
-- Table structure for table `{table}`
--

DROP TABLE IF EXISTS `{table}`;
CREATE TABLE `{table}` (
{columns}
  PRIMARY KEY (`{key}`)
) ENGINE=InnoDB DEFAULT CHARSET=binary;

--
-- Dumping data for table `{table}`
--

LOCK TABLES `{table}` WRITE;
"""

FOOTER = "UNLOCK TABLES;\n"


def sql(value):
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"
    return str(value)


def write_table(folder, table, columns, key, rows, one_row_per_line):
    text = HEADER.format(table=table, key=key, columns="\n".join(f"  `{name}` {kind}," for name, kind in columns))
    values = ["(" + ",".join(sql(v) for v in row) + ")" for row in rows]
    if one_row_per_line:
        text += f"INSERT INTO `{table}` VALUES\n" + ",\n".join(values) + ";\n"
    else:
        text += f"INSERT INTO `{table}` VALUES " + ",".join(values) + ";\n"
    text += FOOTER
    path = folder / f"{PREFIX}-{table}.sql.gz"
    with gzip.open(path, "wb") as f:
        f.write(text.encode("utf-8"))
    return path


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)

    # page_id, page_namespace, page_title, page_is_redirect, page_is_new, page_random, page_touched,
    # page_links_updated, page_latest, page_len, page_content_model, page_lang
    def page(page_id, title, redirect=0, namespace=0):
        return (page_id, namespace, title, redirect, 0, 0.5, "20260101000000", "20260101000000", page_id, 100,
                "wikitext", None)

    pages = [
        page(1, "Start"),
        page(2, "Alpha"),
        page(3, "Beta"),
        page(4, "Gamma's_Page"),
        page(5, "Target"),
        page(6, "Old_Name", redirect=1),
        page(7, "Start", namespace=1),  # Talk:Start, not an article
        page(8, "Isolated"),
    ]
    write_table(folder, "page", [
        ("page_id", "int(8) unsigned NOT NULL AUTO_INCREMENT"),
        ("page_namespace", "int(11) NOT NULL DEFAULT 0"),
        ("page_title", "varbinary(255) NOT NULL DEFAULT ''"),
        ("page_is_redirect", "tinyint(1) unsigned NOT NULL DEFAULT 0"),
        ("page_is_new", "tinyint(1) unsigned NOT NULL DEFAULT 0"),
        ("page_random", "double unsigned NOT NULL DEFAULT 0"),
        ("page_touched", "binary(14) NOT NULL"),
        ("page_links_updated", "binary(14) DEFAULT NULL"),
        ("page_latest", "int(8) unsigned NOT NULL DEFAULT 0"),
        ("page_len", "int(8) unsigned NOT NULL DEFAULT 0"),
        ("page_content_model", "varbinary(32) DEFAULT NULL"),
        ("page_lang", "varbinary(35) DEFAULT NULL"),
    ], "page_id", pages, one_row_per_line=False)

    targets = {"Start": 10, "Alpha": 11, "Beta": 12, "Gamma's_Page": 13, "Target": 14, "Old_Name": 15,
               "Missing_Page": 16}
    write_table(folder, "linktarget", [
        ("lt_id", "bigint(20) unsigned NOT NULL AUTO_INCREMENT"),
        ("lt_namespace", "int(11) NOT NULL"),
        ("lt_title", "varbinary(255) NOT NULL"),
    ], "lt_id", [(lt, 0, title) for title, lt in targets.items()], one_row_per_line=True)

    links = [
        (1, "Alpha"),
        (1, "Old_Name"),  # redirect to Beta
        (1, "Missing_Page"),  # no such page
        (2, "Gamma's_Page"),
        (3, "Gamma's_Page"),
        (4, "Target"),
        (5, "Start"),
    ]
    write_table(folder, "pagelinks", [
        ("pl_from", "int(8) unsigned NOT NULL DEFAULT 0"),
        ("pl_from_namespace", "int(11) NOT NULL DEFAULT 0"),
        ("pl_target_id", "bigint(20) unsigned NOT NULL"),
    ], "pl_from", [(src, 0, targets[title]) for src, title in links], one_row_per_line=True)

    write_table(folder, "redirect", [
        ("rd_from", "int(8) unsigned NOT NULL DEFAULT 0"),
        ("rd_namespace", "int(11) NOT NULL DEFAULT 0"),
        ("rd_title", "varbinary(255) NOT NULL DEFAULT ''"),
        ("rd_interwiki", "varbinary(32) DEFAULT NULL"),
        ("rd_fragment", "varbinary(255) DEFAULT NULL"),
    ], "rd_from", [(6, 0, "Beta", "", "")], one_row_per_line=True)

    print(f"wrote {PREFIX} to {folder}")


if __name__ == "__main__":
    main()
