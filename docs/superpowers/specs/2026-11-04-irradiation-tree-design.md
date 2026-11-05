# Data browser: irradiation tree filter: design

Date: 2026-11-04. Status: design agreed with developer 2026-11-04 (mock D of
filter brainstorm).

Builds on `2026-10-09-data-browser-search-display-design.md` §3.3 (filter
lists, `FacetBox`) and `2026-10-19-browser-selection-column-design.md`.

## 1. Problem

- Browser filter lists (`apps/nobion/src/data_browser_window.cpp`,
  `kFacets`) tick names. Level list ticks `lv.name`: "A" is level A of every
  irradiation. No way to pick level A of NM-300 only.
- Irradiation, Level, Identifier are three flat lists, AND'd: relation
  between them (identifier sits in a level of an irradiation) not shown.
- Identifier row shows identifier only; sample and project not beside it.

## 2. Goal

One tree, irradiation ▸ level ▸ identifier, replaces Irradiation, Level and
Identifier lists. Tick means node, not name.

Developer's choices (2026-11-04):

- Tree replaces those three lists. Analysis type, Spectrometer, Sample, PI
  stay `FacetBox` lists.
- Identifiers with no irradiation position (blanks, air, cocktails) under a
  last node, "Not irradiated".
- Node reaches query by key, PI pattern: uuid on a source that keeps uuids
  (store), name elsewhere. Not expanded to identifiers.

## 3. Widget: `IrradiationTree`

New `apps/nobion/src/irradiation_tree.{hpp,cpp}`, beside `facet_box`.

- Folding section like `FacetBox`: header `QToolButton`, arrow, title
  "Irradiation", count of ticked nodes in title (`Irradiation (2)`), starts
  folded. Body: filter line, clear button (×), `QTreeWidget`.
- Rows: irradiation; level; identifier. Identifier row column 1 (0-based):
  `<sample> · <project>`. One missing: other alone, no `·`. Both missing:
  column empty.
  Last top-level node "Not irradiated", its identifiers under it directly.
- Order: irradiations by name; levels by name; identifiers by identifier.
  "Not irradiated" last.
- Tick boxes tri-state on irradiation, level, "Not irradiated":
  `Qt::PartiallyChecked` when some, not all, children ticked. User ticks
  only to checked or unchecked; tick on a parent sets all its children.
- Rows padded as `FacetBox` rows: `style::make_tick_list` on the tree.
- Filter line: case-insensitive substring of irradiation, level,
  identifier, sample or project. Row shown when it, a descendant or an
  ancestor matches (filter by irradiation name leaves its levels to tick),
  or when it is ticked or partly ticked; ancestors of a match expanded.
  Ticked row never hidden. Applied again after new rows, as `FacetBox`.
- `set_rows(rows)` keeps ticks by key. Ticked node no longer among rows
  stays, at end of its parent (or of top level), label it had: as
  `FacetBox::set_items`. Parent gone too is recreated as stand-in to hold it:
  stand-in lists only kept nodes, so it never reads `Qt::Checked` (partly
  ticked at most) and `selection()` keeps the child's key, not the parent's.
- Emits `changed()` on user tick, untick, clear. `set_rows` emits nothing.

### 3.1 Selection

`selection()` gives `processing::PositionSelection` (§4), read from ticks:
the highest fully ticked node is what is kept.

- Irradiation fully ticked → its key in `irradiations`; nothing under it
  listed.
- Else level fully ticked → its key in `levels`.
- Else identifier ticked → identifier in `identifiers`.
- "Not irradiated" fully ticked → `not_irradiated = true`; else its ticked
  identifiers in `identifiers`.
- Nothing ticked → `std::nullopt`.

Ticking every child of a level is the same selection as ticking the level.

## 4. Query: `processing`

`libs/processing/include/pychron/processing/source.hpp`:

```cpp
// The irradiation tree's ticks (irradiation tree design §4). An analysis
// matches when any one list matches; empty lists match nothing.
struct PositionSelection {
  std::vector<std::string> irradiations, levels;  // keys
  std::vector<std::string> identifiers;
  bool not_irradiated = false;
  friend bool operator==(const PositionSelection&, const PositionSelection&) = default;
};

struct BrowseQuery {
  ...
  std::optional<PositionSelection> positions;  // empty: tree filters nothing
};
```

- Key: uuid text on a source that keeps uuids (store); elsewhere
  irradiation name, and level `<irradiation>/<level>`.
- Within `positions`: OR. `positions` against every other field: AND, as
  lists are with each other.
- `irradiations`, `levels`, `identifiers` of `BrowseQuery` (by name) stay;
  browser stops filling them.

`AnalysisSummary` gains `irradiation_uuid`, `level_uuid`: uuid text from
store (`row_from` reads `ir.uuid`, `lv.uuid`), empty elsewhere, as
`principal_investigator_uuid`. Key of a summary (free functions beside
`facet_value`): `irradiation_key(s)` = `irradiation_uuid`, else
`irradiation`; `level_key(s)` = `level_uuid`, else `<irradiation>/<level>`,
empty when `level` empty. Record folder source needs no change.
`matches()` tests `positions` against those keys, `identifier`, and "not
irradiated" = `irradiation_key(s)` empty.

New on `IAnalysisSource`:

```cpp
// One row per identifier left by every filter of `query` but `positions`.
struct PositionRow {
  std::string irradiation_key, irradiation, level_key, level, identifier, sample, project;
  friend bool operator==(const PositionRow&, const PositionRow&) = default;
};
virtual Result<std::vector<PositionRow>> positions(const BrowseQuery& query) = 0;
```

- Not irradiated: keys, `irradiation`, `level` empty.
- Order: irradiation, level, identifier, by name; not irradiated last.
- `MemorySource`, `RecordDirectorySource`: from summaries, one shared helper
  beside `facet_values` (`position_rows(rows, q)`), `matches` with
  `positions` ignored.
- `StoreSource`: §5.

## 5. Store: `libs/persistence`

`BrowseFilter` gains:

```cpp
struct PositionFilter {
  std::vector<Uuid> irradiations, levels;
  std::vector<std::string> identifiers;
  bool not_irradiated = false;
};
std::optional<PositionFilter> positions;
```

`StoreSource` turns keys to `Uuid`; a key that is no uuid is an error
(`ErrorKind::Protocol`), never silently dropped.

### 5.1 Filter clause

One clause on analysis's own column, a `UNION` of one branch per non-empty
part (branch left out when its part is empty):

```sql
a.identifier_uuid IN (
  SELECT i.uuid FROM identifier i
    JOIN irradiation_position ip ON ip.uuid = i.position_uuid
    JOIN level lv ON lv.uuid = ip.level_uuid
   WHERE lv.irradiation_uuid IN (?, ...)
  UNION
  SELECT i.uuid FROM identifier i
    JOIN irradiation_position ip ON ip.uuid = i.position_uuid
   WHERE ip.level_uuid IN (?, ...)
  UNION
  SELECT uuid FROM identifier WHERE identifier IN (?, ...)
  UNION
  SELECT uuid FROM identifier WHERE position_uuid IS NULL)
```

- No `OR` across tables (`libs/persistence/AGENTS.md`, Queries).
- Branch indexes, all existing: `level` UNIQUE (`irradiation_uuid`, `name`);
  `irradiation_position` UNIQUE (`level_uuid`, `position`); `identifier`
  UNIQUE (`position_uuid`), UNIQUE (`identifier`).
- `positions` set with every part empty: clause matches nothing (`1 = 0`).
- Applied in page, count and facet statements (`build_where`); ignored by
  §5.2 only.
- No migration. `analysis.identifier_uuid` has no index; subquery is
  computed once and each analysis row only checks against it. Plan test
  (§7) holds this.

### 5.2 Listing statement

```sql
SELECT DISTINCT ir.uuid AS irradiation_uuid, ir.name AS irradiation,
       lv.uuid AS level_uuid, lv.name AS level,
       i.identifier AS identifier, s.name AS sample, p.name AS project,
       (ir.name IS NULL) AS unplaced
<kBrowseFrom, as from_for_aggregate>
<build_where, positions ignored>
ORDER BY (ir.name IS NULL), ir.name, lv.name, i.identifier
```

`unplaced` is selected only because PostgreSQL wants an `ORDER BY` expression
of a `SELECT DISTINCT` in the select list; not read.

New `Store::positions(const BrowseFilter&)` → `std::vector<PositionListing>`
(persistence's twin of `PositionRow`, uuids as `Uuid`). Both dialects, one
statement.

## 6. Window

`DataBrowserWindow`:

- `kFacets` loses `Irradiation`, `Level`, `Identifier`. `IrradiationTree`
  added after the remaining lists, before Rescan.
- `query()` sets `positions` from `tree->selection()`.
- `update_facets()` also calls `source_.positions(query())` and
  `tree->set_rows`. Error: tree emptied, logged as facets are.
- Accessor for tests: `irradiation_tree()`.

## 7. Cost

Measured before landing, numbers in commit message:

- Both statements (§5.1 with each branch, §5.2 with no other filter) under
  `EXPLAIN QUERY PLAN` and timed, on a copy of a real-size store (8.7
  thousand analyses, 15 thousand identifiers). Bad signs per
  `libs/persistence/AGENTS.md`.
- `IrradiationTree::set_rows` with 15 thousand rows, timed.
  If over 100 ms: identifier rows built only when their level is first
  expanded (filter line then searches rows' data, not items). Fallback
  named, not assumed.

## 8. Rules that must hold

- A ticked level is that irradiation's level only: two levels named "A"
  in two irradiations are two keys.
- Within tree: OR. Tree against other filters: AND.
- A tick is kept by key: a position added later under a ticked level or
  irradiation is matched.
- `positions()` ignores `positions`, as a facet ignores its own list.
- Store never filters positions by name.
- `tests/ingest/forwarding_store.hpp` (`ForwardingStore`) forwards
  `Store::positions` as it forwards `facet`.

## 9. Tests

- `tests/persistence`: §5.1 each branch alone; branches together (OR);
  tree with another list (AND); `not_irradiated`; two levels named "A" in
  two irradiations; all parts empty matches nothing. §5.2 rows, order,
  ignoring `positions`. On SQLite; PostgreSQL with `PYCHRON_TEST_PG_URL`.
  Plan test in `SchemaIndexes` style: no `SCAN` of `identifier`,
  `irradiation_position`, `level` in §5.1 subquery.
- `tests/processing`: same cases on `MemorySource` and record folder
  source (name keys); `position_rows` order and not-irradiated rows;
  `StoreSource` key that is no uuid → error.
- `tests/ui` (`test_data_windows.cpp`): tri-state from children; parent
  tick sets children; §3.1 selection at highest fully ticked node; filter
  line shows matches and their ancestors, never hides ticked row; ticks
  kept across `set_rows`, missing ticked node kept at end; header count;
  starts folded; window `query().positions` from ticks; Irradiation, Level,
  Identifier lists gone.

## 10. Out of scope

- Extra columns for Sample list (item 2 of 2026-11-04 request).
- Chip bar, counts per value, query line, timeline brush, saved views
  (mocks A, B, C, E, F).
- `elctl` filters; `BrowseQuery` name lists stay as they are.
- Positions inside a level shown by hole number.

## Open questions

None.
