# index_server

A live content-indexing service for Haiku, originally started by Clemens
Zeidler (GSoC 2010) and revived here. Watches your volumes, keeps a set of
add-ons ("analysers") up to date about every file that changes, and lets
those add-ons decide what's worth remembering about it - a full-text index,
a few BFS attributes, or nothing at all.

## What this is - and isn't

- It is **not** a replacement for Tracker's own Query mechanism.
  Tracker queries only ever see BFS attributes, which is exactly what most
  of the analysers here write - those results already show up in normal
  Tracker queries and Find panels without anything extra.
- The one thing Tracker *can't* see is full text content (the inside of a
  document, an email body, and so on), because that lives in a separate
  CLucene index outside BFS. That's what the bundled search app
  (`IndexServerSearch`) is for - point it at a query and it checks every
  volume's full-text index directly.
- index_server runs constantly in the background (via launch_daemon, see
  below) with no window of its own. Nothing to click on; if it's doing its
  job, you won't notice it except for an occasional progress notification
  during a large catch-up.

## What gets indexed

Each of these is a separate add-on; disabling one in the settings preflet
doesn't affect the others.

| Add-on | What it writes | Where |
| --- | --- | --- |
| FullTextAnalyser | Full text of any file BTranslatorRoster can turn into plain text | CLucene index (searchable via IndexServerSearch) |
| MailAnalyser | Mail body text | Same CLucene index as FullTextAnalyser |
| AudioTagAnalyser | Artist/Title/Album | BFS attributes (`Audio:Artist`, `Media:Title`, `Audio:Album`) |
| ExifAnalyser | Camera make/model, date taken, pixel dimensions (JPEG/TIFF only) | BFS attributes (`EXIF:*`) |
| MediaKitAnalyser | Codec, duration, video resolution | BFS attributes (`Media:Codec`, `Media:Length`, `Media:Width`/`Height`) |

Because BTranslatorRoster is the whole basis of FullTextAnalyser, whatever
formats it can convert to text automatically become full-text searchable -
including any translator you install later (an OCR translator for scanned
images, for instance) without index_server itself needing to know about it.

Formats that need a dedicated *Translator* rather than an index_server
analyser (PDF, ODF/OOXML/EPUB, HTML) aren't bundled yet - see the project's
issue tracker for the current state of that work.

## Settings

Open **Index Server Settings** from Preferences. Three things to configure:

- **Mode**: *Blacklist* (index everything except the listed paths - the
  default) or *Whitelist* (index only the listed paths).
- **Paths**: drag folders from Tracker onto the list, or use "Add…"/
  "Remove". The default blacklist already excludes index_server's own
  data, `/boot/system/cache`, `/boot/system/var/log`, Trash, the packages
  directory, and (during development) index_server's own non-packaged
  binaries.
- **Analysers**: enable/disable each add-on independently from the
  "Analysers" menu.

"Revert" reloads whatever is currently saved on disk (discarding unsaved
changes in the window); "Defaults" resets to the built-in defaults above.
Changes take effect immediately - no separate "Apply" step, and no restart
needed.

There's currently no per-folder analyser assignment (e.g. "index audio
tags here, but not full text") - it's all-or-nothing per volume. See issue
#7 if you need that.

## Searching

Launch **Index Search**. Type a query and press Enter or click "Search".

The query box is a real [Lucene query][lucene-syntax], not a plain keyword
match: `"exact phrase"` for phrases, `AND`/`OR`/`NOT` between terms,
`term*` for a prefix wildcard, `term~` for a fuzzy match. If you type
something that isn't valid query syntax (mismatched quotes, for instance),
the search currently just comes back with zero results rather than
explaining what went wrong - if a query you expect to work returns
nothing, check for a syntax mistake before assuming the content isn't
indexed.

[lucene-syntax]: https://lucene.apache.org/core/2_9_4/queryparsersyntax.html

Double-click a result to open it with its default application.

## Known limitations

- Results show a path and a relevance score, but no preview/snippet of
  where the match actually is in the document - CLucene's index doesn't
  currently keep the original text around to generate one from. Possible
  later, but changes the index format (see the schema-versioning issue).
- Files larger than 32MB aren't indexed for full text, to keep a single
  large file from stalling the catch-up queue.
- No per-folder analyser assignment yet (see above).
- The search app has no live filter-as-you-type yet - you always need to
  submit the query explicitly.

## Autostart

index_server starts automatically once your home volume is mounted, via a
launch_daemon service definition (`data/launch/index_server`) - no need to
launch it yourself. It runs as a background app (no Deskbar window), and
restarts automatically if it crashes.

**Install system-wide** (`pkgman install` without `-H`/`--home`). A home
install doesn't work: the service is a system-level job (it needs to watch
every mounted volume, not just one user's), and Haiku's system-mode
launch_daemon never scans a user's package data for job definitions - only
the per-session user-mode daemon does, and `launch_roster` won't find the
job there under its usual name. There's currently no error at install time
that says so; `launch_roster start x-vnd.haiku-index_server` just fails
with "Name not found" (see issue #63).

## Troubleshooting

- **"No volume has a full text index yet." right after a fresh
  install/boot**: the first catch-up over the whole volume can take a
  while for a large disk. A progress notification appears once the
  backlog is big enough (200+ files) to be worth mentioning - if you
  don't see one and it's been a while, something may be stuck; check
  whether index_server is still running.
- **A previous crash left search stuck**: CLucene keeps a `write.lock`
  file in its index directory while writing; if index_server was killed
  forcefully mid-write, a stale lock can make every subsequent write
  time out. Removing the volume's `FullTextAnalyser/write.lock` file lets
  it recover (there is currently no automatic detection for this).

## Localization

The settings preflet, search app, and index_server's own notifications
are wired for translation (`B_TRANSLATE`) with a German translation
included. Contributing more languages upstream goes through
i18n.haiku-os.org once this merges, not through this repository directly.
