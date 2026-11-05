#include "irradiation_tree.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "theme.hpp"

namespace pychron::ui {

namespace {

constexpr int key_role = Qt::UserRole;
constexpr int kind_role = Qt::UserRole + 1;
// Set on a parent made again to hold a ticked node: it lists only that node,
// not all it once held, so it is never fully ticked.
constexpr int stand_in_role = Qt::UserRole + 2;

using Kind = IrradiationTree::Kind;

Kind kind_of(const QTreeWidgetItem* item) { return static_cast<Kind>(item->data(0, kind_role).toInt()); }
QString key_of(const QTreeWidgetItem* item) { return item->data(0, key_role).toString(); }

// One node, told apart by its kind as well as its key.
QString id_of(const QTreeWidgetItem* item) { return QString::number(item->data(0, kind_role).toInt()) + QLatin1Char(':') + key_of(item); }

QTreeWidgetItem* make_item(Kind kind, const QString& key, const QString& label) {
  auto* item = new QTreeWidgetItem;  // NOLINT(cppcoreguidelines-owning-memory): a parent or the tree owns it
  item->setText(0, label);
  item->setData(0, key_role, key);
  item->setData(0, kind_role, static_cast<int>(kind));
  item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
  item->setCheckState(0, Qt::Unchecked);
  return item;
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

void set_down(QTreeWidgetItem* item, Qt::CheckState state) {
  for (int i = 0; i < item->childCount(); ++i) {
    item->child(i)->setCheckState(0, state);
    set_down(item->child(i), state);
  }
}

// A parent reads from its children: all ticked, none, or some.
void set_up(QTreeWidgetItem* item) {
  for (QTreeWidgetItem* p = item->parent(); p != nullptr; p = p->parent()) {
    int ticked = 0, none = 0;
    for (int i = 0; i < p->childCount(); ++i) {
      const Qt::CheckState s = p->child(i)->checkState(0);
      if (s == Qt::Checked) ++ticked;
      else if (s == Qt::Unchecked) ++none;
    }
    const int n = p->childCount();
    if (p->data(0, stand_in_role).toBool()) {
      p->setCheckState(0, none == n ? Qt::Unchecked : Qt::PartiallyChecked);
      continue;
    }
    p->setCheckState(0, ticked == n ? Qt::Checked : (none == n ? Qt::Unchecked : Qt::PartiallyChecked));
  }
}

// What an old node was, for one no longer offered.
struct Old {
  Kind kind;
  QString key, label, detail, parent;  // parent: id_of of its parent; empty at top level
  bool ticked;
};

}  // namespace

IrradiationTree::IrradiationTree(const QString& title, QWidget* parent)
    : QWidget(parent),
      title_(title),
      toggle_(new QToolButton),
      body_(new QWidget),
      filter_(new QLineEdit),
      clear_(new QToolButton),
      tree_(new QTreeWidget) {
  toggle_->setText(title);
  toggle_->setCheckable(true);
  toggle_->setChecked(false);  // folded until asked for
  toggle_->setArrowType(Qt::RightArrow);
  toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggle_->setAutoRaise(true);
  toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  toggle_->setToolTip(tr("Show or hide this tree"));
  QFont bold = toggle_->font();
  bold.setBold(true);
  toggle_->setFont(bold);
  filter_->setPlaceholderText(tr("Filter"));
  filter_->setClearButtonEnabled(true);
  clear_->setText(QStringLiteral("×"));
  clear_->setToolTip(tr("Untick every node of this tree"));
  clear_->setEnabled(false);
  tree_->setColumnCount(2);
  tree_->setHeaderHidden(true);
  tree_->header()->setStretchLastSection(true);
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  style::make_tick_list(tree_);  // room above and below each tick box
  tree_->setMaximumHeight(220);

  auto* header = new QHBoxLayout;
  header->addWidget(filter_, 1);
  header->addWidget(clear_);
  auto* body = new QVBoxLayout(body_);
  body->setContentsMargins(0, 0, 0, 0);
  body->addLayout(header);
  body->addWidget(tree_);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(2);
  layout->addWidget(toggle_);
  layout->addWidget(body_);
  body_->setVisible(false);

  connect(toggle_, &QToolButton::toggled, this, &IrradiationTree::set_expanded);
  connect(filter_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
  connect(clear_, &QToolButton::clicked, this, &IrradiationTree::clear_checked);
  connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
    if (!updating_ && column == 0) on_item_changed(item);
  });
}

void IrradiationTree::on_item_changed(QTreeWidgetItem* item) {
  updating_ = true;  // the ticks set below come back here
  if (item->checkState(0) != Qt::PartiallyChecked) set_down(item, item->checkState(0));
  set_up(item);
  updating_ = false;
  apply_filter();  // a node just ticked must not be hidden, one just unticked may be
  update_header();
  emit changed();
}

void IrradiationTree::set_rows(const std::vector<processing::PositionRow>& rows) {
  // What the tree was: every node, for those no longer offered; and which were open.
  std::vector<Old> old;
  std::set<QString> open;
  for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
    const QTreeWidgetItem* item = *it;
    old.push_back({kind_of(item), key_of(item), item->text(0), item->text(1),
                   item->parent() != nullptr ? id_of(item->parent()) : QString{}, item->checkState(0) == Qt::Checked});
    if (item->isExpanded()) open.insert(id_of(item));
  }

  updating_ = true;
  tree_->clear();
  std::map<QString, QTreeWidgetItem*> by_id;
  QTreeWidgetItem* not_irradiated = nullptr;
  const auto add = [&](QTreeWidgetItem* item, QTreeWidgetItem* parent) {
    if (parent != nullptr) {
      parent->addChild(item);
    } else if (not_irradiated != nullptr) {
      tree_->insertTopLevelItem(tree_->indexOfTopLevelItem(not_irradiated), item);  // "Not irradiated" stays last
    } else {
      tree_->addTopLevelItem(item);
    }
    by_id[id_of(item)] = item;
    if (kind_of(item) == Kind::NotIrradiated) not_irradiated = item;
  };

  std::vector<processing::PositionRow> sorted = rows;
  std::ranges::stable_sort(sorted, [](const auto& a, const auto& b) {
    // Irradiations and levels by name, then identifiers; keys group them.
    return std::tie(a.irradiation, a.irradiation_key, a.level, a.level_key, a.identifier) <
           std::tie(b.irradiation, b.irradiation_key, b.level, b.level_key, b.identifier);
  });
  for (const auto& row : sorted) {
    QTreeWidgetItem* parent = nullptr;
    if (!row.irradiation_key.empty()) {
      const QString ikey = qs(row.irradiation_key);
      QTreeWidgetItem*& irradiation = by_id[QStringLiteral("0:") + ikey];
      if (irradiation == nullptr) {
        irradiation = make_item(Kind::Irradiation, ikey, qs(row.irradiation));
        add(irradiation, nullptr);
      }
      const QString lkey = qs(row.level_key);
      QTreeWidgetItem*& level = by_id[QStringLiteral("1:") + lkey];
      if (level == nullptr) {
        level = make_item(Kind::Level, lkey, qs(row.level));
        add(level, irradiation);
      }
      parent = level;
    }
    auto* identifier = make_item(Kind::Identifier, qs(row.identifier), qs(row.identifier));
    const QString sample = qs(row.sample), project = qs(row.project);
    identifier->setText(1, sample.isEmpty() ? project : (project.isEmpty() ? sample : sample + QStringLiteral(" · ") + project));
    if (parent != nullptr) {
      add(identifier, parent);
    } else {
      if (not_irradiated == nullptr) add(make_item(Kind::NotIrradiated, QString{}, tr("Not irradiated")), nullptr);
      add(identifier, not_irradiated);
    }
  }

  // Ticked nodes keep their place; one no longer offered is made again, with
  // its parents where they are gone too, at the end of its parent.
  const std::function<QTreeWidgetItem*(const Old&)> ensure = [&](const Old& node) -> QTreeWidgetItem* {
    const QString id = QString::number(static_cast<int>(node.kind)) + QLatin1Char(':') + node.key;
    if (const auto found = by_id.find(id); found != by_id.end() && found->second != nullptr) return found->second;
    QTreeWidgetItem* parent = nullptr;
    if (!node.parent.isEmpty()) {
      const auto it = std::ranges::find_if(old, [&](const Old& o) {
        return QString::number(static_cast<int>(o.kind)) + QLatin1Char(':') + o.key == node.parent;
      });
      if (it != old.end()) parent = ensure(*it);
    }
    auto* item = make_item(node.kind, node.key, node.label);
    item->setText(1, node.detail);
    item->setData(0, stand_in_role, !node.ticked);
    if (node.kind == Kind::NotIrradiated) {
      tree_->addTopLevelItem(item);
      by_id[id] = item;
      not_irradiated = item;
    } else {
      add(item, parent);
    }
    return item;
  };
  for (const Old& node : old) {
    if (!node.ticked) continue;
    QTreeWidgetItem* item = ensure(node);
    item->setCheckState(0, Qt::Checked);
    set_down(item, Qt::Checked);
    set_up(item);
  }
  for (const auto& [id, item] : by_id)
    if (item != nullptr && open.contains(id)) item->setExpanded(true);
  tree_->resizeColumnToContents(0);
  updating_ = false;
  apply_filter();
  update_header();
}

std::optional<processing::PositionSelection> IrradiationTree::selection() const {
  processing::PositionSelection out;
  const auto identifiers_under = [&](const QTreeWidgetItem* node) {
    for (int i = 0; i < node->childCount(); ++i)
      if (node->child(i)->checkState(0) == Qt::Checked) out.identifiers.push_back(key_of(node->child(i)).toStdString());
  };
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    const QTreeWidgetItem* top = tree_->topLevelItem(i);
    const Qt::CheckState state = top->checkState(0);
    if (state == Qt::Unchecked) continue;
    if (kind_of(top) == Kind::NotIrradiated) {
      if (state == Qt::Checked) out.not_irradiated = true;
      else identifiers_under(top);
    } else if (state == Qt::Checked) {
      out.irradiations.push_back(key_of(top).toStdString());
    } else {
      for (int j = 0; j < top->childCount(); ++j) {
        const QTreeWidgetItem* level = top->child(j);
        if (level->checkState(0) == Qt::Checked) out.levels.push_back(key_of(level).toStdString());
        else if (level->checkState(0) == Qt::PartiallyChecked) identifiers_under(level);
      }
    }
  }
  if (out == processing::PositionSelection{}) return std::nullopt;
  return out;
}

void IrradiationTree::clear_checked() {
  if (!selection().has_value()) return;
  updating_ = true;
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    tree_->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    set_down(tree_->topLevelItem(i), Qt::Unchecked);
  }
  updating_ = false;
  apply_filter();
  update_header();
  emit changed();
}

void IrradiationTree::set_expanded(bool expanded) {
  if (toggle_->isChecked() != expanded) toggle_->setChecked(expanded);  // toggled() comes back here
  toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  body_->setVisible(expanded);
}

bool IrradiationTree::is_expanded() const noexcept { return toggle_->isChecked(); }

QString IrradiationTree::title() const { return toggle_->text(); }

void IrradiationTree::apply_filter() {
  const QString text = filter_->text().trimmed();
  // Returns whether the node shows. A match shows what is under it; a node
  // with a match under it shows, opened; a ticked or partly ticked one shows.
  const std::function<bool(QTreeWidgetItem*, bool)> visit = [&](QTreeWidgetItem* item, bool above_matches) {
    const bool own = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive) ||
                     item->text(1).contains(text, Qt::CaseInsensitive);
    bool below = false;
    for (int i = 0; i < item->childCount(); ++i) below = visit(item->child(i), above_matches || own) || below;
    const bool ticked = item->checkState(0) != Qt::Unchecked;
    item->setHidden(!(own || above_matches || below || ticked));
    if (below && !text.isEmpty()) item->setExpanded(true);
    return own || below || ticked;
  };
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i), false);
}

void IrradiationTree::update_header() {
  int ticked = 0;
  if (const auto sel = selection())
    ticked = static_cast<int>(sel->irradiations.size() + sel->levels.size() + sel->identifiers.size()) + (sel->not_irradiated ? 1 : 0);
  clear_->setEnabled(ticked > 0);
  toggle_->setText(ticked > 0 ? tr("%1 (%2)").arg(title_).arg(ticked) : title_);
}

}  // namespace pychron::ui
