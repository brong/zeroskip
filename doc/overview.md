# zeroskip: an overview

zeroskip is an ordered key-value store — a database of keys and values, kept
sorted by key, so you can look one up or walk through a range of them. A
database is a **directory of files** rather than a single file. One process
writes at a time; any number read simultaneously, and readers never wait.

## The one idea

**Nothing is ever overwritten.** A file is only appended to, or newly created.
Nothing is modified in the middle, nothing is truncated, and nothing is renamed
once it has a name.

Almost everything else follows. A crash can only damage the *tail* of a file,
never the middle, so an interrupted database is shorter than intended rather than
broken, and everything before the interruption is exactly as it was. And since
the earlier part of a file never changes, a reader needs no protection from a
writer: the bytes it is looking at cannot move under it. That is why readers take
no locks.

Each batch of changes ends with a marker giving its size and a checksum over it.
A batch counts only once that marker is present and correct, so a half-written
batch is indistinguishable from one that never happened.

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

Writes land at the end of one **active file** in arrival order. That is fast, but
leaves no quick way to find a key without reading the whole file. Once it reaches
a couple of megabytes the writer starts a fresh one and rewrites the old one
**sorted**, with a small index at the end, which is quick to search. Those sorted
files are then **merged** into fewer, larger ones in the background. Merging is
what reclaims space: a key written ten times occupies ten records until a merge
collapses them to one.

Each file's name carries the range of **generation** numbers it holds, so the
directory listing is the whole story: to see what a database contains, list it.

## Deletions

Deleting a key writes a small "deleted" record rather than removing anything.
Older files are untouched, so there is nothing to remove; the marker hides the
older copies instead. It can itself be discarded only once a merge can see the
key's whole history, from creation to deletion, in one place. Discard it sooner
and the deleted value reappears.

## What you get

| Property | What it means in practice |
|---|---|
| Crash safety | An interrupted write costs the interrupted change and nothing else. The database always opens. |
| Reads never block | No lock and no waiting, however busy the writer is. A reader sees a consistent snapshot from the moment it opened. |
| One writer | Enforced by a lock the operating system releases when the process dies, so a crash never leaves the database locked against the next writer. |
| A database is only files | Nothing is kept outside the directory, so nothing needs cleaning up after a crash. |
| Identical output everywhere | The same operations produce byte-identical files on any machine and in any implementation, which is what allows independent implementations to be checked against each other. |
| Self-describing files | Every file states its own format version and how to read it, so files written by different versions sit side by side. |

## Sealing and compaction

Two things a caller can ask for explicitly.

**Sealing** converts the newest file — the one records land in — into a sorted,
indexed one. Afterwards nothing in the database needs reading end to end, so any
process opening it starts work immediately. It is cheap and bounded, because the
newest file is only ever a couple of megabytes, and there is no harm in doing it
often: before a backup, or before handing the database to readers. It creates
nothing new; the next write simply starts a fresh file.

**Compaction** merges everything into a single file. This is the only operation
that reclaims the space held by deleted records. A record marking a deletion has
to be kept as long as any older file might still hold the key it deletes — so
ordinary merging, which only ever combines some of the files, must keep them all.
A merge that takes *every* file has nothing older to worry about, and can drop
them. In a database where three quarters of the keys have been deleted, that is
about three quarters of the space.

The cost is that compaction rewrites the entire database in one go. Writing
continues while it runs, but the I/O is proportional to everything stored, not to
what changed. It is a maintenance operation, not something to do on a timer.

## The pointer table cache

Finding a key in the **active file** means reading it end to end, because records
are in arrival order with no index. That is bounded — the active file only grows
to a couple of megabytes — but it is paid every time a process opens the database,
and again at the start of every write.

A process may write the result out: a **pointer table**, a sorted list of where
each key lives, published into a scratch directory the caller nominates. The next
process to come along loads it and only has to read whatever has been appended
since. Writers and readers alike publish; whoever does the work shares it.

Three things keep this from undoing anything above. The scratch directory is not
the database, so nothing is written into the database that was not already going
there. A table is published by writing a new file and renaming it into place,
never by editing one, so the rule that nothing is overwritten still holds. And a
table carries enough about the file it describes to prove it belongs to it —
anything doubtful is ignored and the file is read the old way, so a damaged or
stale table costs time and never correctness.

It is **off unless asked for**, and the scratch directory must be one the caller
controls: a table planted by someone else would produce wrong answers rather than
obvious failure. It must also be discarded whenever the database directory is
restored from a backup, since a table can outlive the file it describes.

In exchange, opening a database with a large active file goes from around 1.5 ms
to under 0.1 ms, and a workload committing one record at a time gets about ten
times faster — the second effect being the larger one, and not the one it was
built for. Below a few hundred records it is marginally slower than not having it.

## What it costs

- **Space before merges.** Superseded and deleted records occupy space until a
  merge removes them, so a database is larger than its live contents — sometimes
  substantially.
- **A read may touch several files**, newest to oldest, until the key is found.
  Keeping the file count low is what merging is for.
- **One writer at a time.** Concurrent writers serialise. Reads are unaffected.
- **Merges can be long.** Rewriting a large database moves a lot of data. Writing
  continues throughout, but the I/O is real.
- **Readers hold space open.** A file being read is not freed until the reader
  finishes, so a long-running reader keeps retired files on disk.
- **The pointer table cache needs looking after.** It is optional and everything
  works without it, but if used, its directory has to be scoped to the database —
  a stale table surviving a restore is the one way it can mislead.

## Where it fits

Its sibling `twom` keeps everything in one file and edits it in place, which
suits general-purpose use. zeroskip suits workloads that write heavily, want
readers that never block, and can let compaction happen out of band. The
append-only structure is what produces the crash behaviour and the lock-free
reads; space and background merging are what it costs.
