# Irradiation Tree Filter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the data browser's Irradiation, Level and Identifier filter lists with one irradiation ▸ level ▸ identifier tree whose ticks select nodes by key.

**Architecture:** The store gets a position filter (one `a.identifier_uuid IN (… UNION …)` clause) and a listing statement. `libs/processing` gets `PositionSelection` on `BrowseQuery`, keys on `AnalysisSummary`, and `IAnalysisSource::positions()` on all three sources. The UI gets an `IrradiationTree` widget beside `FacetBox`, wired into `DataBrowserWindow` in place of three facet boxes.

**Tech Stack:** C++20, Qt 6 Widgets, TinyORM on QtSql (SQLite and PostgreSQL), GoogleTest, Qt Test.

**Spec:** `docs/superpowers/specs/2026-11-04-irradiation-tree-design.md`

## Global Constraints

- No migration; no change under `libs/persistence/migrations`.
- No `OR` across tables in the filter clause; the `UNION` form of spec §5.1, with a branch left out when its part is empty.
- Within the tree, ticks combine with OR; the tree combines with every other filter by AND.
- On a store, a key is uuid text. On other sources it is the irradiation name, or `<irradiation>/<level>` for a level.
- `BrowseQuery::irradiations`, `levels`, `identifiers` (by name) stay and keep their meaning; the browser stops filling them.
- `positions()` ignores `BrowseQuery::positions`, as a facet ignores its own list.
- A key that is not a uuid, given to `StoreSource`, is an `ErrorKind::Protocol` error.
- Static analysis (`python3 tools/quality_check.py`) and `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui` are clean after each task that changes C++.
- Final run before merge: full `cmake --build --preset dev-ui` and `ctest --preset dev-ui`, then the full compiler sweep.

## Review Focus

1. Two irradiations, each with a level named "A": ticking NM-300/A leaves out NM-301/A on every source. Tests in Tasks 1 and 2.
2. A tree selection whose lists are all empty and `not_irradiated` false matches nothing. It must not turn into "no filter". Tests in Tasks 1 and 2; Task 3 guarantees `selection()` never builds one.
3. A ticked level that the other filters no longer list stays ticked, at the end of its parent, and the query still carries it. Test in Task 3.
4. "Not irradiated" ticked along with an irradiation: both sets of analyses are listed (OR), not neither. Tests in Tasks 1 and 2.
5. A store selection carrying a name key (a selection built for another source): an error, never a silently empty filter. Test in Task 2.

---

### Task 1: Store position filter and listing

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/store.hpp` (`BrowseFilter`, `BrowseRow`, `Store`)
- Modify: `libs/persistence/src/sql/browse.hpp` (select `ir.uuid`, `lv.uuid`; listing and branch SQL)
- Modify: `libs/persistence/src/browse.cpp` (`build_where`, `row_from`, `positions`)
- Modify: `libs/persistence/src/store_impl.hpp`, `libs/persistence/src/store.cpp` (declare, forward)
- Modify: `tests/ingest/forwarding_store.hpp` (forward `positions`)
- Test: `tests/persistence/test_browse.cpp`, `tests/persistence/test_schema.cpp`

**Interfaces:**
- Produces, in `pychron::persistence`:
  ```cpp
  struct PositionFilter {
    std::vector<Uuid> irradiations, levels;
    std::vector<std::string> identifiers;
    bool not_irradiated = false;
  };
  // BrowseFilter: std::optional<PositionFilter> positions;
  // BrowseRow:    std::optional<Uuid> irradiation_uuid, level_uuid;
  struct PositionListing {
    std::optional<Uuid> irradiation_uuid, level_uuid;
    std::string irradiation, level, identifier, sample, project;
    friend bool operator==(const PositionListing&, const PositionListing&) = default;
  };
  // Store: virtual Result<std::vector<PositionListing>> positions(const BrowseFilter& filter) = 0;
  // detail: Result<BrowseStatement> positions_statement(Db&, Dialect, const BrowseFilter&);
  ```

- [ ] **Step 1: Write the failing tests** in `test_browse.cpp`, on `BrowseTest` (fixture: NM-300 level A holds `77000`; `66574` air and `66573` blank have no position). In each test's setup, add irradiation `NM-301` with a level `A` and position 1 holding identifier `88000`, plus one unknown analysis `88000-01`.
  - `TEST_P(BrowseTest, PositionsFilterByLevelKeyNotName)`: `positions = {.levels = {lab_.level}}` → rows are exactly a1, a2 (not 88000-01).
  - `TEST_P(BrowseTest, PositionsBranchesCombineByOr)`: `{.irradiations = {nm301}, .not_irradiated = true}` → 88000-01, a3, a5 (a4 is invalid and hidden by the default tag exclusion); `{.identifiers = {"77000"}}` → a1, a2.
  - `TEST_P(BrowseTest, PositionsCombineByAndWithOtherFilters)`: `positions = {.not_irradiated = true}`, `analysis_types = {"air"}` → a3 only. With only `positions = {.not_irradiated = true}`, `facet(BrowseFacet::AnalysisType, f)` gives `air`, `blank_unknown` (the lists respect the tree).
  - `TEST_P(BrowseTest, AnEmptyPositionSelectionMatchesNothing)`: `positions = PositionFilter{}` → no rows; count is 0.
  - `TEST_P(BrowseTest, PositionListingIgnoresItsOwnFilterAndOrders)`: `store_->positions(f)` with `f.positions = {.levels = {lab_.level}}` still lists `88000`. Rows in order: NM-300/A/77000 (sample `FC-2`, project `Fish Canyon`, uuids set), NM-301/A/88000, then 66573 and 66574 with the irradiation and level uuids unset and names empty.
  - `TEST_P(BrowseTest, BrowseRowsCarryIrradiationAndLevelUuids)`: a1's row has `irradiation_uuid == lab_.irradiation` and `level_uuid == lab_.level`; a3's has neither.
- [ ] **Step 2: Write the failing plan test** `TEST(SchemaIndexes, APositionSelectionIsFoundByIndexes)` in `test_schema.cpp`. It builds `detail::browse_page_statement` with every part of `positions` set, runs `EXPLAIN QUERY PLAN`, and asserts the plan has no `SCAN identifier`, `SCAN irradiation_position`, `SCAN ip`, `SCAN level`, `SCAN lv` and no `SCAN i ` (print the plan on failure, as `AnAnalysissReferencesAreFoundByScope` does).
- [ ] **Step 3: Run them to verify they fail**
  Run: `cmake --build --preset dev --target pychron_persistence_tests && build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='*Position*:*IrradiationAndLevelUuids*'`
  Expected: compile errors (no `PositionFilter`), then FAIL once declared.
- [ ] **Step 4: Implement.** `build_where` gains a last parameter `bool with_positions = true`; when it is true and `filter.positions` is set, it appends the §5.1 clause. Each branch's SQL is a constant in `sql/browse.hpp`. With all parts empty, append `1 = 0`. `positions_statement` is the §5.2 statement: `from_for_aggregate`, `build_where(..., std::nullopt, false)`, and `ORDER BY (ir.name IS NULL), ir.name, lv.name, i.identifier`. `row_from` reads the two new uuid columns, which are added to `kBrowseSelect` as `ir.uuid AS irradiation_uuid, lv.uuid AS level_uuid`. Forward `positions` in `store.cpp` and in `ForwardingStore`.
- [ ] **Step 5: Run tests to verify they pass**, the new ones and `BrowseTest.*`, `SchemaIndexes.*`, `StoreTest.*`.
  Run: `build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='BrowseTest*:SchemaIndexes.*'`
  Expected: PASS. Also run once with `PYCHRON_TEST_PG_URL` set (rebuild first, see `libs/persistence/AGENTS.md`).
- [ ] **Step 6: Static analysis**, `python3 tools/quality_check.py`: 0 findings.
- [ ] **Step 7: Commit**
  ```bash
  git commit -m "feat(persistence): filter and list analyses by irradiation position"
  ```

### Task 2: Selection, keys and `positions()` in processing

**Files:**
- Modify: `libs/processing/include/pychron/processing/source.hpp`, `libs/processing/src/source.cpp`
- Modify: `libs/processing/adapters/records/include/pychron/processing/record_source.hpp`, `libs/processing/adapters/records/src/record_source.cpp`
- Modify: `libs/processing/adapters/store/include/pychron/processing/store_source.hpp`, `libs/processing/adapters/store/src/store_source.cpp`
- Test: `tests/processing/test_units.cpp`, `tests/processing/test_record_source.cpp`, `tests/processing/test_store_source.cpp`

**Interfaces:**
- Consumes: Task 1's `PositionFilter`, `PositionListing`, `Store::positions`, `BrowseRow::irradiation_uuid/level_uuid`.
- Produces, in `pychron::processing`:
  ```cpp
  struct PositionSelection { std::vector<std::string> irradiations, levels, identifiers; bool not_irradiated = false;
                             friend bool operator==(const PositionSelection&, const PositionSelection&) = default; };
  // BrowseQuery:     std::optional<PositionSelection> positions;
  // AnalysisSummary: std::string irradiation_uuid, level_uuid;
  std::string irradiation_key(const AnalysisSummary& s);
  std::string level_key(const AnalysisSummary& s);
  struct PositionRow { std::string irradiation_key, irradiation, level_key, level, identifier, sample, project;
                       friend bool operator==(const PositionRow&, const PositionRow&) = default; };
  std::vector<PositionRow> position_rows(const std::vector<AnalysisSummary>& rows, const BrowseQuery& q, double newest);
  // IAnalysisSource: virtual Result<std::vector<PositionRow>> positions(const BrowseQuery& query) = 0;
  ```

- [ ] **Step 1: Write the failing tests.** In `test_units.cpp`, build summaries by hand: `77000` in NM-300/A, `88000` in NM-301/A, `66574` with no irradiation.
  - `TEST(MemorySource, AKeyIsTheNameWithoutAStore)`: `level_key` of the 77000 summary is `"NM-300/A"`; with `level_uuid = "u1"` it is `"u1"`; `irradiation_key` of 66574 is empty.
  - `TEST(MemorySource, PositionsFilterByLevelKeyNotName)`: `positions = {.levels = {"NM-300/A"}}` → 77000 only.
  - `TEST(MemorySource, PositionsBranchesCombineByOrAndTheTreeByAnd)`: `{.irradiations = {"NM-301"}, .not_irradiated = true}` → 88000 and 66574. Adding `analysis_types = {"air"}` (66574 is air) → 66574 only.
  - `TEST(MemorySource, AnEmptyPositionSelectionMatchesNothing)`.
  - `TEST(MemorySource, PositionRowsIgnoreTheTreeAndPutNotIrradiatedLast)`: with `positions = {.levels = {"NM-300/A"}}`, `positions()` gives NM-300/A/77000, NM-301/A/88000, then 66574 with empty keys. One row per identifier even when it has two analyses.
  - In `test_record_source.cpp`, `TEST(RecordSource, PositionsUseNameKeys)`: on a record folder with two analyses of one identifier at a level, `positions()` gives one row whose `level_key` is `<irradiation>/<level>`.
  - In `test_store_source.cpp`: `TEST_F(StoreSourceTest, PositionsCarryUuidKeys)` (row keys are the fixture's uuid text; a level-key query filters as in Task 1). `TEST_F(StoreSourceTest, ANameKeyIsAnError)`: `browse` with `positions = {.levels = {"NM-300/A"}}` returns `ErrorKind::Protocol`.
- [ ] **Step 2: Run them to verify they fail**
  Run: `cmake --build --preset dev --target pychron_processing_tests pychron_processing_store_tests && build/dev/tests/processing/pychron_processing_tests --gtest_filter='MemorySource.*:RecordSource.*'`
  Expected: compile errors, then FAIL.
- [ ] **Step 3: Implement.** In `matches()`, a set `positions` is tested before the other lists. The keys come from `irradiation_key` and `level_key`; not irradiated means `irradiation_key(s)` is empty. `position_rows` keeps one row per identifier, using `matches` with the positions part left out, sorted as spec §4. `MemorySource` and `RecordDirectorySource` call it over their summaries. `StoreSource` converts each key with `Uuid::parse` (fail → `ErrorKind::Protocol`, naming the key), sets `BrowseFilter::positions`, maps `PositionListing` to `PositionRow` (uuid `str()` as key), and fills `irradiation_uuid` and `level_uuid` on summaries from `BrowseRow`.
- [ ] **Step 4: Run tests to verify they pass**, the new ones and every `MemorySource.*`, `RecordSource.*`, `StoreSourceTest.*`.
  Expected: PASS.
- [ ] **Step 5: Static analysis** clean.
- [ ] **Step 6: Commit**
  ```bash
  git commit -m "feat(processing): a browse query selects irradiation positions by key"
  ```

### Task 3: `IrradiationTree` widget

**Files:**
- Create: `apps/nobion/src/irradiation_tree.hpp`, `apps/nobion/src/irradiation_tree.cpp`
- Modify: `apps/nobion/CMakeLists.txt` (add the source to the UI library)
- Test: `tests/ui/test_data_windows.cpp`

**Interfaces:**
- Consumes: Task 2's `PositionRow`, `PositionSelection`; `style::make_tick_list` (`theme.hpp`).
- Produces, `pychron::ui::IrradiationTree : QWidget`:
  ```cpp
  explicit IrradiationTree(const QString& title, QWidget* parent = nullptr);
  void set_rows(const std::vector<processing::PositionRow>& rows);  // emits nothing
  std::optional<processing::PositionSelection> selection() const;
  void clear_checked();                 // changed() once when anything was ticked
  void set_expanded(bool expanded);
  bool is_expanded() const noexcept;
  QString title() const;
  QTreeWidget* tree() const noexcept;
  QLineEdit* filter() const noexcept;
  QToolButton* toggle_button() const noexcept;
  QToolButton* clear_button() const noexcept;
  // signal: void changed();
  ```
  Each item keeps its key in `Qt::UserRole` and its kind (irradiation, level, identifier, not irradiated) in `Qt::UserRole + 1`. Column 0 shows the name; column 1 shows `sample · project` on identifier rows.

- [ ] **Step 1: Write the failing tests**, in `test_data_windows.cpp`. Shared rows: NM-300/A with 77000 (FC-2, Fish Canyon) and 77001 (GA1550, no project); NM-301/A with 88000; 66574 not irradiated.
  - `irradiation_tree_starts_folded_and_lists_by_node`: `!is_expanded()`. There are three top-level items, and the last one's text is `Not irradiated`. 77000's column 1 is `FC-2 · Fish Canyon`; 77001's is `GA1550`.
  - `irradiation_tree_parent_tick_sets_children_and_shows_partial`: ticking NM-300 ticks A, 77000 and 77001. Unticking 77001 shows A and NM-300 as `Qt::PartiallyChecked`.
  - `irradiation_tree_selection_is_the_highest_full_node`: NM-300 ticked → `{.irradiations = {"NM-300"}}`. Only A ticked → `{.levels = {"NM-300/A"}}`. 77000 and 77001 both ticked → `{.levels = {"NM-300/A"}}`. 77000 alone → `{.identifiers = {"77000"}}`. "Not irradiated" ticked → `.not_irradiated == true`. Nothing ticked → `std::nullopt`.
  - `irradiation_tree_filter_shows_matches_and_ancestors`: filter `ga15` shows 77001 with NM-300 and A expanded, and hides 88000 and NM-301. A ticked 88000 is never hidden.
  - `irradiation_tree_keeps_a_tick_no_longer_listed`: tick NM-300/A, then `set_rows` without NM-300. NM-300/A is still there and ticked, under NM-300, at the end of the top-level items before "Not irradiated". `selection()` still gives `{.levels = {"NM-300/A"}}`. `set_rows` emitted nothing.
  - `irradiation_tree_header_counts_and_clear`: two ticked nodes → `title() == "Irradiation (2)"`. Clear → `changed` once, then `title() == "Irradiation"`.
- [ ] **Step 2: Run them to verify they fail**
  Run: `cmake --build --preset dev-ui --target pychron_ui_test_data_windows && QT_QPA_PLATFORM=offscreen build/dev-ui/tests/ui/pychron_ui_test_data_windows irradiation_tree_starts_folded_and_lists_by_node`
  Expected: compile error, then FAIL.
- [ ] **Step 3: Implement** `irradiation_tree.{hpp,cpp}`. The header, body and folding are as in `FacetBox` (`facet_box.cpp`). Use `Qt::ItemIsAutoTristate` on parent items for the three-state ticks, and guard against re-entry with an `updating_` flag as `FacetBox` does. A ticked node keeps its place: before `set_rows` rebuilds the tree, collect the ticked keys and their labels.
- [ ] **Step 4: Run tests to verify they pass**, the whole `test_data_windows` binary.
  Expected: PASS.
- [ ] **Step 5: Static analysis and `compiler_sweep.py --changed --build-dir build/dev-ui`** clean.
- [ ] **Step 6: Commit**
  ```bash
  git commit -m "feat(ui): an irradiation tree that ticks irradiations, levels and identifiers"
  ```

### Task 4: Wire the tree into the data browser

**Files:**
- Modify: `apps/nobion/src/data_browser_window.hpp`, `apps/nobion/src/data_browser_window.cpp`
- Test: `tests/ui/test_data_windows.cpp`

**Interfaces:**
- Consumes: Task 3's `IrradiationTree`; Task 2's `IAnalysisSource::positions`, `BrowseQuery::positions`.
- Produces: `IrradiationTree* DataBrowserWindow::irradiation_tree() const;`

- [ ] **Step 1: Write the failing tests.**
  - `browser_has_a_tree_in_place_of_three_lists`: `facet_box()` of `Irradiation`, `Level` and `Identifier` is null; `irradiation_tree()` is not.
  - `browser_query_takes_the_tree_ticks`: ticking a level in `w.irradiation_tree()` → `w.query().positions == PositionSelection{.levels = {key}}`, and the table shows only that level's analyses. Unticking it → `!w.query().positions`.
  - Update the existing tests that read `facet_box(pp::Facet::Irradiation)` or `facet_list(pp::Facet::Identifier/Level)`, such as `browser_list_filter_survives_a_reload`, to use the tree. Keep what each one checks: the filter text survives a reload, and narrowing asks the source nothing.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** Drop the three entries from `kFacets` and add the tree after the remaining lists, before Rescan. Connect `changed` to `reload()`. `query()` sets `positions` from `selection()`. `update_facets()` calls `source_.positions(query())` and passes the result to `set_rows`; on error it empties the tree and logs as the facets do. Delete the switch cases in `query()` that filled `irradiations`, `levels` and `identifiers` from those boxes.
- [ ] **Step 4: Run tests to verify they pass**, the whole `test_data_windows` binary.
- [ ] **Step 5: Static analysis and sweep** clean.
- [ ] **Step 6: Commit**
  ```bash
  git commit -m "feat(ui): the data browser filters by irradiation tree"
  ```

### Task 5: Measure cost and finish

**Files:**
- Modify (only if the measurement calls for it): `apps/nobion/src/irradiation_tree.cpp`
- Modify (only if code departed from spec): `docs/superpowers/specs/2026-11-04-irradiation-tree-design.md`

- [ ] **Step 1: Plans on a real-size store.** Work on a copy of a lab's imported store (ask the developer for one; do not use the original). With `.timer on`, run `EXPLAIN QUERY PLAN` and then the statement itself for §5.1 with each branch alone, and for §5.2 with no other filter. Record the times. Look for the bad signs in `libs/persistence/AGENTS.md` ("How to know").
- [ ] **Step 2: Tree build time.** Time `IrradiationTree::set_rows` with the §5.2 rows of that store (about 15 thousand). If it is over 100 ms, build identifier rows only when their level is first expanded, per spec §7. Add a test that a level's identifiers appear on expand and that the filter line still finds an unexpanded identifier. Measure again.
- [ ] **Step 3: Full verification.** Run `cmake --build --preset dev-ui`, then `ctest --preset dev-ui`: 100% passed, and no `LastTestsFailed.log`. Then `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --build-dir build/dev-ui`: 0 findings, 0 warnings.
- [ ] **Step 4: Check in the app.** Run `build/dev-ui/apps/nobion/Nobion.app` on a store with two irradiations. Tick one level and check that only its analyses are listed.
- [ ] **Step 5: Commit** any measurement-driven change, with the numbers in its message (persistence AGENTS.md):
  ```bash
  git commit -m "perf(ui): ..."   # only if Step 2 changed code
  ```

## Open questions

None.
