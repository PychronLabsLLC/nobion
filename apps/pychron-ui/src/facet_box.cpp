#include "facet_box.hpp"

#include <algorithm>

#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace pychron::ui {

FacetBox::FacetBox(const QString& title, QWidget* parent)
    : QGroupBox(title, parent), title_(title), filter_(new QLineEdit), clear_(new QToolButton), list_(new QListWidget) {
  filter_->setPlaceholderText(tr("Filter"));
  filter_->setClearButtonEnabled(true);
  clear_->setText(QStringLiteral("×"));
  clear_->setToolTip(tr("Untick every value of this list"));
  clear_->setEnabled(false);
  list_->setMaximumHeight(130);

  auto* header = new QHBoxLayout;
  header->addWidget(filter_, 1);
  header->addWidget(clear_);
  auto* layout = new QVBoxLayout(this);
  layout->addLayout(header);
  layout->addWidget(list_);

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
  setTitle(ticked > 0 ? tr("%1 (%2)").arg(title_).arg(ticked) : title_);
}

}  // namespace pychron::ui
