#pragma once

// StepHeatTableModel (step heat template design 7): the steps of one step heat
// template, one row per step, A, B, C, ... in order.
//
// A cell whose optional is empty inherits: it shows, greyed, the value
// resolve_step gives it against the form's extraction, and edits as empty
// text. Setting a cell to empty text clears the optional again. There is no
// undo stack: the editor window's Revert reloads the file.

#include <optional>
#include <string>
#include <vector>

#include <QAbstractTableModel>
#include <QString>
#include <QVariant>

#include "pychron/core/error.hpp"
#include "pychron/experiment/factory/step_heat_template.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/model/types.hpp"

namespace pychron::ui {

class StepHeatTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { Step, Value, Units, Duration, Cleanup, BeamDiameter, RampRate, ColumnCount };

  explicit StepHeatTableModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  // Replaces everything (no edited() signal: nothing was edited).
  void set_template(experiment::StepHeatTemplate t);
  [[nodiscard]] const experiment::StepHeatTemplate& tmpl() const noexcept { return t_; }

  // The [template] table: defaults for the steps, and the description.
  void set_template_defaults(std::optional<experiment::Unit> units, std::optional<experiment::Duration> duration,
                             std::optional<experiment::Duration> cleanup, std::string description);

  // The run factory's extraction, which inherited cells resolve against.
  void set_form_extraction(experiment::ExtractionSpec form);

  // Row operations. Each returns the affected row, or -1 / false when refused.
  // step_after(t, selected, else the last row), inserted after that row.
  int add_step(std::optional<int> selected);
  // A copy of `row`, inserted before it.
  int insert_step(int row);
  // Rows out of range are ignored.
  void remove_rows(std::vector<int> rows);
  bool move_up(int row);
  bool move_down(int row);
  // Sets column `c` to `v` on `rows` (every row when empty); empty text
  // clears the optional. Values that do not read change nothing.
  void fill(Column c, const QVariant& v, std::vector<int> rows);
  // parse_step_values(text) then experiment::fill_values: with rows, the
  // values go on them in order; with none, they are appended.
  [[nodiscard]] Result<void> fill_values(const std::vector<int>& rows, const QString& text);

 signals:
  void edited();

 private:
  [[nodiscard]] bool valid(int row) const { return row >= 0 && row < rowCount(); }
  // Writes one cell from text; false when the text does not read.
  static bool set_cell(experiment::StepHeatStep& s, int column, const QVariant& v);
  void letters_changed();
  void all_changed();

  experiment::StepHeatTemplate t_;
  experiment::ExtractionSpec form_;
};

}  // namespace pychron::ui
