#include "valve_popup.hpp"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include "gauge_popup.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

QString kind_name(canvas::ValveKind kind) {
  switch (kind) {
    case canvas::ValveKind::Manual:
      return ValvePopup::tr("manual valve");
    case canvas::ValveKind::Rough:
      return ValvePopup::tr("roughing valve");
    case canvas::ValveKind::Switch:
      return ValvePopup::tr("switch");
    case canvas::ValveKind::Valve:
      break;
  }
  return ValvePopup::tr("valve");
}

QLabel* line_label(QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  return label;
}

}  // namespace

ValvePopup::ValvePopup(QWidget* parent)
    : QFrame(parent, Qt::Popup),
      title_(new QLabel(this)),
      description_(line_label(this)),
      state_(line_label(this)),
      interlocks_(line_label(this)),
      history_(line_label(this)),
      failure_(line_label(this)),
      note_(line_label(this)),
      open_(new QPushButton(tr("Open"), this)),
      close_(new QPushButton(tr("Close"), this)),
      lock_(new QPushButton(this)) {
  setObjectName(QStringLiteral("valvePopup"));
  // The click that closes the faceplate is not replayed on the canvas: on the
  // valve itself it would open the faceplate again.
  setAttribute(Qt::WA_NoMouseReplay);
  setFrameShape(QFrame::StyledPanel);
  setMinimumWidth(240);
  setMaximumWidth(340);
  QFont bold = title_->font();
  bold.setBold(true);
  title_->setFont(bold);
  title_->setTextFormat(Qt::PlainText);
  QPalette muted = history_->palette();
  muted.setColor(QPalette::WindowText, theme().muted_text);
  description_->setPalette(muted);
  history_->setPalette(muted);
  QPalette error = failure_->palette();
  error.setColor(QPalette::WindowText, theme().error_text);
  failure_->setPalette(error);
  QPalette warning = note_->palette();
  warning.setColor(QPalette::WindowText, theme().warning_text);
  note_->setPalette(warning);

  auto* buttons = new QHBoxLayout;
  buttons->addWidget(open_);
  buttons->addWidget(close_);
  buttons->addStretch(1);
  buttons->addWidget(lock_);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 8, 10, 10);
  layout->setSpacing(4);
  for (QWidget* w : {static_cast<QWidget*>(title_), static_cast<QWidget*>(description_), static_cast<QWidget*>(state_),
                     static_cast<QWidget*>(interlocks_), static_cast<QWidget*>(history_),
                     static_cast<QWidget*>(failure_), static_cast<QWidget*>(note_)}) {
    layout->addWidget(w);
  }
  layout->addLayout(buttons);

  connect(open_, &QPushButton::clicked, this, [this] {
    if (on_operate_) on_operate_(faceplate_.name, systems::SwitchOp::Open);
  });
  connect(close_, &QPushButton::clicked, this, [this] {
    if (on_operate_) on_operate_(faceplate_.name, systems::SwitchOp::Close);
  });
  connect(lock_, &QPushButton::clicked, this, [this] {
    if (on_lock_) on_lock_(faceplate_.name, !faceplate_.locked);
  });
}

void ValvePopup::set_faceplate(const ValveFaceplate& faceplate) {
  faceplate_ = faceplate;
  const ValveFaceplate& f = faceplate_;
  title_->setText(QStringLiteral("%1  ·  %2").arg(f.title, kind_name(f.kind)));
  description_->setText(f.description);
  description_->setVisible(!f.description.isEmpty());
  state_->setText(f.state_line);

  QStringList rows;
  for (const auto& i : f.interlocks) {
    const QString want = i.must_be_closed ? tr("%1 closed").arg(i.partner) : tr("%1 open").arg(i.partner);
    rows += tr("Opens only with %1: %2").arg(want, i.met ? tr("met") : tr("not met"));
  }
  interlocks_->setText(rows.join(QLatin1Char('\n')));
  interlocks_->setVisible(!rows.isEmpty());
  history_->setText(f.history.join(QLatin1Char('\n')));
  history_->setVisible(!f.history.isEmpty());
  failure_->setText(f.last_failure.isEmpty() ? QString() : tr("Last failure: %1").arg(f.last_failure));
  failure_->setVisible(!f.last_failure.isEmpty());

  QString note;
  if (f.operable && f.pending) {
    note = tr("Moving…");
  } else if (f.operable && f.locked) {
    note = tr("Locked: unlock to operate");
  }
  note_->setText(note);
  note_->setVisible(!note.isEmpty());

  open_->setVisible(f.operable && f.state != ValveState::Open);
  close_->setVisible(f.operable && f.state != ValveState::Closed);
  const bool can_operate = !f.pending && !f.locked;
  open_->setEnabled(can_operate);
  close_->setEnabled(can_operate);
  lock_->setVisible(f.operable);
  lock_->setText(f.locked ? tr("Unlock") : tr("Lock"));
  if (isVisible()) adjustSize();
}

void ValvePopup::show_beside(const QRect& global_valve) {
  adjustSize();
  QScreen* screen = QGuiApplication::screenAt(global_valve.center());
  if (screen == nullptr) screen = QGuiApplication::primaryScreen();
  const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(QPoint(0, 0), size() * 4);
  setGeometry(GaugePopup::placed(global_valve, size(), area));
  show();
  raise();
}

QStringList ValvePopup::text() const {
  QStringList lines;
  for (const QLabel* l : {title_, description_, state_, interlocks_, history_, failure_, note_}) {
    if (!l->isHidden() && !l->text().isEmpty()) lines += l->text().split(QLatin1Char('\n'));
  }
  return lines;
}

void ValvePopup::hideEvent(QHideEvent* event) {
  QFrame::hideEvent(event);
  emit closed();
}

void ValvePopup::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    hide();
    event->accept();
    return;
  }
  QFrame::keyPressEvent(event);
}

}  // namespace pychron::ui
