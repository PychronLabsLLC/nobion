// The step heat template editor: the table model over one template's steps,
// and the window against a scratch copy of the example lab.

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "nobion/experiment/factory/form.hpp"
#include "settings_guard.hpp"
#include "step_heat_editor_window.hpp"
#include "step_heat_table_model.hpp"

using pychron::ui::StepHeatEditorWindow;
using pychron::ui::StepHeatTableModel;
namespace ex = pychron::experiment;
namespace fs = std::filesystem;
namespace lab = pychron::experiment::lab;

namespace {

ex::StepHeatTemplate with_values(const std::vector<double>& values) {
  ex::StepHeatTemplate t;
  t.name = "t";
  for (const double v : values) {
    ex::StepHeatStep s;
    s.value = v;
    t.steps.push_back(s);
  }
  return t;
}

std::vector<double> values_of(const StepHeatTableModel& m) {
  std::vector<double> out;
  for (const auto& s : m.tmpl().steps) out.push_back(s.value);
  return out;
}

QString text(const StepHeatTableModel& m, int row, int column, int role = Qt::DisplayRole) {
  return m.data(m.index(row, column), role).toString();
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void spit(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

// Every regular file under `dir` with its bytes: what a test compares to say
// nothing else was written.
std::map<std::string, std::string> snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file()) out[fs::relative(e.path(), dir).generic_string()] = slurp(e.path());
  return out;
}

}  // namespace

class TestStepHeatEditor : public QObject {
  Q_OBJECT

  // The window tests: a fresh scratch lab per test; the lab outlives the window.
  QTemporaryDir tmp_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  fs::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  int settings_n_ = 0;

  fs::path file(const std::string& name) const { return dir_ / "queues" / "step_heats" / (name + ".toml"); }
  void make_lab() {
    dir_ = pychron::ui::test::scratch_lab();
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
  }
  std::unique_ptr<StepHeatEditorWindow> window() {
    if (!lab_) make_lab();
    ++settings_n_;
    auto w = std::make_unique<StepHeatEditorWindow>(
        *lab_, std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("settings%1.ini").arg(settings_n_)),
                                           QSettings::IniFormat));
    w->set_ask_unsaved([](const QString&) { return StepHeatEditorWindow::Unsaved::Discard; });
    w->set_confirm([](const QString&) { return true; });
    w->set_ask_name([]() -> std::optional<QString> { return std::nullopt; });
    return w;
  }
  static void set_value(StepHeatEditorWindow& w, int row, const QString& text) {
    QVERIFY(w.model().setData(w.model().index(row, StepHeatTableModel::Value), text));
  }

 private slots:
  void cleanup() {
    lab_.reset();
    if (!dir_.empty()) {
      std::error_code ec;
      fs::remove_all(dir_, ec);
      dir_.clear();
    }
  }
  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }

  void stepColumnIsLettersAndReadOnly() {
    StepHeatTableModel m;
    m.set_template(with_values({1, 2, 3}));
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(m.columnCount(), int(StepHeatTableModel::ColumnCount));
    QCOMPARE(text(m, 0, StepHeatTableModel::Step), QStringLiteral("A"));
    QCOMPARE(text(m, 2, StepHeatTableModel::Step), QStringLiteral("C"));
    QVERIFY(!(m.flags(m.index(0, StepHeatTableModel::Step)) & Qt::ItemIsEditable));
    QVERIFY(m.flags(m.index(0, StepHeatTableModel::Value)) & Qt::ItemIsEditable);
    QVERIFY(!m.setData(m.index(0, StepHeatTableModel::Step), QStringLiteral("Z")));
    QCOMPARE(m.headerData(StepHeatTableModel::Duration, Qt::Horizontal).toString(), QStringLiteral("Duration (s)"));
    QCOMPARE(m.headerData(StepHeatTableModel::BeamDiameter, Qt::Horizontal).toString(),
             QStringLiteral("Beam diameter"));
  }

  void emptyCellShowsTheInheritedValueGreyed() {
    StepHeatTableModel m;
    ex::ExtractionSpec form;
    form.duration = ex::Duration{10};
    form.cleanup = ex::Duration{7};
    m.set_form_extraction(form);
    auto t = with_values({1, 2});
    t.duration = ex::Duration{30};
    t.steps[1].duration = ex::Duration{45};
    m.set_template(t);

    const QModelIndex inherited = m.index(0, StepHeatTableModel::Duration);
    QCOMPARE(m.data(inherited).toString(), QStringLiteral("30"));
    QCOMPARE(m.data(inherited, Qt::ForegroundRole).value<QColor>(),
             QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text));
    QVERIFY(m.data(inherited, Qt::EditRole).toString().isEmpty());
    // From the form when the template has no default either.
    QCOMPARE(text(m, 0, StepHeatTableModel::Cleanup), QStringLiteral("7"));

    const QModelIndex own = m.index(1, StepHeatTableModel::Duration);
    QCOMPARE(m.data(own).toString(), QStringLiteral("45"));
    QVERIFY(!m.data(own, Qt::ForegroundRole).isValid());
    QCOMPARE(m.data(own, Qt::EditRole).toString(), QStringLiteral("45"));
  }

  void clearingACellInherits() {
    StepHeatTableModel m;
    auto t = with_values({1});
    t.steps[0].duration = ex::Duration{45};
    t.steps[0].units = ex::Unit::Percent;
    m.set_template(t);
    QSignalSpy edited(&m, &StepHeatTableModel::edited);

    QVERIFY(m.setData(m.index(0, StepHeatTableModel::Duration), QString()));
    QVERIFY(!m.tmpl().steps[0].duration.has_value());
    QVERIFY(m.setData(m.index(0, StepHeatTableModel::Units), QString()));
    QVERIFY(!m.tmpl().steps[0].units.has_value());
    QCOMPARE(edited.count(), 2);

    QVERIFY(m.setData(m.index(0, StepHeatTableModel::Cleanup), QStringLiteral("12.5")));
    QCOMPARE(m.tmpl().steps[0].cleanup, std::optional<ex::Duration>(ex::Duration{12.5}));
    QVERIFY(m.setData(m.index(0, StepHeatTableModel::Units), QStringLiteral("%")));
    QCOMPARE(m.tmpl().steps[0].units, std::optional<ex::Unit>(ex::Unit::Percent));
    QVERIFY(m.setData(m.index(0, StepHeatTableModel::BeamDiameter), QStringLiteral("2")));
    QCOMPARE(m.tmpl().steps[0].beam_diameter, std::optional<double>(2));

    // Text that does not read leaves the cell, and a step always has a value.
    QVERIFY(!m.setData(m.index(0, StepHeatTableModel::Duration), QStringLiteral("soon")));
    QVERIFY(!m.setData(m.index(0, StepHeatTableModel::Units), QStringLiteral("furlongs")));
    QVERIFY(!m.setData(m.index(0, StepHeatTableModel::Value), QString()));
    QCOMPARE(m.tmpl().steps[0].value, 1.0);
  }

  void addCopiesSelectedOrLastWithIncrement() {
    StepHeatTableModel m;
    QCOMPARE(m.add_step(std::nullopt), 0);  // nothing yet: one step of value 1
    QCOMPARE(values_of(m), (std::vector<double>{1}));

    m.set_template(with_values({1, 2}));
    QSignalSpy edited(&m, &StepHeatTableModel::edited);
    QCOMPARE(m.add_step(std::nullopt), 2);
    QCOMPARE(values_of(m), (std::vector<double>{1, 2, 3}));
    QCOMPARE(m.add_step(0), 1);  // a copy of row 0: no step before it to step by
    QCOMPARE(values_of(m), (std::vector<double>{1, 1, 2, 3}));
    QCOMPARE(text(m, 3, StepHeatTableModel::Step), QStringLiteral("D"));
    QCOMPARE(edited.count(), 2);
  }

  void insertMoveAndDelete() {
    StepHeatTableModel m;
    m.set_template(with_values({1, 2, 3}));
    QCOMPARE(m.insert_step(1), 1);
    QCOMPARE(values_of(m), (std::vector<double>{1, 2, 2, 3}));
    QVERIFY(m.setData(m.index(1, StepHeatTableModel::Value), QStringLiteral("1.5")));
    QCOMPARE(values_of(m), (std::vector<double>{1, 1.5, 2, 3}));

    QVERIFY(m.move_up(2));
    QCOMPARE(values_of(m), (std::vector<double>{1, 2, 1.5, 3}));
    QVERIFY(m.move_down(0));
    QCOMPARE(values_of(m), (std::vector<double>{2, 1, 1.5, 3}));
    QVERIFY(!m.move_up(0));
    QVERIFY(!m.move_down(3));

    m.remove_rows({2, 0});
    QCOMPARE(values_of(m), (std::vector<double>{1, 3}));
    QCOMPARE(text(m, 0, StepHeatTableModel::Step), QStringLiteral("A"));
    QCOMPARE(text(m, 1, StepHeatTableModel::Step), QStringLiteral("B"));
    QCOMPARE(m.insert_step(5), -1);
  }

  void fillSetsOneColumnOnSelectedRows() {
    StepHeatTableModel m;
    m.set_template(with_values({1, 2, 3}));
    m.fill(StepHeatTableModel::Cleanup, 20, {0, 2});
    QCOMPARE(m.tmpl().steps[0].cleanup, std::optional<ex::Duration>(ex::Duration{20}));
    QVERIFY(!m.tmpl().steps[1].cleanup.has_value());
    QCOMPARE(m.tmpl().steps[2].cleanup, std::optional<ex::Duration>(ex::Duration{20}));

    m.fill(StepHeatTableModel::RampRate, 4, {});
    for (const auto& s : m.tmpl().steps) QCOMPARE(s.ramp_rate, std::optional<double>(4));
    m.fill(StepHeatTableModel::RampRate, QString(), {1});  // empty clears
    QVERIFY(!m.tmpl().steps[1].ramp_rate.has_value());
  }

  void fillValuesFromText() {
    StepHeatTableModel m;
    m.set_template(with_values({}));
    QVERIFY(m.fill_values({}, QStringLiteral("5:2.5:4")).has_value());
    QCOMPARE(values_of(m), (std::vector<double>{5, 7.5, 10, 12.5}));

    m.set_template(with_values({1, 2, 3}));
    const auto bad = m.fill_values({0, 1, 2}, QStringLiteral("1,2"));
    QVERIFY(!bad.has_value());
    QCOMPARE(QString::fromStdString(bad.error().what), QStringLiteral("2 values for 3 selected steps"));
    QCOMPARE(values_of(m), (std::vector<double>{1, 2, 3}));
    QVERIFY(!m.fill_values({}, QStringLiteral("five")).has_value());

    QVERIFY(m.fill_values({2, 0}, QStringLiteral("9 8")).has_value());
    QCOMPARE(values_of(m), (std::vector<double>{8, 2, 9}));
  }

  void templateDefaultsAreEdited() {
    StepHeatTableModel m;
    m.set_template(with_values({1}));
    QSignalSpy edited(&m, &StepHeatTableModel::edited);
    m.set_template_defaults(ex::Unit::Percent, ex::Duration{30}, std::nullopt, "sanidine");
    QCOMPARE(m.tmpl().units, std::optional<ex::Unit>(ex::Unit::Percent));
    QCOMPARE(m.tmpl().duration, std::optional<ex::Duration>(ex::Duration{30}));
    QVERIFY(!m.tmpl().cleanup.has_value());
    QCOMPARE(m.tmpl().description, std::string("sanidine"));
    QCOMPARE(text(m, 0, StepHeatTableModel::Duration), QStringLiteral("30"));
    QCOMPARE(edited.count(), 1);
  }

  // ---- StepHeatEditorWindow ----

  void listsTheLabTemplatesSorted() {
    make_lab();
    spit(file("b-two"), "[[steps]]\nvalue = 1\n");
    spit(file("a-one"), "[[steps]]\nvalue = 2\n");
    auto w = window();
    QCOMPARE(w->file_names(), (QStringList{"a-one", "b-two", "laser-12"}));
    QCOMPARE(w->current_name(), QStringLiteral("a-one"));
    QVERIFY(!w->modified());
  }

  void newSaveWritesOnlyUnderStepHeats() {
    auto w = window();
    const auto before = snapshot(dir_);
    QSignalSpy files(w.get(), &StepHeatEditorWindow::filesChanged);
    QSignalSpy saved(w.get(), &StepHeatEditorWindow::saved);
    QVERIFY(w->new_file(QStringLiteral("x")));
    QCOMPARE(w->current_name(), QStringLiteral("x"));
    QVERIFY(w->modified());
    QVERIFY(w->file_names().contains(QStringLiteral("x")));
    QVERIFY(!fs::exists(file("x")));
    QCOMPARE(w->model().add_step(std::nullopt), 0);
    QCOMPARE(w->model().add_step(std::nullopt), 1);
    QString error;
    QVERIFY2(w->save(&error), qPrintable(error));
    QVERIFY(!w->modified());
    QCOMPARE(saved.count(), 1);
    QCOMPARE(files.count(), 1);

    auto after = snapshot(dir_);
    QVERIFY(after.erase("queues/step_heats/x.toml") == 1);
    QVERIFY(after == before);
    auto back = ex::load_step_heat(file("x"));
    QVERIFY(back.has_value());
    QCOMPARE(back->steps.size(), std::size_t{2});
    QCOMPARE(back->steps[1].value, 1.0);  // a copy: no step before the first to step by

    QString bad;
    QVERIFY(!w->new_file(QStringLiteral("x"), &bad));
    QVERIFY(bad.contains(QStringLiteral("exists")));
    QVERIFY(!w->new_file(QStringLiteral("Bad Name"), &bad));
  }

  void aBadValueBlocksSave() {
    auto w = window();
    QVERIFY(w->open(QStringLiteral("laser-12")));
    const std::string bytes = slurp(file("laser-12"));
    QVERIFY(w->diagnostic_lines().isEmpty());
    set_value(*w, 0, QStringLiteral("0"));
    QVERIFY(w->diagnostic_lines().contains(QStringLiteral("step 1 (A): value must be greater than 0")));
    QString error;
    QVERIFY(!w->save(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!w->save_as(QStringLiteral("other"), &error));
    QVERIFY(!fs::exists(file("other")));
    QCOMPARE(slurp(file("laser-12")), bytes);
    set_value(*w, 0, QStringLiteral("0.5"));
    QVERIFY(w->diagnostic_lines().isEmpty());
  }

  void revertRestoresTheFile() {
    auto w = window();
    QVERIFY(w->open(QStringLiteral("laser-12")));
    const auto original = w->model().tmpl();
    set_value(*w, 0, QStringLiteral("9"));
    w->model().remove_rows({3});
    QVERIFY(w->modified());
    w->revert();
    QVERIFY(!w->modified());
    QVERIFY(w->model().tmpl() == original);
    QCOMPARE(w->model().rowCount(), 12);
  }

  void saveAsCopiesAndOpensTheCopy() {
    auto w = window();
    QVERIFY(w->open(QStringLiteral("laser-12")));
    const std::string bytes = slurp(file("laser-12"));
    set_value(*w, 0, QStringLiteral("0.25"));
    QSignalSpy files(w.get(), &StepHeatEditorWindow::filesChanged);
    QString error;
    QVERIFY(!w->save_as(QStringLiteral("laser-12"), &error));  // exists
    QVERIFY2(w->save_as(QStringLiteral("laser-12b"), &error), qPrintable(error));
    QCOMPARE(w->current_name(), QStringLiteral("laser-12b"));
    QVERIFY(!w->modified());
    QCOMPARE(files.count(), 1);
    QCOMPARE(slurp(file("laser-12")), bytes);  // the old file is kept as it was
    auto copy = ex::load_step_heat(file("laser-12b"));
    QVERIFY(copy.has_value());
    QCOMPARE(copy->name, std::string("laser-12b"));
    QCOMPARE(copy->steps[0].value, 0.25);
    QCOMPARE(copy->description, std::string("CO2, 12 steps, sanidine"));
  }

  void duplicateAndDeleteAskFirst() {
    auto w = window();
    QSignalSpy files(w.get(), &StepHeatEditorWindow::filesChanged);
    QString error;
    QVERIFY2(w->duplicate(QStringLiteral("laser-12"), QStringLiteral("copy"), &error), qPrintable(error));
    QCOMPARE(w->current_name(), QStringLiteral("copy"));
    QVERIFY(fs::exists(file("copy")));
    QCOMPARE(files.count(), 1);
    QVERIFY(!w->duplicate(QStringLiteral("laser-12"), QStringLiteral("copy"), &error));  // exists

    int asked = 0;
    w->set_confirm([&](const QString& q) {
      ++asked;
      return !q.contains(QStringLiteral("copy"));
    });
    QVERIFY(!w->delete_file(QStringLiteral("copy")));
    QCOMPARE(asked, 1);
    QVERIFY(fs::exists(file("copy")));
    QCOMPARE(files.count(), 1);

    w->set_confirm([&](const QString&) {
      ++asked;
      return true;
    });
    QVERIFY(w->delete_file(QStringLiteral("copy"), &error));
    QCOMPARE(asked, 2);
    QVERIFY(!fs::exists(file("copy")));
    QCOMPARE(files.count(), 2);
    QVERIFY(!w->file_names().contains(QStringLiteral("copy")));
    QCOMPARE(w->current_name(), QStringLiteral("laser-12"));
  }

  void unsavedChangesAskedOnSwitch() {
    make_lab();
    spit(file("b"), "[[steps]]\nvalue = 1\n");
    auto w = window();
    QVERIFY(w->open(QStringLiteral("laser-12")));
    set_value(*w, 0, QStringLiteral("9"));
    int asked = 0;
    w->set_ask_unsaved([&](const QString&) {
      ++asked;
      return StepHeatEditorWindow::Unsaved::Cancel;
    });
    QVERIFY(!w->open(QStringLiteral("b")));
    QCOMPARE(asked, 1);
    QCOMPARE(w->current_name(), QStringLiteral("laser-12"));
    QVERIFY(w->modified());
    QCOMPARE(w->model().tmpl().steps[0].value, 9.0);

    w->set_ask_unsaved([&](const QString&) {
      ++asked;
      return StepHeatEditorWindow::Unsaved::Discard;
    });
    QVERIFY(w->open(QStringLiteral("b")));
    QCOMPARE(asked, 2);
    QCOMPARE(w->current_name(), QStringLiteral("b"));
    QVERIFY(!w->modified());
    QCOMPARE(ex::load_step_heat(file("laser-12"))->steps[0].value, 0.5);  // discarded
  }

  void commentsAskedBeforeSave() {
    make_lab();
    spit(file("c"), "# note\n[[steps]]\nvalue = 1 # watts\n");
    auto w = window();
    QVERIFY(w->open(QStringLiteral("c")));
    int asked = 0;
    w->set_confirm([&](const QString&) {
      ++asked;
      return true;
    });
    set_value(*w, 0, QStringLiteral("2"));
    QVERIFY(w->save());
    QCOMPARE(asked, 1);
    QVERIFY(slurp(file("c")).find('#') == std::string::npos);
    set_value(*w, 0, QStringLiteral("3"));
    QVERIFY(w->save());
    QCOMPARE(asked, 1);  // the comments are gone: nothing to ask
  }

  void totalsSayCountAndTime() {
    auto w = window();
    ex::ExtractionSpec form;
    form.duration = ex::Duration{10};
    w->set_form_extraction([form] { return form; });
    QVERIFY(w->open(QStringLiteral("laser-12")));
    const QString totals = w->totals_text();
    QVERIFY2(totals.startsWith(QStringLiteral("12 steps, ")), qPrintable(totals));
    QVERIFY2(totals.endsWith(QStringLiteral(" extraction")), qPrintable(totals));
    // 12 steps of 30 s duration and 15 s cleanup at least: 9 min.
    const double seconds = ex::extraction_time(form, w->model().tmpl()).count();
    QVERIFY(seconds >= 12 * 45);
    QVERIFY2(totals.contains(QStringLiteral("%1 min").arg(std::llround(seconds / 60))), qPrintable(totals));
    w->model().remove_rows({0});
    QVERIFY(w->totals_text().startsWith(QStringLiteral("11 steps, ")));
  }

  void newFromFactoryText() {
    auto w = window();
    QVERIFY(!w->new_from_factory());  // no hook
    w->set_from_factory([]() -> std::optional<ex::StepHeatTemplate> { return std::nullopt; });
    QVERIFY(!w->new_from_factory());
    w->set_from_factory([]() -> std::optional<ex::StepHeatTemplate> {
      return ex::template_from_values({5, 10}, ex::FactoryForm{});
    });
    QVERIFY(!w->new_from_factory());  // the name dialog cancelled
    w->set_ask_name([]() -> std::optional<QString> { return QStringLiteral("fromtext"); });
    QVERIFY(w->new_from_factory());
    QCOMPARE(w->current_name(), QStringLiteral("fromtext"));
    QCOMPARE(w->model().rowCount(), 2);
    QCOMPARE(w->model().tmpl().steps[1].value, 10.0);
    QCOMPARE(w->model().tmpl().name, std::string("fromtext"));
    QVERIFY(w->modified());
    QVERIFY(!fs::exists(file("fromtext")));
    QVERIFY(w->save());
    QVERIFY(fs::exists(file("fromtext")));
  }
};

QTEST_MAIN(TestStepHeatEditor)
#include "test_step_heat_editor.moc"
