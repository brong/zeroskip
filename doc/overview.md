# zeroskip: an overview

zeroskip is an ordered key-value store — a database of keys and values, kept
sorted by key, so you can look one up or walk through a range of them. A
database is a **directory of files** rather than a single file. One process
writes at a time; any number read simultaneously, and readers never wait for
anything.

## The one idea

**Nothing is ever overwritten.** A file is only ever appended to, or newly
created. Nothing is modified in the middle, nothing is truncated, and nothing is
renamed once it has a name.

Almost everything else follows. A crash can only damage the *tail* of a file,
never the middle — so an interrupted database is not "corrupt", just slightly
shorter than intended, with everything before the interruption exactly as it was.
And since the earlier part of a file never changes, a reader needs no protection
from a writer: the bytes it is looking at cannot move under it. That is why
readers take no locks.

Each batch of changes ends with a marker giving its size and a checksum. A batch
counts only once that marker is present and correct, so a half-written batch is
indistinguishable from one that never happened.

## How data moves through it

```
       writes
         │
         ▼
   ┌───────────┐  when it reaches ~2MB   ┌───────────┐   merged with its    ┌──────────┐
   │  active   │ ──────────────────────► │  sorted   │ ───  neighbours  ──► │  larger  │
   │   file    │   a new active file     │ + indexed │      over time       │  sorted  │
   └───────────┘   starts; this one is   └───────────┘                      │   file   │
   records in      sorted and indexed     ready to be                       └──────────┘
   arrival order                          searched directly
```

Writes land at the end of one **active file** in arrival order, which is fast but
leaves no quick way to find a key without reading the whole file. Once it reaches
a couple of megabytes the writer starts a fresh one and rewrites the old one
**sorted**, with a small index at the end — quick to search. Those sorted files
are then **merged** into fewer, larger ones in the background, which is what
reclaims space: a key written ten times occupies ten records until a merge
collapses them to one.

Each file's name carries the range of **generation** numbers it holds, so a
database describes itself: to see what it contains, read the directory listing.
There is no catalogue file to fall out of step with reality, and nothing to
rebuild if one is lost.

## Deletions

Deleting a key writes a small "deleted" record rather than removing anything —
older files are untouched, so there is nothing to remove. That marker hides the
older copies, and can itself be discarded only once a merge can see the key's
whole history, from creation to deletion, in one place. Discard it sooner and the
deleted value reappears.

## What you get

| Property | What it means in practice |
|---|---|
| Crash safety | An interrupted write costs the interrupted change and nothing else. The database always opens. |
| Reads never block | No lock, no waiting, no matter how busy the writer is. A reader sees a consistent snapshot from the moment it opened. |
| One writer | Simple and predictable; enforced by a lock the operating system releases automatically if the process dies, so a crash never leaves things wedged. |
| Nothing shared but files | No shared memory, no cache, no coordination state. Nothing needs cleaning up after a crash. |
| Identical output everywhere | The same operations produce byte-identical files, on any machine and in any implementation — which is what lets several independent implementations be checked against each other. |
| Self-describing files | Every file states its own format version and how to read it, so files written by different versions can sit side by side. |

## What it costs

Honest trade-offs, not footnotes:

- **Space before merges.** Superseded and deleted records occupy space until a
  merge removes them, so a database is larger than its live contents — sometimes
  substantially.
- **A read may touch several files**, newest to oldest, until the key is found.
  Keeping the file count low is what merging is for.
- **One writer at a time.** Concurrent writers serialise; reads are unaffected.
- **Merges can be long.** Rewriting a large database is real work. Writing
  continues throughout, but the I/O is not free.
- **Readers hold space open.** A file being read is not freed until the reader
  finishes, so a long-running reader keeps retired files on disk.

## Where it fits

Its sibling `twom` keeps everything in one file and edits it in place, which
suits general-purpose use. zeroskip suits workloads that write heavily, want
readers that never block, and can let compaction happen out of band — buying the
crash behaviour and the lock-free reads, and paying for them in space and
background merging.
