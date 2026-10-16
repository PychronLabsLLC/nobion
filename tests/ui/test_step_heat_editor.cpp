// The step heat template editor: the table model over one template's steps.

#include <optional>
#include <vector>

#include <QGuiApplication>
#include <QPalette>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "step_heat_table_model.hpp"

using pychron::ui::StepHeatTableModel;
namespace ex = pychron::experiment;

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

}  // namespace

class TestStepHeatEditor : public QObject {
  Q_OBJECT

 private slots:
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
};

QTEST_MAIN(TestStepHeatEditor)
#include "test_step_heat_editor.moc"
