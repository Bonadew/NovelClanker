# Novel storage format

A novel is a [RocksDB](https://rocksdb.org) database, i.e. a folder (conventionally named `*.novel`).
**Every scene's text is a separate entry**; acts and chapters only hold a title and the ordered ids of their
children, so restructuring never rewrites scene text. Ids are unique across all kinds and never reused;
`<id>` is zero-padded to 20 digits:

| Key                   | Value                                   |
|-----------------------|-----------------------------------------|
| `novel/format`        | format version (`3`)                    |
| `novel/next_id`       | next unused id                          |
| `novel/acts`          | act ids in order, comma-separated       |
| `act/<id>/title`      | act title                               |
| `act/<id>/chapters`   | chapter ids in order                    |
| `act/<id>/brief`      | what the act is for (optional)          |
| `chapter/<id>/title`  | chapter title                           |
| `chapter/<id>/scenes` | scene ids in order                      |
| `chapter/<id>/brief`  | what the chapter is for (optional)      |
| `scene/<id>/title`    | scene title                             |
| `scene/<id>/text`     | scene Markdown                          |
| `scene/<id>/brief`    | what happens in the scene (optional)    |
| `novel/synopsis`      | synopsis, exactly as written (optional) |
| `novel/plan_model`    | planner model override (optional)       |
| `novel/plan_template` | plan template (optional; default three acts) |
| `novel/plans`         | plan ids in order                       |
| `plan/<id>/title`     | plan title                              |
| `plan/<id>/text`      | plan Markdown                           |
| `plan/<id>/model`     | generating model; absent if by hand     |
| `plan/<id>/template`  | template it was generated with (optional) |
| `plan/<id>/chat`      | conversation about the plan (JSON)      |

Novels from earlier versions (format 2, with a single `novel/plan`) are upgraded when opened: that plan
becomes "Plan 1". Its model wasn't recorded then, so it shows as "(by hand)".

Writes are synced to disk and multi-key changes are atomic (RocksDB write batches). RocksDB locks an open
novel, so only one NovelWriter can have it open at a time. See [src/NovelStore.h](../src/NovelStore.h).
