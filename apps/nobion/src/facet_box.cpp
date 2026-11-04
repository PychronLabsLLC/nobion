#include "facet_box.hpp"

#include <algorithm>

#include <QFont>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace pychron::ui {

FacetBox::FacetBox(const QString& title, QWidget* parent)
    : QWidget(parent),
      title_(title),
      toggle_(new QToolButton),
      body_(new QWidget),
      filter_(new QLineEdit),
      clear_(new QToolButton),
      list_(new QListWidget) {
  toggle_->setText(title);
  toggle_->setCheckable(true);
  toggle_->setChecked(false);  // folded until asked for
  toggle_->setArrowType(Qt::RightArrow);
  toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggle_->setAutoRaise(true);
  toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  toggle_->setToolTip(tr("Show or hide this list"));
  QFont bold = toggle_->font();
  bold.setBold(true);
  toggle_->setFont(bold);
  filter_->setPlaceholderText(tr("Filter"));
  filter_->setClearButtonEnabled(true);
  clear_->setText(QStringLiteral("×"));
  clear_->setToolTip(tr("Untick every value of this list"));
  clear_->setEnabled(false);
  list_->setSpacing(3);  // room above and below each tick box
  list_->setMaximumHeight(160);

  auto* header = new QHBoxLayout;
  header->addWidget(filter_, 1);
  header->addWidget(clear_);
  auto* body = new QVBoxLayout(body_);
  body->setContentsMargins(0, 0, 0, 0);
  body->addLayout(header);
  body->addWidget(list_);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(2);
  layout->addWidget(toggle_);
  layout->addWidget(body_);
  body_->setVisible(false);

  connect(toggle_, &QToolButton::toggled, this, &FacetBox::set_expanded);

  connect(filter_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
  connect(clear_, &QToolButton::clicked, this, &FacetBox::clear_checked);
  connect(list_, &QListWidget::itemChanged, this, [this] {
    if (updating_) return;
    apply_filter();  // a value just ticked must not be hidden, one just unticked may be
    update_header();
    emit changed();
  });
}

void FacetBox::set_items(const std::vector<Item>& items) {
  const QStringList ticked = checked();
  std::vector<Item> all = items;
  for (int i = 0; i < list_->count(); ++i) {
    const QListWidgetItem* line = list_->item(i);
    const QString value = line->data(Qt::UserRole).toString();
    if (line->checkState() != Qt::Checked) continue;
    if (std::ranges::none_of(items, [&](const Item& item) { return item.value == value; }))
      all.push_back({value, line->text()});
  }
  updating_ = true;
  list_->clear();
  for (const auto& [value, label] : all) {
    auto* line = new QListWidgetItem(label, list_);
    line->setData(Qt::UserRole, value);
    line->setFlags(line->flags() | Qt::ItemIsUserCheckable);
    line->setCheckState(ticked.contains(value) ? Qt::Checked : Qt::Unchecked);
  }
  updating_ = false;
  apply_filter();
  update_header();
}

void FacetBox::set_values(const QStringList& values) {
  std::vector<Item> items;
  items.reserve(static_cast<std::size_t>(values.size()));
  for (const auto& value : values) items.push_back({value, value});
  set_items(items);
}

QStringList FacetBox::checked() const {
  QStringList out;
  for (int i = 0; i < list_->count(); ++i)
    if (list_->item(i)->checkState() == Qt::Checked) out << list_->item(i)->data(Qt::UserRole).toString();
  return out;
}

void FacetBox::clear_checked() {
  if (checked().isEmpty()) return;
  updating_ = true;
  for (int i = 0; i < list_->count(); ++i) list_->item(i)->setCheckState(Qt::Unchecked);
  updating_ = false;
  apply_filter();
  update_header();
  emit changed();
}

void FacetBox::set_expanded(bool expanded) {
  if (toggle_->isChecked() != expanded) toggle_->setChecked(expanded);  // toggled() comes back here
  toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  body_->setVisible(expanded);
}

bool FacetBox::is_expanded() const noexcept { return toggle_->isChecked(); }

QString FacetBox::title() const { return toggle_->text(); }

void FacetBox::apply_filter() {
  const QString text = filter_->text().trimmed();
  for (int i = 0; i < list_->count(); ++i) {
    QListWidgetItem* item = list_->item(i);
    item->setHidden(item->checkState() != Qt::Checked && !item->text().contains(text, Qt::CaseInsensitive));
  }
}

void FacetBox::update_header() {
  const auto ticked = checked().size();
  clear_->setEnabled(ticked > 0);
  toggle_->setText(ticked > 0 ? tr("%1 (%2)").arg(title_).arg(ticked) : title_);
}

}  // namespace pychron::ui
