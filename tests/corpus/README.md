# The zeroskip golden corpus

This directory holds **the bytes**, not a test. Each case is a real zeroskip
database alongside a portable description of what it should contain. Every
implementation, in every language, validates against these same files.

**T-0: the corpus is language-neutral.** The description format below is plain
text, deliberately simple enough to parse in an afternoon in any language, and
holds no C. Any implementation may generate the corpus — a corpus only one can
produce proves nothing about interoperability, because the bugs it would catch are
exactly the ones shared between generator and validator.

## What a case looks like

One directory per case. It contains the database's files verbatim, plus a
`case.txt`:

```
# a comment
uuid 4941da54-9406-4faa-a457-c4b65beae3eb
engine 1
comparator memcmp
op store 6b6579 76616c7565
op delete 6b657932
expect scan
6b6579 76616c7565
expect files
zeroskip-4941da54-9406-4faa-a457-c4b65beae3eb-00000001
```

## Grammar

Lines are processed in order. Blank lines and lines beginning `#` are ignored.
All keys, values and prefixes are **lowercase hex**, because keys may contain NUL
bytes and newlines (F-13) and no raw encoding can round-trip them through a text
file.

| Line | Meaning |
|---|---|
| `uuid <36-char>` | the database UUID, so generation is reproducible (T-1) |
| `engine <0\|1>` | checksum engine for files the generator creates (F-5) |
| `comparator <name>` | comparator name, `memcmp` for the default (F-11b) |
| `rollover <bytes>` | `rollover_size`; omitted means the 2MB default |
| `op store <keyhex> <valhex>` | one transaction, one store |
| `op delete <keyhex>` | one transaction, one delete |
| `op batch` … `op end` | the enclosed `store`/`delete` lines in **one** transaction |
| `op convert` | force conversion of non-active unordered files (D-12) |
| `op repack` | force one repack (D-16) |
| `op truncate <n>` | truncate the newest data file to `n` bytes, simulating a crash |
| `op garbage <hex>` | append raw bytes to the newest data file |
| `expect scan` | every following line, until the next directive, is `<keyhex> <valhex>` in comparator order |
| `expect files` | every following line is a filename that must be present |
| `expect get <keyhex> <valhex\|NOTFOUND>` | one point lookup |
| `expect check <OK\|FAILED>` | the result of the consistency checks |
| `indexdir <subdir>` | the case ships a pointer table cache (spec section 8) in this subdirectory of the case |
| `expect index` | every following line is one line of the pointer-table report, in the format below |

### Cases with a pointer table

A case carrying `indexdir` ships a published pointer table (spec section 8)
alongside the data files, in the named subdirectory. It is a **subdirectory**
rather than the case directory itself because P-2 forbids the cache directory
from being the database directory, so a case that mixed them could not be opened
the way it was built.

The `expect index` block is the report a driver produces from
`index-dump`:

```
INDEXDIR set threshold=<bytes>
TABLE <name> state=usable generation=<8 hex> valid_upto=<n> term_off=<n> term_csum=<8 hex> nptrs=<n>
```

with `state=absent` in place of the rest when no usable table exists. The
threshold is an implementation choice and is echoed rather than required to
match; everything on a `TABLE` line is fixed by the format and must.

Shipping the table's bytes is the point: an implementation that merely writes a
table of its own has proved nothing. **Loading ours** is what distinguishes a
shared format from a coincidentally similar one. An implementation that chooses
not to support the cache at all is still conforming — it must ignore the
subdirectory and produce every other `expect` unchanged, which is the property
that keeps the cache optional (spec section 8).

Engine 2 is deliberately **absent**: a file written under it is readable only by a
caller supplying the same function, so it cannot be part of a shared corpus
(F-5d).

## What an implementation must do with it

Two directions, and both matter:

**Decode.** Open each case's directory and check every `expect`. This catches a
reader that disagrees with the format.

**Encode.** Replay the `op` lines into an empty directory with the recorded UUID,
engine and comparator, then compare the resulting files **byte for byte** with the
checked-in ones. This is the sharper test (T-12a): it catches divergence in
padding, in ancestor omission, in the choice of short versus big form, and in
checksum seeding — *before* that divergence has a chance to become a
compatibility rule nobody meant to make.

Byte-exactness is only possible because encoding is canonical (F-15, F-26c) and
nothing time-varying or random enters the format. The UUID is the only
nondeterministic input, which is why every case pins it.

## Regenerating

```
make corpus
```

**This target exists to add cases, not to resolve a diff.** The checked-in bytes
are the contract. If regenerating changes an existing case, that is a format
change, and it needs a spec commit explaining why — not a corpus commit hiding it.
