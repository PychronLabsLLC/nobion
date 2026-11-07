// RunFactoryPanel against the example lab (its defaults.toml and queues/blocks/) and a
// QueueTableModel holding the example queue. No hardware.

#include <filesystem>
#include <fstream>
#include <memory>

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "identifier_source.hpp"
#include "queue_table_model.hpp"
#include "run_factory_panel.hpp"

using nobion::experiment::AnalysisType;
using nobion::experiment::FactoryForm;
using nobion::experiment::StepHeatTemplate;
using nobion::ui::QueueTableModel;
using nobion::ui::RunFactoryPanel;
using nobion::ui::PackageChoice;
using nobion::ui::PackageContents;
namespace lab = nobion::experiment::lab;

namespace {

// Records what the panel asks and answers nothing until the test does, so the
// order of the answers is the test's choice.
struct FakeIdentifierSource : nobion::ui::IdentifierSource {
  using PackagesDone = std::function<void(nobion::Result<std::vector<PackageChoice>>)>;
  using ContentsDone = std::function<void(nobion::Result<PackageContents>)>;
  std::vector<PackagesDone> package_calls;
  std::vector<std::pair<std::string, ContentsDone>> content_calls;
  void packages(QObject*, PackagesDone done) override { package_calls.push_back(std::move(done)); }
  void contents(QObject*, const std::string& package, ContentsDone done) override {
    content_calls.emplace_back(package, std::move(done));
  }
  void notify() { Q_EMIT changed(); }
};

std::vector<PackageChoice> two_packages() { return {{"p2", "NM-294"}, {"p1", "NM-293"}}; }

// Level C holds no identifier.
PackageContents p1_contents() {
  return {{"A", "B", "C"}, {{"66001", "FC-2", "A", 1}, {"66002", "", "A", 3}, {"66010", "bt-1", "B", 2}}};
}

QStringList all_three() {
  return {QStringLiteral("66001  FC-2  (A 1)"), QStringLiteral("66002  (A 3)"), QStringLiteral("66010  bt-1  (B 2)")};
}

}  // namespace

class TestRunFactoryPanel : public QObject {
  Q_OBJECT

  std::filesystem::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  std::unique_ptr<QueueTableModel> model_;
  std::vector<std::size_t> selection_;
  std::unique_ptr<RunFactoryPanel> panel_;
  std::vector<std::vector<std::size_t>> inserted_;

  QString identifier(int row) const {
    return QString::fromStdString(model_->queue().runs[static_cast<std::size_t>(row)].id.identifier);
  }
  FactoryForm with_identifier(const char* id, const char* position = "") const {
    FactoryForm f = panel_->form();
    f.identifier = id;
    f.position = position;
    return f;
  }
  static constexpr const char* kAOne =
      "[template]\nunits = \"watts\"\nduration = 10\ncleanup = 5\n\n"
      "[[steps]]\nvalue = 2\n\n[[steps]]\nvalue = 4\nduration = 20\n\n[[steps]]\nvalue = 6\n";
  static constexpr const char* kBTwo = "[[steps]]\nvalue = 1\n\n[[steps]]\nvalue = 2\n";
  void write_template(const char* name, const char* text) const {
    const auto d = dir_ / "queues" / "step_heats";
    std::filesystem::create_directories(d);
    std::ofstream(d / (std::string(name) + ".toml")) << text;
  }
  QLineEdit* step_heat_text() const { return panel_->findChild<QLineEdit*>(QStringLiteral("step_heat")); }
  QPushButton* button(const char* name) const { return panel_->findChild<QPushButton*>(QString::fromLatin1(name)); }
  QComboBox* combo(const char* name) const { return panel_->findChild<QComboBox*>(QString::fromLatin1(name)); }
  QLineEdit* edit() const { return panel_->findChild<QLineEdit*>(QStringLiteral("identifier")); }
  // The source set and its packages answered.
  void load(FakeIdentifierSource& fake) {
    panel_->set_identifier_source(&fake);
    fake.package_calls.at(0)(two_packages());
  }
  // NM-293 chosen and its contents answered.
  void choose_p1(FakeIdentifierSource& fake) {
    panel_->choose_package(2);
    fake.content_calls.back().second(p1_contents());
  }

 private slots:
  void initTestCase() {
    dir_ = nobion::ui::test::scratch_lab();
    // Written out of order: the combo sorts them.
    write_template("b-two", kBTwo);
    write_template("a-one", kAOne);
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(nobion::ui::test::lab_paths(dir_)));
    QVERIFY(lab_->problems.empty());
  }
  void cleanupTestCase() {
    lab_.reset();
    std::filesystem::remove_all(dir_);
  }
  void init() {
    model_ = std::make_unique<QueueTableModel>(lab_->ids, [this](const auto& q) { return lab::check_lab_queue(*lab_, q); });
    model_->set_queue(nobion::ui::test::example_queue(dir_, lab_->ids));
    selection_.clear();
    inserted_.clear();
    panel_ = std::make_unique<RunFactoryPanel>(*lab_, *model_, [this] { return selection_; });
    connect(panel_.get(), &RunFactoryPanel::inserted, this,
            [this](const std::vector<std::size_t>& rows) { inserted_.push_back(rows); });
  }
  void cleanup() {
    panel_.reset();
    model_.reset();
  }

  void startsFromTheLabsDefaultsAndChoices() {
    const FactoryForm f = panel_->form();
    QCOMPARE(f.plan, std::string("sim_multicollect"));
    QCOMPARE(f.script, std::string("sim_extract"));
    QCOMPARE(f.post_measurement, std::string("sim_pump"));
    QCOMPARE(f.value, 5.0);
    QCOMPARE(panel_->plan_choices(), QStringList{QStringLiteral("sim_multicollect")});
    QCOMPARE(panel_->script_choices(),
             (QStringList{QStringLiteral("laser_extract"), QStringLiteral("sim_air"), QStringLiteral("sim_blank"),
                          QStringLiteral("sim_cocktail"), QStringLiteral("sim_extract")}));
    QCOMPARE(panel_->block_choices(), QStringList{QStringLiteral("blank_pair")});
    // No identifier yet: nothing to add.
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("identifier")));
    QVERIFY(!panel_->add());
  }

  void addInsertsAfterTheSelectionAndAdvances() {
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->add_enabled());
    QCOMPARE(panel_->preview_text(), QStringLiteral("Adds 1 run(s): 20001 @1"));
    panel_->set_increment(1, 1);
    selection_ = {0};
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), 4);
    QCOMPARE(identifier(1), QStringLiteral("20001"));
    QCOMPARE(model_->queue().runs[1].measurement.plan, std::string("sim_multicollect"));
    QCOMPARE(inserted_.size(), std::size_t{1});
    QCOMPARE(inserted_.front(), std::vector<std::size_t>{1});
    QCOMPARE(panel_->form().identifier, std::string("20002"));
    QCOMPARE(panel_->form().position, std::string("2"));
    QVERIFY(model_->runnable());

    // Without a selection (or with "after selection" off) Add appends.
    selection_.clear();
    QVERIFY(panel_->add());
    QCOMPARE(identifier(4), QStringLiteral("20002"));
    selection_ = {0};
    panel_->set_insert_after_selection(false);
    QVERIFY(panel_->add());
    QCOMPARE(identifier(5), QStringLiteral("20003"));
  }

  void holesAndStepHeatsPreviewAndExpand() {
    FactoryForm f = with_identifier("20001", "1-3");
    f.identifier_step = 1;
    panel_->set_form(f);
    QCOMPARE(panel_->preview_text(), QStringLiteral("Adds 3 run(s): 20001 @1 … 20003 @3"));
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), 6);
    QCOMPARE(identifier(5), QStringLiteral("20003"));
    QCOMPARE(inserted_.back(), (std::vector<std::size_t>{3, 4, 5}));

    f = with_identifier("20010", "7");
    f.step_heat = "2:2:4";
    panel_->set_form(f);
    // A step heat's preview says what the schedule is.
    const auto heat = nobion::experiment::build_runs(panel_->form(), lab_->ids);
    QVERIFY(heat.has_value());
    QCOMPARE(panel_->preview_text(), QString::fromStdString(nobion::experiment::step_heat_summary(*heat)));
    QVERIFY(panel_->preview_text().startsWith(QStringLiteral("4 runs, steps A to D")));
    f.step_heat = "2, warm";
    panel_->set_form(f);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("not a number")));
    QVERIFY(!panel_->add());
    QCOMPARE(model_->rowCount(), 6);
  }

  void runsTheLabWouldRejectCannotBeAdded() {
    FactoryForm f = with_identifier("20001", "1");
    f.plan = "no_such_plan";
    panel_->set_form(f);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("no_such_plan")));
    f.plan = "sim_multicollect";
    f.script = "no_such_script";
    panel_->set_form(f);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("no_such_script")));
  }

  // The new runs are checked by the model's checker (the session's in the
  // app, which reads peak_center.toml as it is now), not the lab as loaded.
  void runsAreCheckedByTheModelsChecker() {
    panel_.reset();
    model_ = std::make_unique<QueueTableModel>(lab_->ids, [this](const auto& q) {
      auto c = lab::check_lab_queue(*lab_, q);
      for (std::size_t i = 0; i < q.runs.size(); ++i)
        c.extra.push_back({nobion::experiment::Severity::Error, static_cast<int>(i), "peak_center", "checked afresh"});
      return c;
    });
    model_->set_queue(nobion::ui::test::example_queue(dir_, lab_->ids));
    panel_ = std::make_unique<RunFactoryPanel>(*lab_, *model_, [this] { return selection_; });
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("checked afresh")));
  }

  void fieldsFollowTheTypeAndATypeChangeAppliesDefaults() {
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->field_enabled("value"));
    QVERIFY(panel_->field_enabled("position"));
    // Typing a special identifier switches to its type and its defaults.
    auto* edit = panel_->findChild<QLineEdit*>(QStringLiteral("identifier"));
    QVERIFY(edit != nullptr);
    edit->setText(QString());
    QTest::keyClicks(edit, QStringLiteral("a"));
    QCOMPARE(panel_->form().identifier, std::string("a"));
    QVERIFY(!panel_->field_enabled("value"));
    QVERIFY(!panel_->field_enabled("step_heat"));
    QVERIFY(!panel_->field_enabled("position"));
    QVERIFY(panel_->field_enabled("plan"));
    QVERIFY(panel_->field_enabled("script"));
    QCOMPARE(panel_->form().post_measurement, std::string("sim_pump"));
    QCOMPARE(panel_->form().script, std::string(""));  // air's defaults have no script
    QVERIFY(panel_->add());
    const auto& added = model_->queue().runs.back();
    QCOMPARE(added.id.type, AnalysisType::Air);
    QVERIFY(!added.extraction.position.has_value());

    // No defaults for detector_ic: Defaults says so and changes nothing.
    panel_->set_form(with_identifier("ic"));
    const FactoryForm before = panel_->form();
    QVERIFY(!panel_->apply_defaults());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("No defaults")));
    QCOMPARE(panel_->form(), before);
  }

  void conditionalsAreChosenFromTheLabsFiles() {
    QCOMPARE(panel_->conditional_choices(), (QStringList{QStringLiteral("system"), QStringLiteral("default_unknown")}));
    QVERIFY(panel_->form().conditionals.empty());
    QCOMPARE(panel_->conditionals_text(), QStringLiteral("(none)"));
    panel_->set_conditional_checked(QStringLiteral("default_unknown"), true);
    QCOMPARE(panel_->form().conditionals, std::vector<std::string>{"default_unknown"});
    QCOMPARE(panel_->conditionals_text(), QStringLiteral("default_unknown"));

    FactoryForm f = with_identifier("20001", "1");
    f.conditionals.clear();
    panel_->set_form(f);  // the form's list replaces the ticks
    QVERIFY(panel_->form().conditionals.empty());
    QCOMPARE(panel_->conditionals_text(), QStringLiteral("(none)"));
    f.conditionals = {"default_unknown", "gone"};  // one the lab no longer has: kept, shown
    panel_->set_form(f);
    QCOMPARE(panel_->form().conditionals, (std::vector<std::string>{"default_unknown", "gone"}));
    QVERIFY(panel_->conditional_choices().contains(QStringLiteral("gone")));
    panel_->set_conditional_checked(QStringLiteral("gone"), false);

    QVERIFY(panel_->add());
    const auto& run = model_->queue().runs.back();
    QCOMPARE(run.id.identifier, std::string("20001"));
    QCOMPARE(run.conditionals.size(), std::size_t{1});
    QCOMPARE(run.conditionals[0].name, std::string("default_unknown"));
    // The next run keeps the choice.
    QCOMPARE(panel_->form().conditionals, std::vector<std::string>{"default_unknown"});

    // A file made while the panel is up appears on refresh; ticks stay.
    std::ofstream(dir_ / "conditionals" / "run_x.toml") << "";
    panel_->refresh_conditionals();
    QVERIFY(panel_->conditional_choices().contains(QStringLiteral("run_x")));
    QCOMPARE(panel_->form().conditionals, std::vector<std::string>{"default_unknown"});
    std::filesystem::remove(dir_ / "conditionals" / "run_x.toml");
  }

  void fromRowFillsTheForm() {
    QVERIFY(!panel_->load_from_selection());
    selection_ = {2};
    QVERIFY(panel_->load_from_selection());
    const FactoryForm f = panel_->form();
    QCOMPARE(f.identifier, std::string("66001"));
    QCOMPARE(f.value, 8.0);
    QCOMPARE(f.duration_s, 10.0);
    QCOMPARE(f.overrides.size(), std::size_t{1});  // "main.cycles" = 1
    QVERIFY(panel_->add());
    QCOMPARE(model_->queue().runs[3].measurement.overrides, model_->queue().runs[2].measurement.overrides);
  }

  void stepHeatCombosListsAlphabetically() {
    QCOMPARE(panel_->step_heat_choices(), (QStringList{QStringLiteral("none"), QStringLiteral("a-one"),
                                                       QStringLiteral("b-two"), QStringLiteral("laser-12")}));
    QCOMPARE(panel_->form().step_heat_template, std::string());
  }

  void pickingATemplateDisablesTheText() {
    FactoryForm f = with_identifier("20010", "7");
    f.step_heat = "5, 10";
    panel_->set_form(f);
    QVERIFY(panel_->field_enabled("step_heat"));
    panel_->choose_step_heat(QStringLiteral("a-one"));
    QVERIFY(!panel_->field_enabled("step_heat"));
    QCOMPARE(step_heat_text()->text(), QString());
    QCOMPARE(panel_->form().step_heat_template, std::string("a-one"));
    QCOMPARE(panel_->form().step_heat, std::string());
    QVERIFY(panel_->add_enabled());
    // Text typed puts the combo back on "none".
    step_heat_text()->setText(QStringLiteral("5, 10"));
    Q_EMIT step_heat_text()->textEdited(QStringLiteral("5, 10"));
    QCOMPARE(combo("step_heat_template")->currentText(), QStringLiteral("none"));
    QCOMPARE(panel_->form().step_heat_template, std::string());
    QCOMPARE(panel_->form().step_heat, std::string("5, 10"));
    QVERIFY(panel_->field_enabled("step_heat"));
    // A pick any other way (a wheel over the combo) clears the text too.
    combo("step_heat_template")->setCurrentIndex(2);
    QCOMPARE(panel_->form().step_heat, std::string());
    QCOMPARE(panel_->form().step_heat_template, std::string("b-two"));
    QVERIFY(!panel_->field_enabled("step_heat"));
    // set_form carries the name both ways.
    f.step_heat.clear();
    f.step_heat_template = "b-two";
    panel_->set_form(f);
    QCOMPARE(combo("step_heat_template")->currentText(), QStringLiteral("b-two"));
    QCOMPARE(panel_->form().step_heat_template, std::string("b-two"));
  }

  void previewSaysCountAndTime() {
    FactoryForm f = with_identifier("20010", "7");
    f.step_heat_template = "a-one";
    panel_->set_form(f);
    const auto runs = nobion::experiment::build_runs(panel_->form(), lab_->ids, lab_->step_heats);
    QVERIFY(runs.has_value());
    QCOMPARE(runs->size(), std::size_t{3});
    QCOMPARE(panel_->preview_text(), QString::fromStdString(nobion::experiment::step_heat_summary(*runs)));
    QVERIFY2(panel_->preview_text().startsWith(QStringLiteral("3 runs, steps A to C, 2 to 6")),
             qPrintable(panel_->preview_text()));
  }

  void addInsertsOneRowPerStep() {
    FactoryForm f = with_identifier("20010", "7");
    f.step_heat_template = "a-one";
    panel_->set_form(f);
    const int before = model_->rowCount();
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), before + 3);
    const auto& runs = model_->queue().runs;
    const auto at = static_cast<std::size_t>(before);
    const std::vector<std::string> steps{"A", "B", "C"};
    const std::vector<double> values{2, 4, 6};
    for (std::size_t i = 0; i < 3; ++i) {
      QCOMPARE(runs[at + i].id.step, steps[i]);
      QCOMPARE(runs[at + i].extraction.value, values[i]);
    }
    QCOMPARE(runs[at + 1].extraction.duration, nobion::experiment::Duration(20));
    QCOMPARE(runs[at + 2].extraction.duration, nobion::experiment::Duration(10));
    QCOMPARE(panel_->form().step_heat_template, std::string("a-one"));  // kept for the next Add
  }

  void deletedTemplateLeavesCombo() {
    panel_->set_form(with_identifier("20010", "7"));
    panel_->choose_step_heat(QStringLiteral("a-one"));
    panel_->refresh_step_heats();  // still there: kept
    QCOMPARE(panel_->form().step_heat_template, std::string("a-one"));
    panel_->choose_step_heat(QStringLiteral("b-two"));
    const auto file = dir_ / "queues" / "step_heats" / "b-two.toml";
    std::filesystem::remove(file);
    panel_->refresh_step_heats();
    write_template("b-two", kBTwo);  // for the tests after this one
    QVERIFY(!panel_->step_heat_choices().contains(QStringLiteral("b-two")));
    QCOMPARE(combo("step_heat_template")->currentText(), QStringLiteral("none"));
    QCOMPARE(panel_->form().step_heat_template, std::string());
    QVERIFY(panel_->field_enabled("step_heat"));
  }

  void saveAsTemplateEmitsTheTextAsATemplate() {
    std::vector<StepHeatTemplate> asked;
    connect(panel_.get(), &RunFactoryPanel::saveStepHeatRequested, this,
            [&](const StepHeatTemplate& t) { asked.push_back(t); });
    FactoryForm f = with_identifier("20010", "7");
    f.step_heat = "warm";
    panel_->set_form(f);
    QVERIFY(!panel_->field_enabled("save_step_heat"));
    f.step_heat = "5,10";
    panel_->set_form(f);
    QVERIFY(panel_->field_enabled("save_step_heat"));
    button("save_step_heat")->click();
    QCOMPARE(asked.size(), std::size_t{1});
    QVERIFY(asked.front() == nobion::experiment::template_from_values({5, 10}, panel_->form()));
    // A picked template has no text to save.
    panel_->choose_step_heat(QStringLiteral("a-one"));
    QVERIFY(!panel_->field_enabled("save_step_heat"));
  }

  void editOpensThePickedTemplate() {
    QStringList asked;
    connect(panel_.get(), &RunFactoryPanel::editStepHeatRequested, this,
            [&](const QString& name) { asked.append(name); });
    panel_->set_form(with_identifier("20010", "7"));
    button("edit_step_heat")->click();
    panel_->choose_step_heat(QStringLiteral("b-two"));
    button("edit_step_heat")->click();
    QCOMPARE(asked, (QStringList{QString(), QStringLiteral("b-two")}));
  }

  void typeThatDoesNotHeatDisablesTheRow() {
    FactoryForm f = with_identifier("a");
    f.step_heat = "5,10";
    panel_->set_form(f);
    for (const char* name : {"step_heat_template", "edit_step_heat", "save_step_heat", "step_heat"})
      QVERIFY2(!panel_->field_enabled(name), name);
    panel_->set_form(with_identifier("bu"));
    for (const char* name : {"step_heat_template", "edit_step_heat", "step_heat"})
      QVERIFY2(panel_->field_enabled(name), name);
  }

  void editStaysEnabledWhenLocked() {
    panel_->set_form(with_identifier("20001", "1"));
    panel_->set_locked(true);
    QVERIFY(panel_->field_enabled("edit_step_heat"));
    QVERIFY(!panel_->field_enabled("step_heat_template"));
    QVERIFY(!panel_->field_enabled("step_heat"));
    panel_->set_locked(false);
    QVERIFY(panel_->field_enabled("step_heat_template"));
  }

  void frequencyAndBlocks() {
    // A blank after every unknown in the example queue (two unknowns).
    panel_->set_frequency(AnalysisType::BlankUnknown, 1, false, false);
    QVERIFY(panel_->insert_frequency());
    QCOMPARE(model_->rowCount(), 5);
    QCOMPARE(identifier(2), QStringLiteral("bu"));
    QCOMPARE(identifier(4), QStringLiteral("bu"));
    QCOMPARE(model_->queue().runs[4].measurement.plan, std::string("sim_multicollect"));  // from defaults

    selection_ = {0};
    panel_->set_block(QStringLiteral("blank_pair"), 2);
    QVERIFY(panel_->insert_block());
    QCOMPARE(model_->rowCount(), 9);
    QCOMPARE(inserted_.back(), (std::vector<std::size_t>{1, 2, 3, 4}));
    QVERIFY(model_->runnable());
  }

  void lockedRefusesEverything() {
    panel_->set_form(with_identifier("20001", "1"));
    panel_->set_locked(true);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(!panel_->add());
    QVERIFY(!panel_->insert_frequency());
    QVERIFY(!panel_->insert_block());
    QCOMPARE(model_->rowCount(), 3);
    panel_->set_locked(false);
    QVERIFY(panel_->add_enabled());
    // A locked model (queue running) refuses too.
    model_->set_locked(true);
    QVERIFY(!panel_->add());
    QCOMPARE(model_->rowCount(), 3);
  }

  // While a queue runs, runs go after the rows the executor has reached even
  // when an earlier row is selected.
  void aRunningQueueTakesRunsAfterTheReachedRows() {
    model_->set_live(2, [](std::uint64_t base, const auto&) -> nobion::Result<std::uint64_t> { return base + 1; });
    panel_->set_insert_after_selection(true);
    selection_ = {0};
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->add_enabled());
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), 4);
    QCOMPARE(inserted_.back(), std::vector<std::size_t>{2});
    QCOMPARE(identifier(2), QStringLiteral("20001"));
    QCOMPARE(model_->version(), 1u);
  }
  // ---- the identifier select (identifier-select design)

  void noSourceLeavesThePanelAsItWas() {
    QVERIFY(!panel_->selects_visible());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(edit() != nullptr);
  }

  void packagesAreListedAfterNone() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    QCOMPARE(fake.package_calls.size(), std::size_t{1});
    QVERIFY(!panel_->selects_visible());
    fake.package_calls[0](two_packages());
    QVERIFY(panel_->selects_visible());
    QCOMPARE(panel_->package_choices(),
             (QStringList{QStringLiteral("(none)"), QStringLiteral("NM-294"), QStringLiteral("NM-293")}));
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(fake.content_calls.empty());
  }

  void aPackageListsEveryLevelsIdentifiers() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    QCOMPARE(fake.content_calls.size(), std::size_t{1});
    QCOMPARE(fake.content_calls[0].first, std::string("p1"));
    fake.content_calls[0].second(p1_contents());
    QCOMPARE(panel_->level_choices(), (QStringList{QStringLiteral("(all)"), QStringLiteral("A"), QStringLiteral("B"),
                                                   QStringLiteral("C")}));
    QCOMPARE(panel_->identifier_choices(), all_three());
    QVERIFY(combo("level")->isEnabled());
  }

  void aLevelNarrowsWithoutAskingAgain() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(1);
    QCOMPARE(panel_->identifier_choices(),
             (QStringList{QStringLiteral("66001  FC-2  (A 1)"), QStringLiteral("66002  (A 3)")}));
    panel_->choose_level(3);
    QVERIFY(panel_->identifier_choices().isEmpty());
    panel_->choose_level(0);
    QCOMPARE(panel_->identifier_choices(), all_three());
    QCOMPARE(fake.content_calls.size(), std::size_t{1});
  }

  void pickingAnItemIsTypingItsIdentifier() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    QVERIFY(panel_->preview_text().contains(QStringLiteral("identifier")));
    panel_->choose_identifier(2);
    QCOMPARE(panel_->form().identifier, std::string("66010"));
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
    QVERIFY(!panel_->preview_text().contains(QStringLiteral("identifier")));
  }

  void aTypedSpecialStillSwitchesType() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(1);
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->field_enabled("value"));
    edit()->setText(QString());
    QTest::keyClicks(edit(), QStringLiteral("a"));
    QCOMPARE(panel_->form().identifier, std::string("a"));
    QVERIFY(!panel_->field_enabled("value"));  // air: its type's rules, as when there is no select
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentIndex(), 1);
  }

  void setFormLeavesTheSelects() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(1);
    const QStringList packages = panel_->package_choices();
    const QStringList identifiers = panel_->identifier_choices();
    panel_->set_form(with_identifier("20001", "1"));
    QCOMPARE(edit()->text(), QStringLiteral("20001"));
    QCOMPARE(panel_->package_choices(), packages);
    QCOMPARE(panel_->identifier_choices(), identifiers);
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentIndex(), 1);
  }

  void choosingAnotherPackageResetsLevel() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(2);
    panel_->choose_package(1);
    QCOMPARE(fake.content_calls.back().first, std::string("p2"));
    QCOMPARE(combo("level")->currentIndex(), 0);
    QCOMPARE(panel_->level_choices(), QStringList{QStringLiteral("(all)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    panel_->choose_package(0);
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
  }

  void packagesOfOneNameAreToldApartById() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    fake.package_calls.at(0)(std::vector<PackageChoice>{{"x", "Dup"}, {"y", "Dup"}});
    panel_->choose_package(2);
    QCOMPARE(fake.content_calls.back().first, std::string("y"));
  }

  void anEmptyPackageIsNotAnError() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    fake.content_calls.back().second(PackageContents{});
    QCOMPARE(panel_->level_choices(), QStringList{QStringLiteral("(all)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(panel_->identifier_tooltip().isEmpty());
  }

  void lockedDisablesTheSelects() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->set_locked(true);
    QVERIFY(!combo("package")->isEnabled());
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(!combo("identifier_select")->isEnabled());
    panel_->set_locked(false);
    QVERIFY(combo("package")->isEnabled());
    QVERIFY(combo("identifier_select")->isEnabled());
    QVERIFY(!combo("level")->isEnabled());
  }
  void aLateAnswerForAnotherPackageIsDropped() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    panel_->choose_package(1);
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
    fake.content_calls[1].second(PackageContents{{"A"}, {{"70001", "", "A", 1}}});
    fake.content_calls[0].second(p1_contents());
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("70001  (A 1)")});
    QCOMPARE(panel_->level_choices(), (QStringList{QStringLiteral("(all)"), QStringLiteral("A")}));
  }

  void aCatalogChangeReloadsAndKeepsTheChoice() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(2);
    panel_->choose_identifier(0);
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
    fake.notify();
    QCOMPARE(fake.package_calls.size(), std::size_t{2});
    fake.package_calls[1](two_packages());
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
    QCOMPARE(fake.content_calls[1].first, std::string("p1"));
    PackageContents more = p1_contents();
    more.choices.push_back({"66011", "", "B", 4});
    fake.content_calls[1].second(more);
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentText(), QStringLiteral("B"));
    QCOMPARE(panel_->identifier_choices(),
             (QStringList{QStringLiteral("66010  bt-1  (B 2)"), QStringLiteral("66011  (B 4)")}));
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
  }

  void aCatalogChangeDropsAChoiceThatIsGone() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(2);
    // The level went away: back to all of the package.
    fake.notify();
    fake.package_calls.back()(two_packages());
    fake.content_calls.back().second(PackageContents{{"A"}, {{"66001", "FC-2", "A", 1}}});
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentIndex(), 0);
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("66001  FC-2  (A 1)")});
    // The package went away: nothing chosen, and nothing asked about it.
    const std::size_t asked = fake.content_calls.size();
    fake.notify();
    fake.package_calls.back()(std::vector<PackageChoice>{{"p2", "NM-294"}});
    QCOMPARE(combo("package")->currentIndex(), 0);
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QCOMPARE(fake.content_calls.size(), asked);
  }

  void aChangeWhileContentsArePendingWins() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    fake.notify();
    fake.package_calls.back()(two_packages());
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
    fake.content_calls[1].second(PackageContents{{"A"}, {{"70001", "", "A", 1}}});
    fake.content_calls[0].second(p1_contents());
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("70001  (A 1)")});
  }

  void aFirstPackagesFailureKeepsTheRowsHidden() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    fake.package_calls.at(0)(nobion::fail(nobion::ErrorKind::Io, "store gone"));
    QVERIFY(!panel_->selects_visible());
  }

  void aLaterPackagesFailureKeepsTheList() {
    FakeIdentifierSource fake;
    load(fake);
    const QStringList before = panel_->package_choices();
    fake.notify();
    fake.package_calls.back()(nobion::fail(nobion::ErrorKind::Io, "store gone"));
    QCOMPARE(panel_->package_choices(), before);
    QVERIFY(panel_->selects_visible());
    QVERIFY(panel_->package_tooltip().contains(QStringLiteral("store gone")));
    fake.notify();
    fake.package_calls.back()(two_packages());
    QVERIFY(panel_->package_tooltip().isEmpty());
  }

  void aContentsFailureEmptiesTheListAndSaysWhy() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_package(2);
    fake.content_calls.back().second(nobion::fail(nobion::ErrorKind::Io, "no such level"));
    QCOMPARE(panel_->level_choices(), QStringList{QStringLiteral("(all)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(panel_->identifier_tooltip().contains(QStringLiteral("no such level")));
    choose_p1(fake);
    QVERIFY(panel_->identifier_tooltip().isEmpty());
    QCOMPARE(panel_->identifier_choices(), all_three());
  }

  void anAnswerWhileLockedFillsButStaysDisabled() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    panel_->set_locked(true);
    fake.content_calls.back().second(p1_contents());
    QCOMPARE(panel_->identifier_choices(), all_three());
    for (const char* name : {"package", "level", "identifier_select"}) QVERIFY(!combo(name)->isEnabled());
    panel_->set_locked(false);
    for (const char* name : {"package", "level", "identifier_select"}) QVERIFY(combo(name)->isEnabled());
  }

  void aSourceThatGoesAwayHidesTheSelects() {
    auto* fake = new FakeIdentifierSource;
    load(*fake);
    choose_p1(*fake);
    panel_->choose_identifier(0);
    delete fake;
    QVERIFY(!panel_->selects_visible());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QCOMPARE(edit()->text(), QStringLiteral("66001"));
    panel_->set_form(panel_->form());
    QCOMPARE(panel_->form().identifier, std::string("66001"));
  }

  void aSourceCanBeReplacedOrCleared() {
    FakeIdentifierSource a;
    FakeIdentifierSource b;
    load(a);
    choose_p1(a);
    panel_->choose_package(1);  // left unanswered
    panel_->set_identifier_source(nullptr);
    QVERIFY(!panel_->selects_visible());
    QCOMPARE(panel_->package_choices(), QStringList{QStringLiteral("(none)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    panel_->set_identifier_source(&b);
    QCOMPARE(b.package_calls.size(), std::size_t{1});
    a.notify();
    QCOMPARE(a.package_calls.size(), std::size_t{1});
    QCOMPARE(b.package_calls.size(), std::size_t{1});
    a.content_calls.back().second(p1_contents());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(!panel_->selects_visible());
  }
  // ---- through the widgets themselves, as the operator's hand goes

  void theSelectsAreWiredToTheirWidgets() {
    FakeIdentifierSource fake;
    load(fake);
    combo("package")->setCurrentIndex(2);
    Q_EMIT combo("package")->activated(2);
    QCOMPARE(fake.content_calls.size(), std::size_t{1});
    fake.content_calls[0].second(p1_contents());
    combo("level")->setCurrentIndex(2);
    Q_EMIT combo("level")->activated(2);
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("66010  bt-1  (B 2)")});
    combo("identifier_select")->setCurrentIndex(0);
    Q_EMIT combo("identifier_select")->activated(0);
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
    QCOMPARE(panel_->form().identifier, std::string("66010"));
  }

  void typedTextSurvivesEnterWithAFilledDropdown() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    edit()->setText(QString());
    QTest::keyClicks(edit(), QStringLiteral("66002"));
    QTest::keyClick(edit(), Qt::Key_Return);
    QCOMPARE(edit()->text(), QStringLiteral("66002"));
    QTest::keyClicks(edit(), QStringLiteral("9"));
    QTest::keyClick(edit(), Qt::Key_Return);
    QCOMPARE(panel_->form().identifier, std::string("660029"));
  }

  // The form scrolls under the pointer: the wheel must not walk the identifier.
  void theWheelOverTheIdentifierChangesNothing() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_identifier(1);
    QCOMPARE(edit()->text(), QStringLiteral("66002"));
    for (QWidget* target : {static_cast<QWidget*>(combo("identifier_select")), static_cast<QWidget*>(edit())}) {
      for (int delta : {-120, 120}) {
        QWheelEvent wheel(QPointF(5, 5), target->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, delta), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &wheel);
        QVERIFY(!wheel.isAccepted());  // left for the scroll area
        QCOMPARE(edit()->text(), QStringLiteral("66002"));
      }
    }
  }

  // Why there are no selects is said where it can be read, and showing the
  // panel again asks again.
  void aFirstFailureSaysWhyAndIsTriedAgainWhenShown() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    panel_->show();
    QCOMPARE(fake.package_calls.size(), std::size_t{1});  // one is out: showing asks nothing more
    fake.package_calls[0](nobion::fail(nobion::ErrorKind::Io, "store gone"));
    QVERIFY(!panel_->selects_visible());
    QVERIFY(panel_->identifier_tooltip().contains(QStringLiteral("store gone")));
    panel_->hide();
    panel_->show();
    QCOMPARE(fake.package_calls.size(), std::size_t{2});
    fake.package_calls[1](two_packages());
    QVERIFY(panel_->selects_visible());
    QVERIFY(panel_->identifier_tooltip().isEmpty());
    panel_->hide();
    panel_->show();
    QCOMPARE(fake.package_calls.size(), std::size_t{2});  // loaded: nothing to try again
  }
};

QTEST_MAIN(TestRunFactoryPanel)
#include "test_run_factory_panel.moc"
