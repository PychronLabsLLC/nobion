#include "alarm_dock.hpp"
#include "theme.hpp"

#include <algorithm>
#include <utility>

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

enum Column { kSource, kSeverity, kMessage, kTime };
constexpr int kValueRole = Qt::UserRole;      // the severity, and the time, as values
constexpr int kRaisedRole = Qt::UserRole + 1;  // on the source column: when raised, in raise order

QString severity_name(AlarmSeverity severity) {
  switch (severity) {
    case AlarmSeverity::Info:
      return QStringLiteral("info");
    case AlarmSeverity::Warning:
      return QStringLiteral("warning");
    case AlarmSeverity::Critical:
      return QStringLiteral("critical");
  }
  return QStringLiteral("?");
}

}  // namespace

AlarmDock::AlarmDock(QWidget* parent, std::function<QDateTime()> now)
    : QDockWidget(tr("Alarms"), parent),
      now_(std::move(now)),
      tree_(new QTreeWidget),
      ack_(new QPushButton(tr("Acknowledge"))),
      ack_all_(new QPushButton(tr("Acknowledge all"))) {
  if (!now_) {
    now_ = [] { return QDateTime::currentDateTime(); };
  }
  setObjectName(QStringLiteral("AlarmDock"));
  tree_->setHeaderLabels({tr("Source"), tr("Severity"), tr("Message"), tr("Time")});
  tree_->setRootIsDecorated(false);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  buttons->addWidget(ack_);
  buttons->addWidget(ack_all_);

  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(tree_);
  layout->addLayout(buttons);
  setWidget(body);

  connect(ack_, &QPushButton::clicked, this, [this] { acknowledge_selected(); });
  connect(ack_all_, &QPushButton::clicked, this, &AlarmDock::acknowledge_all);
}

void AlarmDock::add_alarm(const Alarm& alarm) {
  const QString source = QString::fromStdString(alarm.source);
  QTreeWidgetItem* row = row_for(source);
  const QString message = QString::fromStdString(alarm.message);
  const QDateTime when = now_();
  // A repeat of what the row already says only moves its time: what stands
  // is the same, and nothing that counts it needs to hear.
  const bool same = row != nullptr && row->data(kSeverity, kValueRole).toInt() == static_cast<int>(alarm.severity) &&
                    row->text(kMessage) == message;
  if (!row) {
    row = new QTreeWidgetItem(tree_);
    row->setText(kSource, source);
  }
  row->setText(kTime, when.time().toString(QStringLiteral("HH:mm:ss")));
  if (same) return;
  row->setText(kSeverity, severity_name(alarm.severity));
  row->setText(kMessage, message);
  row->setData(kSeverity, kValueRole, static_cast<int>(alarm.severity));
  row->setData(kTime, kValueRole, when);
  row->setData(kSource, kRaisedRole, QVariant::fromValue(++raised_));
  const QColor color = alarm.severity == AlarmSeverity::Critical ? theme().error_text : theme().warning_text;
  row->setForeground(kSeverity, color);
  emit changed();
}

std::vector<AlarmRow> AlarmDock::rows() const {
  struct Ranked {
    AlarmRow row;
    quint64 raised;
  };
  std::vector<Ranked> ranked;
  ranked.reserve(static_cast<std::size_t>(tree_->topLevelItemCount()));
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    const QTreeWidgetItem* row = tree_->topLevelItem(i);
    ranked.push_back({{row->text(kSource).toStdString(),
                       static_cast<AlarmSeverity>(row->data(kSeverity, kValueRole).toInt()),
                       row->text(kMessage).toStdString(), row->data(kTime, kValueRole).toDateTime()},
                      row->data(kSource, kRaisedRole).toULongLong()});
  }
  // Critical is the highest severity value; newest by raise order, which
  // two alarms in the same second (or the same simulated instant) still have.
  std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    if (a.row.severity != b.row.severity) return a.row.severity > b.row.severity;
    return a.raised > b.raised;
  });
  std::vector<AlarmRow> out;
  out.reserve(ranked.size());
  for (auto& r : ranked) out.push_back(std::move(r.row));
  return out;
}

int AlarmDock::acknowledge_selected() {
  const auto selected = tree_->selectedItems();
  for (QTreeWidgetItem* row : selected) {
    delete row;
  }
  if (!selected.isEmpty()) emit changed();
  return static_cast<int>(selected.size());
}

void AlarmDock::acknowledge_all() {
  tree_->clear();
  emit changed();
}

int AlarmDock::active_count() const { return tree_->topLevelItemCount(); }

bool AlarmDock::is_active(const std::string& source) const {
  return row_for(QString::fromStdString(source)) != nullptr;
}

QTreeWidgetItem* AlarmDock::row_for(const QString& source) const {
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    if (tree_->topLevelItem(i)->text(kSource) == source) {
      return tree_->topLevelItem(i);
    }
  }
  return nullptr;
}

}  // namespace pychron::ui
