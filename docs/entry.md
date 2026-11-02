# Sample and package entry

How to enter samples and packages (irradiations) in the DVC store, from
`pychron-ui` or `elctl entry`. The design is
`superpowers/specs/2026-10-04-sample-irradiation-entry-design.md`.

## Before you start

- Entry writes the store only: the shared PostgreSQL database of the lab (or
  a SQLite file for trying things out). It never writes the legacy MySQL
  database or the meta repository.
- A lab that still has legacy pychron data migrates it first
  (`elctl import`, see `legacy_import.md`). After that the store is the
  catalog, and legacy Python acquisition is not supported.
- Entry needs the server. Nothing here works from an offline export.

## Words

| Word | Meaning |
|---|---|
| package | A set of levels of positions that hold samples. The generic idea of an irradiation. |
| package kind | `irradiation` (a chronology, interference productions and flux) or `package` (positions and samples only). Each package has its own; it can be changed later without losing anything. |
| level | A tray of a package (A, B, ...), on an irradiation holder. |
| position | A hole of a level's holder, numbered from 1. |
| identifier | The labnumber of a position, given out in sequence. |

## Reference samples

An instrument install's database starts with a project `references` and one
sample for each kind of reference run: `blank_unknown`, `blank_air`,
`blank_cocktail`, `blank_extractionline`, `background`, `air`, `cocktail`
and `detector_ic`, each with its special identifier (`bu`, `ba`, `bc`, `be`,
`bg`, `a`, `c`, `ic`). It also starts with the reactor `Triga` and its
production ratios, and with the install's mass spectrometer (`argus`, `helix`
or `ngx`). Setup puts them there from the install's `seed.toml`
(`installation_runbook.md`, part 2.3), and `elctl entry seed` does the same
by hand.

- They are ordinary rows: edit them here like any other sample.
- The seed never changes a row that exists. A special identifier that was
  already in the store (from a legacy import) keeps the sample it has, or
  none.
- `seed.toml` has two lists: `mass_spectrometers` (shipped with the
  install's own) and `extract_devices` (shipped empty). A queue's runs are saved to the database only under a
  mass spectrometer and an extract device the database has, and a queue that
  names one it lacks is not started. To add one, put its name in the list
  and run `elctl entry seed <root>/seed.toml --db <url>`; it prints, for
  example, `seeded 1 extract device`. A name may not be empty or repeated.

## Samples

Entry > Samples… lists the samples (filter by PI, project, material, or
search by name).

- Double-click a cell to edit it. Edited cells are tinted until you Save.
  Save writes every edit at once.
- If another person changed one of those samples since you loaded it, nothing
  is saved. The row turns yellow, and its tooltip shows their values.
  Reload, then edit again.
- New sample: fill in the form above the table, then Add, then Save.
  - The PI is chosen from the list, or made with New PI… (see
    [Principal investigators](#principal-investigators)).
  - A project or material that does not exist yet is created with the
    sample.
  - A warning appears when a similarly named sample exists in any project
    ("FC-2", "fc 2" and "FC_2" all count as the same name).
- Delete is refused for samples that sit in a position or have analyses.

Entry > Import Samples… (or Import… in the Samples window) reads a CSV file,
or rows pasted from a spreadsheet.

1. Write Template… saves a CSV that has every column.
2. Columns are matched by name, and common aliases work (`pi`, `orcid`,
   `latitude`, `grain size`, ...). Change the match in the mapping table if
   needed.
3. The preview gives each row a state:

   | State | Meaning |
   |---|---|
   | create | The sample is new. |
   | exists | The sample is already stored with the same values. |
   | update | The sample is stored with different values. It is written only when "Update samples that differ" is ticked. |
   | error | The row has a problem; every problem is listed. |

4. While any row is in error, nothing can be imported. Export Errors… saves
   those rows to fix them.
5. A row whose PI has namesakes waits for an answer. Import asks once for each
   name, and the answer is used for every row of that name.

UTM coordinates (easting, northing, zone such as `13S`) are converted to
latitude and longitude. A zone letter before N means the southern hemisphere.

## Principal investigators

A PI is a person or an organisation. A lab or a group is an organisation, not
a person's name; there is no list of allowed names.

- A person has a family name and a given name in full, as written ("María
  José", "Ruiz García"), or what is known of the given name (an initial).
  An organisation has its name and no given name.
- ORCID (a person's, `0000-0002-1825-0097`) and ROR (the affiliation's, or the
  organisation's own, `03yrm5c26`) are optional. A URL is cut down to the
  identifier, and a value that is not well formed is refused, with the value
  quoted. They are not looked up on the network. Two PIs cannot have one
  ORCID.
- Two PIs may have the same name. A PI is its uuid, not what it is called.
- A person shows as "Jake Ross", or "J Ross" with an initial, or "Ross" with
  no given name; an organisation as its name. Lists are sorted by family name,
  then given name. Where two PIs show the same text, a list adds the
  affiliation, or else the last four characters of the ORCID, in brackets:
  "J Ross (NMT)". A store migrated from before this shows "J Ross" where it
  showed "Ross, J".

In the Samples window, New PI… asks for each part in its own field, and
nothing is parsed. Edit PI… changes a stored one, its kind included.

The columns of a sheet:

| Column | Meaning |
|---|---|
| `principal_investigator` | A person as "Family, Given", the given name kept in full. Without a comma it is a family name alone, so "Jake Ross" is a family name of two words. An organisation's name when `principal_investigator_kind` says so. |
| `principal_investigator_kind` | Optional. `person` (the default) or `organisation`. An organisation with an ORCID or a given name is refused. |
| `principal_investigator_orcid` | Optional. Matched first: the row is that PI, whatever the name cell says, and a family name that differs from that PI's is an error naming both. |
| `principal_investigator_uuid` | Optional. The PI itself, with no matching at all; a uuid that matches nothing is an error. |

**Namesakes.** A person named without an ORCID or a uuid is not guessed.

- One stored PI of exactly that family name and given name is that PI, so a
  sheet imported twice adds nothing.
- Otherwise, when stored PIs share the family name and the given initial (or
  the cell gives no given name, and any person of that family name could be
  meant), the row stops and asks. Which PI? lists each with its affiliation,
  ORCID and projects: This One, or Another Person.
- New PI… asks the same question when you add someone whose name is close to
  a stored PI's, unless an ORCID you gave is already stored: then it is that PI.
- `elctl entry samples import` cannot ask: it fails, lists the candidates (name,
  affiliation, ORCID, uuid), and writes nothing. Add a
  `principal_investigator_orcid` or `principal_investigator_uuid` column.

A legacy import has nobody to ask. "Ross, J" becomes the stored Ross whose
given name starts with J; with none, a new person; with several, the oldest,
and the import report names the others.

## Packages

Entry > Packages… shows packages and their levels on the left, and the
positions of the chosen level in the middle. The dock on the right has four
tabs: Samples, Level, Chronology and Holder.

**New Package…**

- Choose the kind. The name is the next one with the lab's prefix
  (NM-301 after NM-300).
- Give the levels as `A-C` or `A,B,D`, with a holder and z for them.
- An irradiation also needs a reactor (its production is copied from the
  reactor defaults) and its doses (power, start and end, in local time).
- Everything is written in one step.

**New Level…** pre-fills the next letter, and the holder, z and production of
the level you are looking at.

**Positions**

- Select rows, or click and drag on the Holder drawing.
- Pick a sample in the Samples tab, then press Assign to selected.
- Weight, packet and note are edited in the grid.
- Clear Fields… blanks the chosen fields of the selected positions.
- Fill Packets… numbers packets from the one you give: P7, P8, ...
- Save writes the level: positions, note, holder, z and production, in one
  step.
- Moving to another level with unsaved edits asks whether to save them.
- Clearing a sample never removes a position's identifier.
- Changing the sample of a position that has analyses asks first, and says
  how many analyses it changes.

**Identifiers**

- Generate Identifiers… numbers every position that has a sample and no
  identifier. Numbering continues the store's sequence (the highest numeric
  identifier, then +1, +2, ...), level by level in name order, position by
  position.
- The preview is exactly what is written. If someone else gave out
  identifiers in the meantime, the dialog shows the new plan instead of
  writing.
- "Renumber identifiers nothing has used yet" gives new numbers to positions
  whose identifiers have no analyses. Numbers are never reused.
- For an irradiation, the dialog warns about a level with no monitor, and
  about a monitor outside project `Irradiation-<package>`.

**Other tools**

- Edit Production… on the Level tab edits a production's interference ratios,
  creates a new production, or copies a reactor default.
- The Chronology tab edits the doses of an irradiation.
- Import Positions… / Export CSV… use the columns `level, position,
  identifier, sample, project, principal_investigator, material, grainsize,
  weight, packet, note, j, j_err`. Identifier and J are not read back.
- Save PDF… writes the level sheets: a summary, then each level with its
  holder and a row for every hole.

Entry > Holders… lists irradiation holders and imports legacy holder files
(`circle,<radius>[,True]`, then one hole per line).

Entry > Entry Settings… holds the lab's settings, which every client shares:

- the package name prefix and the kind new packages start as
- the monitor sample and material
- the estimated J per hour of dose
- whether positions without an identifier need a packet

## elctl entry

The same work from a shell. Writing commands take `--db <url>`, `--user`
and `--dry-run`. `elctl entry help` lists every option.

```
elctl entry samples template samples.csv
elctl entry samples import samples.csv --db postgresql://... [--update-existing] [--errors bad.csv]
elctl entry holders import 24-hole.txt --db ...
elctl entry package add NM-301 --levels A-C --holder 24-hole --z 0.5 \
      --reactor Triga --chronology chron.txt --tz America/Denver --db ...
elctl entry package add P-7 --kind package --levels A --db ...
elctl entry positions import NM-301 positions.csv --db ...
elctl entry identifiers generate NM-301 --dry-run --db ...
elctl entry identifiers generate NM-301 --db ...
elctl entry package show NM-301 --csv --db ...
elctl entry settings set package_prefix NM- --db ...
elctl entry seed ~/Pychron/argus/seed.toml --dry-run --db ...
elctl entry seed ~/Pychron/argus/seed.toml --db ...
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | It worked. |
| 1 | Nothing was written: a row was stale, refused or invalid. The output says which. |
| 2 | A usage error or a fatal error. |
