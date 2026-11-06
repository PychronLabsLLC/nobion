#pragma once

// IrradiationTree (irradiation tree filter design, section 3): the data
// browser's filter by where an analysis was irradiated. Irradiations hold
// levels, levels hold identifiers, and a last node, "Not irradiated", holds
// the identifiers that have no position. Tick a node to keep everything under
// it. The line above the tree hides the rows that do not match its text; a
// ticked or partly ticked row is never hidden. The header (arrow and title,
// with the count of ticked nodes) folds the filter line and the tree away; the
// tree starts folded, and folded, its ticks still filter.

#include <optional>
#include <vector>

#include <QString>
#include <QWidget>

#include "nobion/processing/source.hpp"

class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace pychron::ui {

class IrradiationTree : public QWidget {
  Q_OBJECT

 public:
  // What a node is, kept in Qt::UserRole + 1 of its item; its key is in
  // Qt::UserRole.
  enum class Kind { Irradiation = 0, Level, Identifier, NotIrradiated };

  explicit IrradiationTree(const QString& title, QWidget* parent = nullptr);

  // The positions to offer. Ticks are kept, by key, and a ticked node no
  // longer among them stays, at the end of its parent (a parent that is gone
  // too is made again to hold it), with the label it had. Expanded nodes stay
  // expanded. The filter text stays and is applied again. Emits nothing.
  void set_rows(const std::vector<processing::PositionRow>& rows);
  // What is ticked, as the highest fully ticked nodes; nullopt when nothing is.
  std::optional<processing::PositionSelection> selection() const;
  // Unticks everything; changed() once when anything was ticked.
  void clear_checked();

  // Shows or folds away the filter line and the tree.
  void set_expanded(bool expanded);
  bool is_expanded() const noexcept;
  // The header's text: the title, and the count of ticked nodes.
  QString title() const;

  QTreeWidget* tree() const noexcept { return tree_; }
  QLineEdit* filter() const noexcept { return filter_; }
  QToolButton* toggle_button() const noexcept { return toggle_; }
  QToolButton* clear_button() const noexcept { return clear_; }

 signals:
  // The user ticked or unticked a node, or cleared the ticks.
  void changed();

 private:
  void apply_filter();
  void update_header();
  void on_item_changed(QTreeWidgetItem* item);

  QString title_;
  QToolButton* toggle_;
  QWidget* body_;
  QLineEdit* filter_;
  QToolButton* clear_;
  QTreeWidget* tree_;
  bool updating_ = false;
};

}  // namespace pychron::ui
