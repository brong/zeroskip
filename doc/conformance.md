# Conformance map

**T-11.** Every normative requirement in
[`doc/specification.md`](specification.md)
mapped to the test that enforces it. A requirement with no test is a gap to
close, and is listed as one rather than left as a blank cell — a blank reads as
"covered" to the next reader.

Test names are `zstest` functions with the `test_` prefix stripped, except:
`crash/x` is a `zstest-crash` case, `tool.sh` is `tests/tool.sh`, `corpus` is
`tests/corpus/`, and `zsbench` names a benchmark that produces the number.

A citation means **a test whose source cites that requirement**, not necessarily
the only or the strongest one. Most rows were found by scanning the test sources
for requirement labels, which is why the tests carry them in comments; the rows
that scanning could not attribute were filled in by hand.

| | |
|---|---|
| Requirements | 267 |
| With an enforcing test | 257 |
| Gaps, each with a reason | 10 |

Regenerate the citation scan with `./tests/conformance.sh`, which cross-checks
this table against the spec and fails if a label here is missing from the spec or
a spec label is missing here.

## Guarantees (`G-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `G-0` | Nothing in the format depends on `mmap`, on pointer-sized integers, | **none** |
| `G-0a` | Every integer in every structure is little-endian (F-1), including | `interop_constants_uuid` |
| `G-0b` | Any arithmetic on a length, count or offset **read from a file** MUST | `file_bounds`, `interop_constants_uuid`, `overflow_guards`, +1 more |
| `G-1` | Append-only. No committed byte is ever mutated and no file is ever | `crash/crash_at_every_call` |
| `G-2` | Commit atomicity. Once `zs_txn_commit` returns `ZS_OK`, the whole | `crash/crash_at_every_call, crash/sync_failure_gate1, crash/sync_failure_gate2` |
| `G-3` | Always reopens. Any state a crash can produce MUST open in bounded | `check_noncanonical`, `open_bad_nonactive`, `record_canonical`, +2 more |
| `G-4` | Snapshot isolation, lock-free reads. A read transaction sees a fixed | `mp_writer_and_readers`, `write_txn_isolation` |
| `G-5` | One writer. At most one writer per database, enforced by an `fcntl` | `lock_dies_with_process`, `lock_two_handles_one_process`, `mp_killed_writer` |
| `G-6` | No shared mutable state. Nothing a reader may be reading is ever | `mp_reader_across_repack, snapshot_refcount` |
| `G-7` | Read paths agree. Point lookups and range scans resolve visibility | `corpus_engine_from_file_not_config`, `fcur_uniform`, `read_arrangements`, +3 more |

## On-disk format (`F-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `F-1` | Integers are little-endian. | `le_accessors` |
| `F-2` | Every record begins at an offset that is a multiple of 8 and | `crash/crash_leaves_unaligned_length`, `overflow_guards`, `record_roundtrip` |
| `F-3` | Offsets and pointers are absolute byte offsets from the file start. | `header_byte_layout, inorder_ptrs64` |
| `F-4` | The checksum field is always the **last 4 bytes** of the structure | `header_checksum` |
| `F-5` | Exactly three checksum engines exist: | `corpus`, `interop_constants_csum` |
| `F-5a` | The engine id is recorded in each file header, so every file is | `corpus_engine_from_file_not_config`, `file_engine_from_header`, `open_engine_selection` |
| `F-5b` | Engine 1 is **`XXH3_64bits` with the default seed of 0**, and the | `interop_constants_csum` |
| `F-5c` | Engine 0 weakens G-2 and G-3, because F-22's property — that a | `header_checksum`, `span_engine_zero` |
| `F-5d` | Engine 2 makes a file readable only by a caller supplying the same | `corpus` |
| `F-5e` | `ZS_NOCSUM` is distinct from engine 0: it skips verification of | `check_records_checksum`, `span_terminator_without_data`, `nocsum_still_rejects_bad_span`, `idxcache_rejection_rules` |
| `F-6` | A reader MUST validate all 16 bytes, not a prefix. | `magic` |
| `F-6a` | The magic is **not valid UTF-8**: `0x89` lies in the continuation-byte | `magic`, `staging_names` |
| `F-7` | Split read and write versions let an older library determine that it | `header_versions` |
| `F-7a` | Version 2 is the first published version: a conforming writer | `header_versions` |
| `F-8` | Reserved fields MUST be written as zero and MUST be ignored on read. | `header_reserved` |
| `F-9` | Generations start at 1, so `end == 0` is never a legitimate | `filename_rejections`, `header_bounds_and_ranges`, `header_checksum` |
| `F-10` | An unordered file holds **exactly one** generation: `start` is that | `header_roundtrip` |
| `F-11` | Every file of a database MUST carry the same UUID and the same | `open_comparator_agreement` |
| `F-11a` | The default comparator. Compare `min(alen, blen)` bytes as | `index_binary_keys`, `index_ordered_traversal`, `interop_constants_compar`, +1 more |
| `F-11b` | The default comparator's recorded name is the ASCII string | `corpus`, `header_roundtrip`, `interop_constants_compar`, +2 more |
| `F-12` | The table above is normative: any byte not in it is invalid, | `span_progress`, `type_byte_validity` |
| `F-12a` | Each bit is meaningful in isolation: `type & IsBig` selects the wide | `type_byte_validity` |
| `F-12b` | Each data shape has exactly one form. Nothing distinguishes a | `record_roundtrip` |
| `F-12c` | Bit `0x08` was `HasAncestor`, and each data shape had a second form | `type_byte_validity` |
| `F-13` | Lengths are authoritative; keys and values MAY contain NUL bytes, | `corpus`, `index_binary_keys`, `interop_constants_compar`, +4 more |
| `F-14` | A key MUST be at least 1 byte. An empty value is legal and distinct | `malformed_never_hangs`, `record_bounds`, `record_byte_layout`, +2 more |
| `F-15` | Encoding is canonical: an implementation MUST use the short form | `corpus`, `dump_shows_rollback`, `header_roundtrip`, +5 more |
| `F-18` | A record MUST NOT carry any reference to another record. In | `write_record_is_self_contained`, `record_canonical`, `type_byte_validity` |
| `F-19` | The checksum covers the span's data bytes followed by the | `interop_constants_csum`, `mp_reader_sees_torn_span`, `terminator` |
| `F-20` | Terminators are only ever found by scanning **forward** from the | `span_basic` |
| `F-21` | A `COMMIT` makes its span's records live. A `ROLLBACK` is a commit | `span_rollback`, `write_abort` |
| `F-22` | Because the checksum covers the span **and** the terminator, a | `crash/crash_nosync_mode`, `mp_reader_sees_torn_span`, `span_terminator_without_data`, +2 more |
| `F-23` | From the end of an unordered file's header onwards, the file is a flat | `span_basic`, `span_engine_zero` |
| `F-24` | An unordered file is **complete** at its last valid span, whether | `check_noncanonical`, `check_unclean_reported`, `corpus_engine_from_file_not_config`, +3 more |
| `F-24a` | An in-order file has no equivalent notion, because it is written | `inorder_kind_rules` |
| `F-25` | Visibility is per span, not a watermark: a rolled-back span may sit | `dump_shows_rollback`, `span_rollback` |
| `F-26` | Pointers reference every record in the file, sorted by key | `check_out_of_order_pointers` |
| `F-26a` | The trailer's back pointer is what locates the section, so nothing | **none** |
| `F-26b` | The pointer-section checksum covers everything from the start of the | **none** |
| `F-26c` | Encoding is canonical: `PTRS32` MUST be used when every record | `corpus`, `dump_shows_rollback`, `inorder_empty`, +1 more |
| `F-26d` | The narrow section is padded with zeroes to a multiple of 8 so the | `inorder_trailer_negatives`, `inorder_widths_and_padding` |
| `F-26e` | The records checksum covers the region from the end of the header to | `check_records_checksum`, `inorder_records_checksum` |
| `F-26f` | The records checksum is verified lazily — by | `check_records_checksum`, `inorder_records_checksum`, `open_is_o1_in_records` |
| `F-26g` | `count` MAY be **zero**. An in-order file with no records is legal | `fcur_empty_sources`, `inorder_empty`, `interop_constants_csum`, +2 more |
| `F-26h` | An **unordered** file may equally hold no records: an active file | `fcur_empty_sources`, `open_create`, `span_empty_file`, +1 more |
| `F-27` | Every pointer MUST be 8-aligned and lie between the header and the | `inorder_trailer_negatives` |
| `F-28` | `zs_db_check_consistency` MUST verify that an in-order file's | `check_out_of_order_pointers` |
| `F-29` | Progress rule. Iteration computes the next offset from the current | `corpus_engine_from_file_not_config`, `malformed_never_hangs`, `span_progress` |
| `F-30` | Every length, offset and pointer MUST be bounds-checked against the | **none** |
| `F-31` | Opening an in-order file is O(1): validate the header, read the | `open_is_o1_in_records` |
| `F-32` | Every data record ends in a 4-byte checksum: the last 4 bytes of | `record_byte_layout_v2`, `corpus` |
| `F-32a` | A record's checksum MUST be verified when the record is | `read_verifies_record_csum`, `read_verifies_record_csum_unordered`, `record_csum_engine0` |
| `F-32b` | A record's checksum MUST NOT be verified during span replay | `record_csum_replay_no_truncate` |
| `F-32c` | A record copied byte-for-byte keeps a valid checksum only when | `convert_reencodes_engine_mismatch` |

## Database layout (`D-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `D-0` | The `<uuid>` in a filename is the **36-character lowercase hyphenated | `interop_constants_uuid` |
| `D-1` | Generations in filenames are **uppercase hexadecimal, zero-padded to | `filename_rejections`, `filenames` |
| `D-1a` | Data files carry **no extension**, so `zeroskip-*` stays | `filename_sort_property`, `filename_rejections`, `fileset_overlap_table` |
| `D-1b` | **The active file is named `zeroskip-<uuid>.current`.** It does | `filenames`, `filename_sort_property`, `filename_rejections` |
| `D-2` | `zeroskip-*` matches data files only and `zeroskip.*` matches | `filename_rejections`, `fileset_ignores_foreign`, `staging_names` |
| `D-3` | `zeroskip.lock` MUST be a distinct file that is never replaced. | `staging_names` |
| `D-3a` | It is created with the database (D-8a), and is created on open with | `lock_basic`, `open_lock_file_recreated` |
| `D-3b` | It MUST NOT be unlinked — not by the library, and not by anything | `never_unlinks_the_lock_file` |
| `D-3c` | The lock file is empty and its contents are never read. Nothing about | `lock_basic` |
| `D-4` | A file participates if its name matches `zeroskip-<uuid>-*` for this | `filename_rejections`, `fileset_ignores_foreign` |
| `D-4a` | On first open the UUID is not yet known, so it is **discovered**: take | `fileset_uuid_discovery`, `open_create`, `open_uuid_mismatch` |
| `D-5` | Resolution by scan. An output is renamed into place before its inputs | `fileset_overlap_table`, `filename_rejections`, `snapshot_resolves_overlap` |
| `D-5a` | The order the sweep takes the files in MUST be, for each file, its | `fileset_overlap_table`, `fileset_mid_conversion_stable` |
| `D-5b` | The order MUST be derived from each file's range and kind, as D-5a | `fileset_first_vs_last`, `fileset_mid_conversion_stable` |
| `D-5c` | A **partial** overlap, where neither range contains the other, cannot | `fileset_gaps`, `mp_racing_removers` |
| `D-6` | Completeness. A set is complete if and only if the scan of D-5 consumes | `convert_steady_state`, `fileset_gaps`, `mp_repack_and_writer_concurrent`, +1 more |
| `D-7` | `readdir` is not atomic, so a scan may miss an entry and produce a set | `fileset_gaps` |
| `D-8` | Creating a file **is** publishing it, since the directory is the truth. | `open_create` |
| `D-8a` | Creating a database. With `ZS_CREATE` and no existing directory, or a | `fileset_ignores_foreign`, `open_create`, `snapshot_basic`, +1 more |
| `D-9` | An active file is **clean** if it has a valid header and zero or more | `check_unclean_reported`, `crash/crash_after_invalid_terminator`, `crash/snapshot_gap_retry`, +4 more |
| `D-9a` | A writer moves to a new file when the active file is not clean, or | `write_rollover` |
| `D-9b` | The next generation is one above the highest present in the directory. | `fileset_next_gen` |
| `D-9c` | Generations are never reused and never reset, not even by a repack | `fileset_next_gen`, `header_roundtrip` |
| `D-9d` | A writer MAY additionally treat the active file as due for rollover | `rollover_txns_seals_on_span_count`, `rollover_txns_counts_the_replay_window` |
| `D-10` | An active file with a corrupt header or zero length is treated as a | `file_bad_header`, `file_zero_length`, `open_bad_nonactive`, +3 more |
| `D-10a` | A **non-active** file with an invalid header MUST be **reported** | `file_bad_header`, `open_bad_nonactive`, `snapshot_bad_nonactive` |
| `D-10b` | An earlier version of D-10a made this fatal, which contradicts D-10 | `open_bad_nonactive`, `snapshot_bad_nonactive` |
| `D-11` | The writer never appends a pointer section to an unordered file. When | `write_rollover` |
| `D-12` | Immediate conversion. A writer that finds a **non-active unordered | `api_pointer_lifetime`, `conversion_avoids_the_repack_lock`, `convert_basic`, +2 more |
| `D-12a` | This is what keeps the steady state at **exactly one unordered file, | `convert_basic`, `convert_steady_state`, `write_rollover` |
| `D-12b` | A writer MUST convert the active file to its in-order form **before** | `convert_only_one_unordered_file`, `write_rollover` |
| `D-12c` | Conversion never takes the repack lock. It renames its output in | `conversion_avoids_the_repack_lock` |
| `D-12d` | Each conversion is bounded by `rollover_size` — sort the keys, write | `zsbench (store, rollover Nk)` |
| `D-13` | The private index MUST support point lookup, lower-bound seek and | `index_ordered_traversal` |
| `D-13a` | It reflects **committed spans only**, and for each key only its | `index_committed_only` |
| `D-13b` | A **writer is a reader that also maintains the active file's index | `index_delta` |
| `D-13c` | No shared state is mutated in place anywhere in the design: files are | `mp_reader_across_repack` |
| `D-13d` | The cost is that each snapshot replays the unordered files it | `zsbench (snapshot open)` |
| `D-14` | Within a file the newest version of a key wins — the highest offset | `fcur_no_duplicate_keys`, `read_d14f_duplicate_across_three_files`, `read_model` |
| `D-14a` | Point lookups, cursors and range scans MUST all resolve visibility | `fcur_uniform` |
| `D-14b` | Searching one file for a key: | `fcur_empty_sources`, `inorder_empty`, `read_arrangements` |
| `D-14c` | A read never follows a chain, because there is no chain to follow: | `reads_never_consult_ancestors` |
| `D-14d` | Point lookup Walk the sources in priority order; in each, search | `inorder_probe_ends_agrees`, `zsbench` |
| `D-14e` | Iteration A cursor holds one **per-file cursor** per source, each | `fcur_deletions_visible`, `fcur_empty_sources`, `open_uuid_mismatch`, +2 more |
| `D-14f` | Because the tie-break is part of the sort, cursors on the emitted key | `read_d14f_duplicate_across_three_files`, `read_prefix_across_files` |
| `D-14g` | The write transaction's own records sort as though they had a | **none** |
| `D-14h` | A per-file cursor never yields the same key twice: an in-order file | `fcur_no_duplicate_keys`, `index_delta_shadows_base` |
| `D-14j` | **Liveness.** What a cursor observes of writes made while it runs | `cursor_sees_own_handle_writes`, `txn_cursor_sees_own_writes`, `txn_cursor_view_is_fixed`, +1 more |
| `D-14j-a` | A source's records MUST NOT be yielded twice because of a write | `txn_cursor_no_duplicate_on_write` |
| `D-14j-b` | After observing a change, a cursor resumes at the first key | `cursor_start_key_survives_refresh`, `txn_cursor_store_behind_not_yielded`, +1 more |
| `D-14i` | Picking the next record is O(1) and re-sorting one cursor is O(k) | `read_cursor_invariant` |
| `D-15` | The repacker **never touches the active file**, and never touches an | `repack_never_touches_unordered`, `repack_selection` |
| `D-16` | The repacker works **only on in-order files**; converting unordered | `corpus`, `repack_cascade`, `repack_never_touches_unordered`, +1 more |
| `D-16a` | The two jobs divide by whether a file has an `end`, which is what | `mp_repack_and_writer_concurrent` |
| `D-16b` | A cascade writes one output for the whole selected set, not one per | `repack_selection`, `zsbench` |
| `D-16c` | Because D-12b keeps in-order files as a contiguous prefix, the | `convert_only_one_unordered_file` |
| `D-16e` | **Who runs it.** A writer SHOULD run the cascade itself, at the start of | `autorepack_bounds_the_file_count`, `autorepack_only_at_a_new_generation`, `noautorepack_leaves_the_files` |
| `D-16d` | Step 2's comparison MUST include equality. Rollover produces files of | `repack_selection` |
| `D-17` | The output holds **exactly one record per key**, built from the live | `check_out_of_order_pointers`, `fcur_no_duplicate_keys`, `repack_one_record_per_key` |
| `D-17b` | A repack MUST consider the versions of a key in a **total order**, | `repack_version_order` |
| `D-18` | Per key, where **below** means "in a file whose range lies entirely | `repack_d18_table` |
| `D-19` | A tombstone is retained **if and only if the newest record for its | `repack_d18_table`, `repack_d19a_resurrection`, `repack_d19_newer_file_recreates` |
| `D-19a` | The emitted record MUST be written even when a newer file already | `repack_d19a_shadowed` |
| `D-19b` | D-19 states a **predicate, not an algorithm**. An implementation MAY | **none** |
| `D-19c` | The test MAY err toward **retention**, never toward dropping. A | **none** |
| `D-20` | Inputs are iterated in key order: from the pointer section where present, | `convert_basic, repack_one_record_per_key` |
| `D-20a` | A staging file MUST be created with `O_CREAT\|O_EXCL`, advancing `<n>` | `convert_staging_exclusive` |
| `D-20b` | Before writing the output, the writer MUST verify the checksums | `repack_verifies_inputs`, `repack_verifies_inputs_nocsum`, `seal_verifies_spans_nocsum` |
| `D-21` | The output is written to `zeroskip.tmp.<pid>.<n>` and `rename`d to | `repack_selection` |
| `D-22` | The output may legitimately contain **zero records**, in the form | `inorder_empty`, `repack_empty_output` |
| `D-23` | Removing a data file — a converted unordered file, repack inputs, or | `convert_basic`, `convert_remove_refuses_when_needed`, `mp_racing_removers`, +1 more |
| `D-23a` | Tiling alone is **not** a sufficient test, because D-6 measures | `crash/snapshot_gap_retry` |
| `D-24` | `zs_db_should_repack` reports whether D-16 currently has work. | `repack_selection` |
| `D-25` | **Sealing.** A writer MAY convert the **active** file on demand, holding | `seal_converts_the_active_file` |
| `D-25a` | Sealing MUST NOT create a replacement active file. A conversion | `seal_creates_no_new_generation` |
| `D-25b` | Sealing is a no-op, and NOT an error, when there is no active file, | `seal_noop_cases` |
| `D-25c` | An unclean active file (D-9) MAY be sealed. The conversion reads to | `seal_unclean_active_file` |
| `D-14k` | **Reverse iteration.** A cursor MAY traverse in descending key order. | `cursor_reverse_walks_everything`, `cursor_reverse_prefix`, `cursor_reverse_own_writes` |
| `D-14l` | **Predecessor lookup.** The record with the largest key ≤ K (or | `cursor_reverse_tombstones`, `fetchprev_basic` |
| `D-25d` | A writer SHOULD seal at the end of any commit that leaves the | `commit_seals_oversized_active`, `commit_below_rollover_stays_unordered` |
| `D-25e` | A writer sealing under D-25d SHOULD NOT publish a pointer table | `seal_at_commit_skips_table_publish` |
| `D-26` | **Compaction.** An implementation MAY merge the **entire** database into | `compact_to_one_file` |
| `D-26a` | D-16's geometric selection does NOT apply to compaction. That rule | `compact_ignores_geometric_selection` |
| `D-26b` | Adjacency is why compaction merges every maximal **run** of adjacent | `compact_reports_and_fails_on_bad_file` |
| `D-27` | Because a compaction output spans the whole generation interval, | `compact_drops_tombstones` |
| `D-28` | Compaction is **best effort in action and strict in reporting**: it | `compact_reports_and_fails_on_bad_file` |
| `D-29` | Compaction is unbounded: it rewrites the whole database in one | `zsbench` |

## Concurrency and durability (`C-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `C-1` | Three byte-range locks on `zeroskip.lock`: | `lock_basic, lock_byte_offsets` |
| `C-1a` | The write and repack locks never contend, because the two jobs | `conversion_avoids_the_repack_lock`, `lock_excludes_other_process`, `mp_repack_and_writer_concurrent` |
| `C-1b` | Publishing a new file needs **no lock at all**: `rename` into the | `mp_repack_and_writer_concurrent` |
| `C-1c` | The **remove** lock makes verifying completeness and unlinking one | `mp_removal_needs_the_lock` |
| `C-1d` | **Lock ordering.** The locks form one total order: repack → write → remove. | `lock_basic`, `compact_lock_order` |
| `C-1e` | The primitive and the byte offsets are normative, because | `lock_byte_offsets`, `lock_never_uses_flock` |
| `C-1f` | `fcntl` locks are per-process, not per-thread: two threads of one | `lock_byte_offsets`, `lock_dies_with_process`, `lock_two_handles_one_process`, +1 more |
| `C-1g` | `fcntl` locks are released by closing **any** descriptor for the file | `one_lock_descriptor` |
| `C-1h` | Locks across databases. C-1d orders the locks within one database. | **none** |
| `C-1i` | An implementation MAY use `F_OFD_SETLK` instead of `F_SETLK`, but only | `lock_two_handles_one_process` |
| `C-1j` | **Same-process exclusion.** An implementation SHOULD exclude two handles | `lock_two_handles_one_process`, `lock_registry_keys_on_inode`, `lock_registry_is_per_database` |
| `C-2` | Readers take **no lock**. | `mp_two_writers`, `mp_writer_and_readers` |
| `C-3` | A file is published by writing it under a staging name, then | `convert_basic, mp_reader_across_repack` |
| `C-4` | Taking a snapshot. The protocol is: | `crash/crash_after_invalid_terminator`, `file_open_failures`, `fileset_gaps`, +1 more |
| `C-4a` | Completeness. Step 2's tiling check *is* the completeness proof: every | `crash/snapshot_gap_retry` |
| `C-4b` | Why a retry suffices. `readdir` may miss entries, and a file may be | `crash/snapshot_gap_retry` |
| `C-4c` | Immutability of what was opened. In-order files are never modified. | `mp_writer_and_readers`, `snapshot_boundary` |
| `C-4c-a` | Immutability makes a file **shareable between snapshots**, which | `snapshot_reuses_immutable_files`, `fcache_sweeps_superseded_files` |
| `C-4d` | Every index is private (D-13c), so a snapshot needs no synchronisation | `mp_writer_and_readers` |
| `C-4f` | Concurrent visibility. A reader scanning the active file may meet a | `mp_reader_sees_torn_span`, `span_terminator_without_data` |
| `C-4g` | Lifetime. Once its descriptors are open a packer may (subject to | `mp_reader_across_repack`, `snapshot_refcount` |
| `C-4h` | Termination. A retry happens only when the file set changed during | `snapshot_retries_and_bounds` |
| `C-4i` | Freshness. Beginning a transaction — shared or exclusive — MUST | `mp_read_sees_other_process_commit`, `read_freshens_after_rollover`, `probe_no_change_reuses_snapshot`, `write_begin_reuses_snapshot`, `failed_refresh_keeps_probe_stale` |
| `C-5` | The accepted cost of C-4g is that disk space is held until the last | `mp_reader_across_repack` |
| `C-6` | Directory durability. After creating a **data file** (a new active | `crash/dirsync_justifies_c6` |
| `C-6a` | A directory sync is **not** required after `unlink`. If a removed name | `crash/dirsync_justifies_c6` |
| `C-6b` | Output durability. A conversion or repack output MUST be | `crash/nosync_structural_syncs`, `crash/sync_failure_every_point` |
| `C-7` | Two gates per commit. Under default durability a commit is: | `crash/crash_at_every_call`, `mp_reader_sees_torn_span`, `zsbench` |
| `C-7a` | Together the gates make "a valid terminator implies its data is | `crash/crash_nosync_mode`, `crash/dirsync_justifies_c6`, `crash/sync_failure_every_point`, +2 more |
| `C-7b` | The cost is two syncs per commit rather than one. It is paid per | `zsbench (store, N per txn)` |
| `C-7c` | `ZS_NOSYNC` omits **both** gates — and nothing else. The structural | `crash/crash_nosync_mode`, `crash/nosync_structural_syncs` |
| `C-8` | An aborted transaction appends a `ROLLBACK` and syncs **neither** gate. | `write_abort`, `stream_abort_writes_rollback` |
| `C-8a` | The unsynced `ROLLBACK` is still **ordered** ahead of any later | `stream_abort_writes_rollback`, `crash/sync_failure_gate1` |

## Open and recovery (`R-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `R-1` | Open is C-4: scan the directory, resolve enclosures, check the tiling, | `open_create, crash/crash_at_every_call` |
| `R-2` | Live data is the union of records in spans with `COMMIT` terminators; | `span_rollback` |
| `R-3` | A reader MUST NOT write **to the database**. Opening a damaged | `convert_readonly_does_nothing`, `corpus_engine_from_file_not_config`, `open_readonly_no_side_effects`, `idxcache_rejects_db_dir`, +1 more |
| `R-4` | There is no in-place repair. A file that is not clean is simply | `crash/crash_after_invalid_terminator`, `span_bad_header_and_kind`, `span_torn_tail`, +1 more |
| `R-5` | A crash during a repack or conversion leaves either a staging file, | `snapshot_resolves_overlap`, `staging_names` |

## Pointer table cache (`P-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `P-1` | A pointer table covers exactly one **unordered** file, identified by | `idxcache_only_unordered_files` |
| `P-2` | Tables live in a **cache root** named by the caller, or — when the | `idxcache_rejects_db_dir` |
| `P-2a` | Under a configured root, the tables for a database live in the | `idxcache_threshold_defaults`, `corpus_index_table` |
| `P-2b` | With A-8a's flag, the cache directory is `zeroskip.cache` inside | `index_local_publishes`, `index_local_readonly_creates_nothing`, `index_local_sweeps_foreign_uuid` |
| `P-3` | A published table is named `zeroskip.index-<uuid>-<GEN8hex>`, using | `idxcache_published_name` |
| `P-4` | A table is published by writing a complete file under a staging name | `idxcache_publishes_by_rename` |
| `P-5` | A table is a 96-byte header, then `nptrs` 8-byte little-endian record | `idxcache_header_byte_layout` |
| `P-6` | The magic is the 16 bytes | `idxcache_header_byte_layout` |
| `P-7` | A table's checksums use **the engine named by the covered data file's | `idxcache_uses_file_engine` |
| `P-8` | `valid_upto` is the data-file offset the table covers. It MUST be a | `idxcache_valid_upto_is_span_boundary` |
| `P-9` | The offsets are the record offsets of every distinct key committed | `idxcache_matches_full_build`, `idxcache_open_agrees`, `corpus_index_table` |
| `P-10` | `term_off` is the offset of the terminator immediately below | `idxcache_rejects_bad_term_binding` |
| `P-11` | A reader MUST use a table only if **all** of the following hold, and | `idxcache_rejection_rules` |
| `P-12` | Having accepted a table, a reader takes its offsets as the index's | `idxcache_open_agrees`, `idxcache_valid_upto_is_span_boundary` |
| `P-13` | After building or extending an index over an unordered file, a | `idxcache_threshold` |
| `P-14` | A table MUST NOT be `fsync`ed before publication. It is rebuildable, | `crash/idxcache_no_fsync_on_publish` |
| `P-15` | A failure to publish MUST NOT fail the operation that triggered it. | `idxcache_publish_failure_is_not_fatal` |
| `P-16` | A process MAY unlink tables in the cache directory whose uuid matches | `idxcache_sweeps_dead_generations` |
| `P-17` | P-10's binding detects a data file whose covered prefix has changed. | `idxcache_rejects_bad_term_binding` |

## Salvage (`S-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `S-1` | Salvage reads a source directory and writes a **new** database | `salvage_never_writes_the_source` |
| `S-2` | Salvage MUST NOT apply D-5's overlap resolution or D-6's tiling check. | `salvage_across_a_missing_generation` |
| `S-3` | Files are processed **oldest first**: by `start` ascending, and for | `salvage_newest_version_wins` |
| `S-4` | The source's comparator does not affect the output. The output is | `salvage_comparator_mismatch_reported` |
| `S-5` | A file whose header does not validate is still processed. Its | `salvage_invalid_header` |
| `S-6` | For an in-order file the pointer section MUST be ignored and the | `salvage_ignores_pointer_section` |
| `S-7` | **Resynchronisation.** On reaching a span that does not validate, | `salvage_resyncs_after_a_bad_span` |
| `S-8` | The span that failed cannot be verified — its terminator is what would | `salvage_unverified_needs_the_flag` |
| `S-8a` | An in-order file has no commitment question: it was written whole | `salvage_verifies_records_inorder` |
| `S-9` | A **rolled-back** span MUST NOT be recovered under any option. | `salvage_never_recovers_rollback` |
| `S-10` | Salvage MUST report, per key, where the record it recovered may be | `salvage_reports_maybe_stale` |
| `S-11` | Reporting MUST be structured — a kind, a location, and a key where | `salvage_event_fields` |
| `S-12` | Salvage MUST NOT reconstruct a missing generation's contents, invent | `salvage_never_writes_the_source` |

## C binding (`A-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `A-0` | Every read and write entry point exists in three forms — on the | `api_three_forms` |
| `A-1` | `store` with `val == NULL` writes a deletion; with a non-NULL | `record_byte_layout`, `span_basic`, `tool.sh`, +1 more, `empty_value_is_not_null_on_read` |
| `A-1a` | A write inside a transaction is visible to subsequent reads on that | `tool.sh`, `write_txn_isolation` |
| `A-1b` | The `*_delete` forms are **macros** over `store` and | `api_cursor_replace`, `api_three_forms`, `write_basic` |
| `A-1d` | `ZS_IFCHANGED` on a store means **write nothing if the stored state | `ifchanged_writes_nothing`, `read_seek_and_flags` |
| `A-1c` | A transaction supports **any number of cursors open at once**, and | `txn_many_cursors`, `txn_insert_select_self` |
| `A-2` | There is no `yield` call and no yield flags: readers hold no lock, so | `no_yield_and_no_mvcc` |
| `A-3` | There is no MVCC flag. Snapshot isolation is the only read mode, | `no_yield_and_no_mvcc` |
| `A-4` | Returned key and value pointers remain valid for the lifetime of the | `api_pointer_lifetime`, `txn_fetch_survives_overwrite`, `reverse_a4_lifetime` |
| `A-4a` | A-4 binds across a **snapshot swap**. A transaction or cursor may | `a4_borrow_survives_new_generation`, `a4_borrow_survives_cursor_swap`, `a4_borrow_survives_shared_snapshot_swap` |
| `A-4b` | `ZS_EPHEMERAL` on a fetch **weakens A-4 for that result**: the key and | `ephemeral_matches_durable`, `ephemeral_avoids_the_flush`, `ephemeral_rejected_on_cursor` |
| `A-5` | `ZS_SHARED` is read-only and MUST NOT write (R-3). | `api_readonly`, `open_readonly_no_side_effects` |
| `A-6` | A `ZS_CSUM_*` flag chooses the engine for files this handle **creates**; | `corpus_engine_from_file_not_config`, `file_engine_from_header`, `interop_constants_csum`, +1 more |
| `A-7` | `zs_compar` returns negative, zero or positive like `memcmp`, but MUST | `interop_constants_compar` |
| `A-8` | `index_dir` names the pointer table cache **root** (§8); tables live | `idxcache_rejects_db_dir`, `idxcache_publish_failure_is_not_fatal` |
| `A-8a` | `ZS_INDEX_LOCAL` selects P-2b's in-database cache directory, | `index_local_and_dir_is_badusage`, `index_local_publishes` |
| `A-9` | `index_threshold` is P-13's threshold in bytes. Zero selects an | `idxcache_threshold_defaults` |
| `A-10` | `zs_db_seal` performs D-25. It returns `ZS_OK` for each of D-25b's | `seal_noop_cases`, `seal_readonly` |
| `A-11` | `zs_db_compact` performs D-26, returning `ZS_OK` only when the | `compact_to_one_file`, `compact_reports_and_fails_on_bad_file` |
| `A-12` | The point-lookup forms on `zs_db_fetch` and `zs_txn_fetch`: | `fetchprev_basic`, `fetchprev_sees_txn_writes`, `fetchnext_inclusive`, `fetchnext_inclusive_sees_txn_writes`, `reverse_rejected_compositions` |
| `A-13` | `ZS_REVERSE` on `zs_db_begin_cursor` and `zs_txn_begin_cursor` | `cursor_reverse_seek_and_skiproot`, `reverse_a4_lifetime`, `reverse_rejected_compositions` |
| `A-14` | `ZS_NOAUTOREPACK` suppresses D-16e, so the repack cascade runs only | `noautorepack_leaves_the_files` |
| `A-15` | `rollover_txns` is D-9d's span bound. Zero selects an | `rollover_txns_seals_on_span_count`, `rollover_txns_counts_the_replay_window` |

## Conformance suite (`T-n`)

| Req | Requirement | Enforced by |
|---|---|---|
| `T-0` | The corpus is language-neutral. `tests/corpus/` holds data files | `corpus`, `dump_shows_rollback` |
| `T-0a` | Driver contract. Each implementation MUST provide a small executable | `dump_line_format`, `tool.sh` |
| `T-0b` | Corpus workloads avoid writer-choice-dependent bytes. A buffered | `corpus` (no case stores or deletes one key twice in a transaction) |
| `T-1` | to T-11 are per-implementation tests , run | `corpus`, `dump_shows_rollback`, `open_with_uuid`, +1 more |
| `T-2` | Magic and versions. All 16 magic bytes required (F-6); each | `magic_designed_corruptions`, `staging_names` |
| `T-2a` | The trailer. Opening an in-order file depends entirely on it, so: the | `inorder_trailer_negatives` |
| `T-2b` | Type byte validity. All 256 byte values fed as a record type, asserting | `header_bounds_and_ranges`, `type_byte_validity` |
| `T-2c` | Interoperability constants. The values two implementations must agree on | `filenames`, `overflow_guards`, `strerror` |
| `T-3` | Malformed input. Every golden file truncated at *every byte offset*, | `corpus_engine_from_file_not_config`, `malformed_bitflips`, `malformed_truncation`, +1 more |
| `T-4` | Behavioural. Ordering, prefix scans, cursor replace, | `header_bounds_and_ranges`, `read_model`, `span_long_terminator`, +1 more |
| `T-5` | Model-based. Randomised operation sequences against an in-memory | `open_uuid_mismatch`, `read_model`, `read_prefix_across_files` |
| `T-5a` | Read paths under every file arrangement. The same assertions driven | `inorder_probe_ends_agrees`, `open_uuid_mismatch`, `read_arrangements` |
| `T-5b` | Cursor mechanics. The sorted-array invariant of D-14e asserted after | `open_uuid_mismatch`, `read_cursor_invariant`, `read_d14f_duplicate_across_three_files` |
| `T-6` | File states and encoding. That `end == 0` and `end != 0` files are | `check_noncanonical`, `inorder_kind_rules`, `inorder_ptrs64`, +3 more |
| `T-7` | Tombstone retention across repacks. For every arrangement of create, | `repack_d18_table`, `repack_version_order`, `repack_d19a_resurrection`, +2 more |
| `T-8` | Crash injection. A test build interposes `write`, `fdatasync`, `rename` | `crash/crash_after_invalid_terminator`, `crash/crash_leaves_unaligned_length`, `crash/sync_failure_every_point` |
| `T-8a` | Sync failure. The case C-7a exists for, which no crash test reaches: | `crash/dirsync_justifies_c6` |
| `T-9` | File set discovery. That the set and every range are derived from | `fcur_deletions_visible`, `filename_sort_property`, `fileset_first_vs_last`, +3 more |
| `T-10` | Multi-process. Real forked processes. A writer plus *N* readers | `malformed_never_hangs`, `mp_racing_removers` |
| `T-10a` | Steady state. That the number of unordered files returns to one after | `api_pointer_lifetime`, `convert_only_one_unordered_file`, `convert_steady_state` |
| `T-10b` | The snapshot protocol. Each step of C-4 attacked directly, since these | `crash/crash_after_invalid_terminator`, `malformed_never_hangs`, `mp_reader_sees_torn_span` |
| `T-11` | Traceability. `doc/conformance.md` maps every normative requirement | this document |
| `T-12` | to T-14 are cross-implementation tests , | **none** |
| `T-12a` | Byte-for-byte agreement. Given the same UUID and the same operation | `corpus_encode_byte_identical` — Local half only: byte-identical output from the same operations. The cross-implementation half needs a second implementation. |
| `T-13` | Cross-implementation concurrency. The tests most likely to find real | **none** |
| `T-14` | Two handles in one process. Two write handles on one database from a | `lock_two_handles_one_process` — Repurposed by the spec amendment in 6282469: asserts the implementation does NOT claim exclusion it cannot provide. |

## Gaps

Each of these is a deliberate, explained absence rather than an oversight.

- **`G-0`** — Architectural: nothing in the format depends on mmap. This implementation *does* mmap, so it cannot demonstrate the alternative. A second implementation reading with ordinary reads is what would establish it (T-12).
- **`F-26a`** — Not yet attributed.
- **`F-26b`** — Not yet attributed.
- **`F-30`** — Not yet attributed.
- **`D-14g`** — Not yet attributed.
- **`D-19b`** — Permissive: it states what an implementation MAY do (any means of evaluating D-19's predicate), so there is nothing to enforce. What *is* enforced is the predicate itself, by `repack_d18_table`.
- **`D-19c`** — Permissive in the same way, and the direction it permits is the safe one. A test could show that repacking two ranges in either order leaves a conforming result, but it would be asserting the absence of a constraint.
- **`C-1h`** — Documentation only: the library cannot see across two databases, so a caller holding locks on several must impose its own order. Stated in zeroskip.h and CLAUDE.md; nothing here can enforce it.
- **`T-12`** — Needs a second implementation. The corpus (T-0) and driver contract (T-0a) it requires are both in place.
- **`T-13`** — Needs a second implementation. zstool provides hold-write for it.

## What this implementation does not claim

- **Engine 2** is outside the shared corpus (F-5d): a file written under a
  caller-supplied checksum is readable only by a caller supplying the same
  function, so it cannot be validated by anyone else.
- **Thread-safe handles.** Removed from the spec in `6282469` after measurement
  showed the in-process mutex did not deliver what G-5 appeared to promise.
  Two threads sharing ONE handle remain the caller's problem. Two separate
  handles on one database are a different question and are excluded (C-1j),
  by `F_OFD_SETLK` where the platform has it and by a registry keyed on the
  lock file's inode otherwise.
- **The cross-implementation tests** (`T-12`, `T-13`) need a second
  implementation. Everything they depend on — the language-neutral corpus and
  the driver contract — is in place.

