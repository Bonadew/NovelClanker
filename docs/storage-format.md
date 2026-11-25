# Novel storage format

A novel is a [RocksDB](https://rocksdb.org) database, i.e. a folder (conventionally named `*.novel`).
**Every scene's text is a separate entry**; acts and chapters only hold a title and the ordered ids of their
children, so restructuring never rewrites scene text. Ids are unique across all kinds and never reused;
`<id>` is zero-padded to 20 digits:

| Key                   | Value                                   |
|-----------------------|-----------------------------------------|
| `novel/format`        | format version (`1`)                    |
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
| `novel/details/<name>` | book detail from Novel Setup (optional) |

Book details (`novel/details/<name>`, all optional, written together in one batch):

| Name              | Value                                                                         |
|-------------------|-------------------------------------------------------------------------------|
| `title`           | book title                                                                    |
| `subtitle`        | subtitle                                                                      |
| `series_name`     | series name                                                                   |
| `series_number`   | number in the series (digits)                                                 |
| `edition`         | edition number (digits)                                                       |
| `language`        | language, e.g. `English`                                                      |
| `author`          | primary author                                                                |
| `contributors`    | one per line: role and name, separated by a tab               |
| `description`     | description for the book's page                                               |
| `keywords`        | one per line, up to 7                                                         |
| `categories`      | one per line, up to 3                                                         |
| `publisher`       | publisher                                                                     |
| `isbn`            | ISBN                                                                          |
| `rights`          | `own` (own copyright) or `public_domain`                                      |
| `explicit`        | sexually explicit content: `yes` or `no`                                      |
| `reading_age_min` | youngest reading age: `0` (baby) to `18`                                      |
| `reading_age_max` | oldest reading age: `1` to `17`, or `18+`                                     |
| `epub_identifier` | the book's identifier for EPUB exports (`urn:uuid:...`), set on first export  |
| `spelling_dictionary` | the dictionary to spell check with, e.g. `en_GB` (Edit > Spelling)        |

Format 1 is the only format. A novel with any other format version is refused when opened, and left
unchanged.

Writes are synced to disk and multi-key changes are atomic (RocksDB write batches). RocksDB locks an open
novel, so only one NovelClanker can have it open at a time. See [src/NovelStore.h](../src/NovelStore.h).
