# Installation instructions (ASD-STE100)

These instructions tell you how to install Nobion on a lab computer and how
to make the first installation. They are written in Simplified Technical
English (ASD-STE100). The [installation runbook](installation_runbook.md)
contains the full procedures and the reasons for them.

> **Status.** All of Nobion operates with the built-in simulator. No driver
> has had a test on a real instrument. Thus, an instrument installation starts
> in simulation and stays in simulation.

## 1. General

### 1.1 Programs

The package contains two programs:

- `nobion`: the application. On macOS, the name of the application is
  Pychron.
- `elctl`: the command-line program.

The package also contains the setup profiles and the example configuration
files.

### 1.2 Installation

An installation is one folder of TOML files. Setup writes these files from a
setup profile. There are four setup profiles:

- `data-reduction`: for a computer that only shows, reduces and plots data.
- `argus`, `helix`, `ngx`: for a computer that controls an instrument.

The site configuration file lists the installations on the computer. It also
identifies the default installation. The first installation that you make
becomes the default installation.

| Platform | Site configuration file |
|---|---|
| macOS | `~/Library/Application Support/Pychron/site.toml` |
| Linux | `$XDG_CONFIG_HOME/pychron/site.toml`, else `~/.config/pychron/site.toml` |
| Windows | `%APPDATA%\Pychron\site.toml` |

The environment variable `NOBION_SITE_CONFIG` changes the location of the
site configuration file.

### 1.3 Which procedures to do

| Computer | Procedures |
|---|---|
| A computer that only shows, reduces and plots data | 3, 4, 5, 6 |
| A computer that controls an instrument (Argus, Helix, NGX) | 3, 4, 5, 7 |
| A computer that you cannot install a package on | 2 (prerequisites), then 4 (from the source code) |

## 2. Prerequisites

Before you start, make sure that you have these items:

- A user account that can install software on the computer.
- For an instrument installation:
  - The IP address and port of the control computer of the instrument.
    The default port of the Qtegra RemoteControlServer is 1069. The default
    port of the NGX controller is 1099.
  - The calibration data of the instrument: the field table, the detectors
    and the source settings.
- For a PostgreSQL database:
  - A PostgreSQL server with PostGIS.
  - An empty database.
  - A database user that can make tables and the `postgis` extension. If the
    user cannot make the extension, an administrator must type
    `CREATE EXTENSION postgis` one time.

## 3. Install the programs from a release package

### 3.1 Get the package

1. Go to <https://github.com/PychronLabsLLC/nobion/releases>.
2. Download the package for your platform:

   | Platform | Package |
   |---|---|
   | macOS (Apple silicon) | `Nobion-<version>-macOS-arm64.dmg` |
   | Ubuntu 24.04 | `nobion_<version>_amd64.deb` |
   | Linux, other | `.tar.gz` |
   | Windows x64 | `Nobion-<version>-windows-x64.exe` or `.zip` |

Each package contains its own Python for the extraction scripts.

### 3.2 Install on macOS

1. Open the `.dmg` file.
2. Move Nobion to the Applications folder.
3. Open Nobion one time.
4. If macOS shows "Apple could not verify ...", do these steps:
   1. Close the message.
   2. Open System Settings > Privacy & Security.
   3. Click Open Anyway.

   On macOS 15 and later, right-click > Open does not open the application.
5. If macOS shows "Nobion is damaged and can't be opened", type this command
   in Terminal. Then open Nobion again.

   ```bash
   xattr -dr com.apple.quarantine /Applications/Nobion.app
   ```

6. To use `elctl` in Terminal, type this command:

   ```bash
   alias elctl=/Applications/Nobion.app/Contents/MacOS/elctl
   ```

The message in step 5 occurs only with release v0.3.0.

If an older release wrote a file and macOS does not open it, type this
command:

```bash
xattr -d com.apple.quarantine <file>
```

### 3.3 Install on Ubuntu 24.04

1. Type this command in the folder that contains the package:

   ```bash
   sudo apt install ./nobion_*.deb
   ```

2. If you use a PostgreSQL database, install the PostgreSQL driver for Qt:

   ```bash
   sudo apt install libqt6sql6-psql
   ```

The package uses the Qt of Ubuntu. The command `elctl` is on the `PATH`.

### 3.4 Install on other Linux systems

1. Unpack the `.tar.gz` file into a folder.

You can put the folder at any location. The command `elctl` is in
`<folder>/bin/elctl`.

### 3.5 Install on Windows

1. Run the installer (`.exe`).
2. If Windows shows a warning, accept it and continue.

The Windows packages do not have a code signature. Thus, Windows shows a
warning when you start the installer for the first time. The installer adds
`elctl` to the `PATH`.

## 4. Install the programs from the source code

Use this procedure only if there is no package for your platform. Also use it
for a change that is not in a release. The file
[dev_setup.md](dev_setup.md), sections 1 and 7, tells you how to install the
tools.

1. In the source folder, type these commands:

   ```bash
   cmake -S . -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_UI=ON -DBUILD_TESTS=OFF
   ```

   ```bash
   cmake --build build/pkg
   ```

   ```bash
   (cd build/pkg && cpack)
   ```

2. Install the package that `cpack` wrote. Use the procedure for your
   platform (3.2 to 3.5).
3. Do the check in procedure 5.

On macOS, `macdeployqt` can show `ERROR: Cannot resolve rpath ...` lines and
a codesign error. These lines do not cause a problem. Use the check in procedure 5 to
identify a good build.

A package from this procedure uses the Python on the build computer. It
operates only on a computer that has the same Python at the same location.
The file [dev_setup.md](dev_setup.md), section 7, tells you how to put Python
into the package.

If Qt 6 (Core and Sql) is not on the build computer, the build does not
include the database library. Then `elctl import` is not available.

## 5. Do a check of the programs

1. Type this command:

   ```bash
   elctl --version
   ```

   The output shows the version, a `profiles:` line and an `examples:` line.
2. If the `profiles:` line or the `examples:` line is missing, the package is
   not complete. Install the package again.
3. Type this command:

   ```bash
   nobion --self-test
   ```

   On macOS, the program is `/Applications/Nobion.app/Contents/MacOS/Nobion`.
4. Make sure that each line of the output shows `OK`.

On Linux without a display, put `QT_QPA_PLATFORM=offscreen` before the
command. Do not use this variable on macOS or Windows.

## 6. Make a data-reduction installation

You can make an installation in two ways. The two ways give the same result:

- The setup wizard. Nobion opens the wizard when there is no installation.
  To open the wizard again, type `nobion --setup`.
- The command line. Type `elctl init <profile>`. The command asks the same
  questions as the wizard.

### 6.1 With a database on this computer

1. Type this command:

   ```bash
   elctl init data-reduction --root ~/Documents/Pychron --yes
   ```

   Setup makes an empty SQLite database and prepares it.
2. Do the check in 6.3.

### 6.2 With the PostgreSQL server of the lab

> **CAUTION:** MAKE SURE THAT THE SERVER DATABASE HAS A SCHEMA. SETUP DOES
> NOT MAKE OR CHANGE THE SCHEMA OF A SERVER DATABASE.

1. Type this command. Use the values of your server:

   ```bash
   elctl init data-reduction --root ~/Documents/Pychron \
     --set data_source=server --set db_host=labdb.example.org --set db_port=5432 \
     --set db_name=pychron --set db_user=pychron
   ```

2. When setup asks for the password, type the password.

   Setup keeps the password in `<root>/.pychron/credentials.toml`. Only the
   owner of the file can read it.
3. Do the check in 6.3.

The importer makes the schema of a server database (runbook, part 3.3). After
an update of Nobion, the schema can be old. To find the status of the
schema, type `elctl db status --db <url>`. To update the schema, type
`elctl db migrate --db <url>`.

### 6.3 Do a check of the installation

1. Type this command:

   ```bash
   elctl doctor
   ```

2. Make sure that the output shows `[OK] database: ... (schema version N)`.
3. Open Pychron. The data browser opens.

## 7. Make an instrument installation

> **WARNING:** DO NOT CONNECT PYCHRON TO AN INSTRUMENT WITH THIS PROCEDURE.
> NO DRIVER HAS HAD A TEST ON A REAL INSTRUMENT.

> **CAUTION:** DO NOT STOP SIMULATION BEFORE YOU DO ALL OF PART 2.4 OF THE
> RUNBOOK. AN INCORRECT FIELD TABLE CAN LEAVE THE CDD WITHOUT PROTECTION.

This procedure uses the `argus` profile. For a Helix or an NGX, use `helix` or
`ngx`.

### 7.1 Make the installation

1. Type this command:

   ```bash
   elctl init argus --root ~/Pychron/argus
   ```

2. Push Enter to accept each default value.

The installation starts in simulation. Setup writes these items:

- The extraction line and the canvas.
- `spectrometer.toml` and the field tables.
- Three measurement plans: `multicollect`, `detector_ic` and
  `multicollect_hop_ar39`.
- The scripts, the conditionals and `identifiers.toml`.
- The run defaults, an example queue and `CALIBRATE.md`.
- A SQLite database at `data/pychron.db`.

Setup puts the reference samples into the database from `<root>/seed.toml`.
The [runbook](installation_runbook.md), part 2.3, lists these samples.

### 7.2 Select the extraction line

The `line_source` question selects the extraction line:

| `line_source` | Result |
|---|---|
| `starter` (default) | The example line with five valves, on simulated controllers |
| `import` | Your `extraction_line.toml` and `canvas.toml` files (`line_file`, `canvas_file`). Setup does a check of the files before it writes. |
| `legacy` | Setup converts a legacy Pychron `setupfiles` folder (`legacy_folder`). Refer to the runbook, part 3.6. |

### 7.3 Do a check of the installation

1. Type this command:

   ```bash
   elctl doctor --install argus
   ```

2. Make sure that each line of the output shows `[OK]`.
3. Type this command. It runs the example queue in simulation:

   ```bash
   elctl --install argus exp run experiment.toml --sim-speed 50
   ```

4. Make sure that the output ends with these lines:

   ```
   queue completed
   5/5 run(s) succeeded; in sqlite:<root>/data/pychron.db
   ```

5. Type `nobion --install argus`.
6. Select View > Data. Make sure that the five runs show.

> **CAUTION:** DO NOT RUN TWO PROGRAMS WITH THE DATABASE OF ONE INSTALLATION
> AT THE SAME TIME.

### 7.4 After simulation

Part 2.4 of the [runbook](installation_runbook.md) tells you how to go from
simulation to the instrument. Do all the steps of part 2.4 in sequence.

## 8. Change the configuration

| Task | Procedure |
|---|---|
| Change an answer | Type `elctl init --reconfigure --install NAME --set id=value`, or type `nobion --setup` |
| Change the data source | Select Preferences > Data source, or type `elctl data-source set` |
| Change a configuration file | Edit the TOML file in the installation folder. Then type `elctl doctor`. |
| Select, set as default or remove an installation | In Nobion, select File > Installations. Nobion removes the entry only. It does not delete the files. |
| Use one installation | Type `elctl --install NAME <command>` or `nobion --install NAME` |

## 9. Remove Nobion

### 9.1 Remove the programs

| Platform | Procedure |
|---|---|
| macOS | Move Nobion to the Trash. |
| Ubuntu | Type `sudo apt remove nobion`. |
| Windows | Open Apps & features and remove Pychron. |

The installation folders and the site configuration file stay on the
computer.

### 9.2 Remove an installation

1. In Nobion, select File > Installations and remove the installation.

   As an alternative, delete its `[[installs]]` entry in the site
   configuration file.
2. Delete the installation folder.

## 10. Troubleshooting

| Message or problem | Cause | Correction |
|---|---|---|
| `nothing is installed yet (elctl init --list)` | The site configuration file has no installation. | Make an installation (procedure 6 or 7). Or examine `NOBION_SITE_CONFIG`. |
| `several installs and no default` | There is no default installation. | Add `--install NAME` to the command. Or set the default in File > Installations. |
| `doctor`: `database ... not checked: built without the database library` | The build does not contain Qt Sql. | Use a release package. Or build again with Qt 6. |
| `doctor` shows an error for a PostgreSQL database | The server, the user or the password is incorrect. Or the `libqt6sql6-psql` driver is missing. Or the database has no schema. | Correct the value. Install the driver. For the schema, refer to 6.2. |
| `seed skipped: the database could not be opened` | Setup cannot open the database, or the schema is old. | Correct the database (6.2). Then type the command that setup shows. |
| `doctor` warns `every transport is simulated` | Simulation is off, but the extraction line is the example line. | Put the real controllers in `extraction_line.toml`. |

For more troubleshooting, refer to the [runbook](installation_runbook.md)
and to [10 Troubleshooting](user/10-troubleshooting.md).

## 11. Help

Open an issue at <https://github.com/PychronLabsLLC/nobion/issues>.
Attach the output of these commands:

- `elctl --version`
- `elctl doctor`

---

Written by Claude Opus 5.5 (`claude-opus-5-5`).
