#include "step_heat_table_model.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include <QGuiApplication>
#include <QPalette>

#include "nobion/experiment/factory/form.hpp"
#include "nobion/experiment/factory/increments.hpp"

namespace nobion::ui {

using experiment::StepHeatStep;
using experiment::Unit;

namespace {

QString number(double v) { return QString::number(v); }

std::optional<double> read_number(const QString& text) {
  bool ok = false;
  const double v = text.toDouble(&ok);
  return ok ? std::optional<double>(v) : std::nullopt;
}

// The step's own value of a column, as edit text; empty when it inherits.
QString own_text(const StepHeatStep& s, int column) {
  switch (column) {
    case StepHeatTableModel::Value: return number(s.value);
    case StepHeatTableModel::Units:
      return s.units ? QString::fromUtf8(to_string(*s.units).data()) : QString();
    case StepHeatTableModel::Duration: return s.duration ? number(s.duration->count()) : QString();
    case StepHeatTableModel::Cleanup: return s.cleanup ? number(s.cleanup->count()) : QString();
    case StepHeatTableModel::BeamDiameter: return s.beam_diameter ? number(*s.beam_diameter) : QString();
    case StepHeatTableModel::RampRate: return s.ramp_rate ? number(*s.ramp_rate) : QString();
    default: return {};
  }
}

bool is_inherited(const StepHeatStep& s, int column) {
  switch (column) {
    case StepHeatTableModel::Units: return !s.units;
    case StepHeatTableModel::Duration: return !s.duration;
    case StepHeatTableModel::Cleanup: return !s.cleanup;
    case StepHeatTableModel::BeamDiameter: return !s.beam_diameter;
    case StepHeatTableModel::RampRate: return !s.ramp_rate;
    default: return false;
  }
}

// What a run made from the step gets for a column.
QString resolved_text(const experiment::ExtractionSpec& e, int column) {
  switch (column) {
    case StepHeatTableModel::Value: return number(e.value);
    case StepHeatTableModel::Units: return QString::fromUtf8(to_string(e.units).data());
    case StepHeatTableModel::Duration: return number(e.duration.count());
    case StepHeatTableModel::Cleanup: return number(e.cleanup.count());
    case StepHeatTableModel::BeamDiameter: return e.beam_diameter ? number(*e.beam_diameter) : QString();
    case StepHeatTableModel::RampRate: return e.ramp_rate ? number(*e.ramp_rate) : QString();
    default: return {};
  }
}

}  // namespace

StepHeatTableModel::StepHeatTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int StepHeatTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(t_.steps.size());
}

int StepHeatTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant StepHeatTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  switch (section) {
    case Step: return tr("Step");
    case Value: return tr("Value");
    case Units: return tr("Units");
    case Duration: return tr("Duration (s)");
    case Cleanup: return tr("Cleanup (s)");
    case BeamDiameter: return tr("Beam diameter");
    case RampRate: return tr("Ramp rate");
    default: return {};
  }
}

Qt::ItemFlags StepHeatTableModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::NoItemFlags;
  const Qt::ItemFlags base = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  return index.column() == Step ? base : base | Qt::ItemIsEditable;
}

QVariant StepHeatTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || !valid(index.row())) return {};
  const StepHeatStep& s = t_.steps[static_cast<std::size_t>(index.row())];
  const int column = index.column();
  if (column == Step) {
    return role == Qt::DisplayRole ? QVariant(QString::fromStdString(experiment::step_name(index.row())))
                                   : QVariant();
  }
  switch (role) {
    case Qt::DisplayRole:
      if (is_inherited(s, column)) return resolved_text(experiment::resolve_step(form_, t_, s), column);
      return own_text(s, column);
    case Qt::EditRole: return own_text(s, column);
    case Qt::ForegroundRole:
      if (is_inherited(s, column)) return QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text);
      return {};
    default: return {};
  }
}

bool StepHeatTableModel::set_cell(StepHeatStep& s, int column, const QVariant& v) {
  const QString text = v.toString().trimmed();
  const bool clear = text.isEmpty();
  if (column == Units) {
    if (clear) {
      s.units.reset();
      return true;
    }
    const auto u = experiment::parse_unit(text.toStdString());
    if (!u) return false;
    s.units = u;
    return true;
  }
  std::optional<double> n;
  if (!clear) {
    n = read_number(text);
    if (!n) return false;
  }
  switch (column) {
    case Value:
      if (!n) return false;  // a step always has a value
      s.value = *n;
      return true;
    case Duration: s.duration = n ? std::optional<experiment::Duration>(*n) : std::nullopt; return true;
    case Cleanup: s.cleanup = n ? std::optional<experiment::Duration>(*n) : std::nullopt; return true;
    case BeamDiameter: s.beam_diameter = n; return true;
    case RampRate: s.ramp_rate = n; return true;
    default: return false;
  }
}

bool StepHeatTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (role != Qt::EditRole || !index.isValid() || !valid(index.row())) return false;
  StepHeatStep s = t_.steps[static_cast<std::size_t>(index.row())];
  if (!set_cell(s, index.column(), value)) return false;
  t_.steps[static_cast<std::size_t>(index.row())] = s;
  emit dataChanged(this->index(index.row(), 0), this->index(index.row(), ColumnCount - 1));
  emit edited();
  return true;
}

void StepHeatTableModel::set_template(experiment::StepHeatTemplate t) {
  beginResetModel();
  t_ = std::move(t);
  endResetModel();
}

void StepHeatTableModel::set_template_defaults(std::optional<Unit> units, std::optional<experiment::Duration> duration,
                                               std::optional<experiment::Duration> cleanup, std::string description) {
  t_.units = units;
  t_.duration = duration;
  t_.cleanup = cleanup;
  t_.description = std::move(description);
  all_changed();
  emit edited();
}

void StepHeatTableModel::set_form_extraction(experiment::ExtractionSpec form) {
  form_ = std::move(form);
  all_changed();
}

void StepHeatTableModel::all_changed() {
  if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
}

void StepHeatTableModel::letters_changed() {
  if (rowCount() > 0) emit dataChanged(index(0, Step), index(rowCount() - 1, Step));
}

int StepHeatTableModel::add_step(std::optional<int> selected) {
  if (selected && !valid(*selected)) return -1;
  const int source = selected ? *selected : rowCount() - 1;
  const StepHeatStep s = experiment::step_after(t_, source < 0 ? 0 : static_cast<std::size_t>(source));
  const int row = source + 1;
  beginInsertRows({}, row, row);
  t_.steps.insert(t_.steps.begin() + row, s);
  endInsertRows();
  letters_changed();
  emit edited();
  return row;
}

int StepHeatTableModel::insert_step(int row) {
  if (!valid(row)) return -1;
  const StepHeatStep s = t_.steps[static_cast<std::size_t>(row)];
  beginInsertRows({}, row, row);
  t_.steps.insert(t_.steps.begin() + row, s);
  endInsertRows();
  letters_changed();
  emit edited();
  return row;
}

void StepHeatTableModel::remove_rows(std::vector<int> rows) {
  std::erase_if(rows, [this](int r) { return !valid(r); });
  std::ranges::sort(rows, std::greater<>());
  const auto dup = std::ranges::unique(rows);
  rows.erase(dup.begin(), dup.end());
  if (rows.empty()) return;
  for (const int r : rows) {
    beginRemoveRows({}, r, r);
    t_.steps.erase(t_.steps.begin() + r);
    endRemoveRows();
  }
  letters_changed();
  emit edited();
}

bool StepHeatTableModel::move_up(int row) { return valid(row) && move_down(row - 1); }

bool StepHeatTableModel::move_down(int row) {
  if (!valid(row) || !valid(row + 1)) return false;
  // Moving row + 1 to before row is the same swap, and is what Qt can say.
  beginMoveRows({}, row + 1, row + 1, {}, row);
  std::swap(t_.steps[static_cast<std::size_t>(row)], t_.steps[static_cast<std::size_t>(row) + 1]);
  endMoveRows();
  letters_changed();
  emit edited();
  return true;
}

void StepHeatTableModel::fill(Column c, const QVariant& v, std::vector<int> rows) {
  if (c == Step || c >= ColumnCount) return;
  if (rows.empty()) {
    rows.resize(t_.steps.size());
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<int>(i);
  }
  std::erase_if(rows, [this](int r) { return !valid(r); });
  if (rows.empty()) return;
  // All or nothing: the text reads for one row exactly when it reads for every row.
  auto steps = t_.steps;
  for (const int r : rows) {
    if (!set_cell(steps[static_cast<std::size_t>(r)], c, v)) return;
  }
  t_.steps = std::move(steps);
  all_changed();
  emit edited();
}

Result<void> StepHeatTableModel::fill_values(const std::vector<int>& rows, const QString& text) {
  auto values = experiment::parse_step_values(text.toStdString());
  if (!values) return fail(values.error());
  std::vector<std::size_t> at;
  at.reserve(rows.size());
  for (const int r : rows) {
    if (!valid(r)) return fail(ErrorKind::Config, "no step at row " + std::to_string(r));
    at.push_back(static_cast<std::size_t>(r));
  }
  auto t = t_;
  if (auto filled = experiment::fill_values(t, at, *values); !filled) return filled;
  beginResetModel();
  t_ = std::move(t);
  endResetModel();
  emit edited();
  return {};
}

}  // namespace nobion::ui
