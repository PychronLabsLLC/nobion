#include "status_banner.hpp"

#include <algorithm>
#include <utility>

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include "theme.hpp"

namespace pychron::ui {

namespace {

namespace exec = experiment::executor;

// A pill on the band: a field whose state is not the normal one. The weight
// is in the sheet, not a setFont: the application's sheet sets the font, and
// a size measured with one font and drawn with another overlaps.
QString pill(const QColor& background, const QColor& text, bool bold = false, const QColor& border = {}) {
  return QStringLiteral("background: %1; color: %2; border-radius: 9px; padding: 1px %3px; font-weight: %4; border: %5;")
      .arg(background.name(), text.name(), QString::number(StatusBanner::kClockPadding),
           bold ? QStringLiteral("600") : QStringLiteral("400"),
           border.isValid() ? QStringLiteral("1px solid %1").arg(border.name()) : QStringLiteral("none"));
}

// A field in its normal state: plain text on the band, nothing coloured
// (ISA-101: colour is for what needs attention).
QString plain(bool bold = false) {
  return QStringLiteral("background: transparent; color: %1; padding: 1px %2px; font-weight: %3;")
      .arg(theme().text.name(), QString::number(StatusBanner::kClockPadding),
           bold ? QStringLiteral("600") : QStringLiteral("400"));
}

QLabel* field(const char* name, QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setObjectName(QLatin1String(name));
  label->setTextFormat(Qt::PlainText);
  label->setStyleSheet(plain());
  return label;
}

QString severity_word(AlarmSeverity severity) {
  return severity == AlarmSeverity::Critical ? AlarmStrip::tr("CRITICAL") : AlarmStrip::tr("WARNING");
}

}  // namespace

StatusBanner::StatusBanner(std::function<QDateTime()> clock, QWidget* parent)
    : QFrame(parent),
      clock_(std::move(clock)),
      mode_(field("bannerMode", this)),
      line_(field("bannerLine", this)),
      executor_(field("bannerExecutor", this)),
      alarms_(new QToolButton(this)),
      health_(field("bannerHealth", this)),
      clock_label_(field("bannerClock", this)) {
  setObjectName(QStringLiteral("statusBanner"));
  setFixedHeight(kHeight);
  // A light grey band (ISA-101: a muted background, so that colour on it is
  // only ever an alarm or an abnormal state), ruled off from the canvas.
  setStyleSheet(QStringLiteral("QFrame#statusBanner { background: %1; border-bottom: 1px solid %2; }")
                    .arg(theme().header_bg.name(), theme().strong_border.name()));

  // The band never sets how narrow the window may be: the executor's text
  // takes the spare width and gives way first, then the line's name (each
  // whole in its tooltip). The executor, not an empty stretch, takes the
  // room: a stretch would leave it its minimum and squeeze it under its
  // neighbour. The name keeps its width while there is room (its minimum is
  // set, not its text's).
  executor_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  executor_->setMinimumWidth(kMinField);
  line_->setMinimumWidth(kMinField);
  line_->setStyleSheet(plain(true));
  alarms_->setObjectName(QStringLiteral("bannerAlarms"));
  alarms_->setAutoRaise(true);
  alarms_->setCursor(Qt::PointingHandCursor);
  alarms_->setToolTip(tr("Show the Alarms panel"));
  alarms_->setStyleSheet(QStringLiteral("QToolButton { %1 border: none; }").arg(plain()));
  connect(alarms_, &QToolButton::clicked, this, &StatusBanner::alarms_clicked);

  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(6, 2, 6, 2);
  row->setSpacing(4);
  row->addWidget(mode_);
  row->addWidget(line_);
  row->addSpacing(12);
  row->addWidget(executor_, 1);
  row->addWidget(alarms_);
  row->addWidget(health_);
  row->addWidget(clock_label_);

  set_mode({});
  set_session(false);
  set_alarms({});
  set_health({});
  fit_clock();
  clock_label_->installEventFilter(this);
  ticker_.setInterval(1000);
  connect(&ticker_, &QTimer::timeout, this, &StatusBanner::tick);
  ticker_.start();
  tick();
}

void StatusBanner::set_mode(LineMode mode) {
  clock_label_->setToolTip(mode.speed > 0 ? tr("The line's clock: simulated, %1 times real time").arg(mode.speed)
                                          : tr("The line's clock: real time"));
  if (!mode.simulated) {
    mode_->setText(tr("LIVE"));
    mode_->setStyleSheet(pill(theme().base, theme().muted_text, true, theme().strong_border));
    return;
  }
  mode_->setText(mode.speed > 0 ? tr("SIMULATED x%1").arg(mode.speed) : tr("SIMULATED"));
  mode_->setStyleSheet(pill(theme().accent_soft, theme().accent_strong, true));
}

void StatusBanner::set_line(const QString& name) {
  line_->setText(name);
  line_->setToolTip(name);
}

void StatusBanner::set_session(bool has_session, bool running) {
  session_ = has_session;
  state_.reset();
  run_.clear();
  hold_reason_.clear();
  if (has_session) state_ = running ? exec::ExecutorState::Running : exec::ExecutorState::Idle;
  render_executor();
}

void StatusBanner::on_state(const exec::ExecutorStateChanged& e) {
  state_ = e.to;
  if (e.to != exec::ExecutorState::Holding) hold_reason_.clear();
  // A new queue's runs are named as they start.
  if (e.to == exec::ExecutorState::Idle || e.to == exec::ExecutorState::Preparing) run_.clear();
  render_executor();
}

void StatusBanner::on_run_started(const exec::RunStarted& e) {
  run_ = QString::fromStdString(e.name.empty() ? e.identifier : e.name);
  render_executor();
}

void StatusBanner::on_holding(const exec::ExecutorHolding& e) {
  // The executor says Holding, then why; either order shows the reason.
  state_ = exec::ExecutorState::Holding;
  hold_reason_ = QString::fromStdString(e.reason);
  render_executor();
}

QString StatusBanner::executor_text(std::optional<exec::ExecutorState> state, const QString& run,
                                    const QString& hold_reason) {
  if (!state) return tr("No experiment session");
  switch (*state) {
    case exec::ExecutorState::Idle: return tr("Idle");
    case exec::ExecutorState::Preparing: return tr("Preparing queue");
    case exec::ExecutorState::Running: return run.isEmpty() ? tr("Running") : tr("Running %1").arg(run);
    case exec::ExecutorState::Holding: return hold_reason.isEmpty() ? tr("Holding") : tr("Holding: %1").arg(hold_reason);
    case exec::ExecutorState::StoppingAtBoundary:
      return run.isEmpty() ? tr("Stopping") : tr("Stopping after %1").arg(run);
    case exec::ExecutorState::Cancelling: return tr("Cancelling");
    case exec::ExecutorState::Aborting: return tr("Aborting");
    case exec::ExecutorState::Finalizing: return tr("Finishing queue");
  }
  return {};
}

void StatusBanner::render_executor() {
  executor_->setText(executor_text(session_ ? state_ : std::nullopt, run_, hold_reason_));
  executor_->setToolTip(executor_->text());
  const bool attention = session_ && state_ &&
                         (*state_ == exec::ExecutorState::Holding || *state_ == exec::ExecutorState::Aborting);
  executor_->setStyleSheet(attention ? pill(theme().warning, theme().text, true) : plain());
}

void StatusBanner::set_alarms(const std::vector<AlarmRow>& rows) {
  const auto critical = std::count_if(rows.begin(), rows.end(),
                                      [](const AlarmRow& r) { return r.severity == AlarmSeverity::Critical; });
  const auto warning = std::count_if(rows.begin(), rows.end(),
                                     [](const AlarmRow& r) { return r.severity == AlarmSeverity::Warning; });
  QStringList parts;
  if (critical > 0) parts += tr("%1 critical").arg(critical);
  if (warning > 0) parts += tr("%1 warning").arg(warning);
  alarms_->setText(parts.isEmpty() ? tr("No alarms") : parts.join(QStringLiteral(" · ")));
  QString look = plain();
  if (critical > 0) {
    look = pill(theme().error, theme().base, true);
  } else if (warning > 0) {
    look = pill(theme().warning, theme().text, true);
  }
  alarms_->setStyleSheet(QStringLiteral("QToolButton { %1 border: none; }").arg(look));
}

void StatusBanner::set_health(const HealthBar::Summary& s) {
  const int total = s.ok + s.degraded + s.down + s.unknown;
  if (s.degraded == 0 && s.down == 0 && s.unknown == 0) {
    health_->setText(tr("Connections ok (%1)").arg(total));
    health_->setStyleSheet(plain());
    return;
  }
  QStringList parts;
  if (s.down > 0) parts += tr("%1 down").arg(s.down);
  if (s.degraded > 0) parts += tr("%1 degraded").arg(s.degraded);
  if (s.unknown > 0) parts += tr("%1 unknown").arg(s.unknown);
  health_->setText(parts.join(QStringLiteral(" · ")));
  if (s.down > 0) {
    health_->setStyleSheet(pill(theme().error, theme().base, true));
  } else if (s.degraded > 0) {
    health_->setStyleSheet(pill(theme().warning, theme().text, true));
  } else {
    health_->setStyleSheet(plain());  // not heard from yet: nothing wrong known
  }
}

void StatusBanner::fit_clock() {
  // As wide as the widest time, so the fields beside it stay put as the
  // seconds count: digits differ in width in most fonts.
  clock_label_->ensurePolished();
  const QFontMetrics metrics(clock_label_->font());
  int digit = 0;
  for (char d = '0'; d <= '9'; ++d) digit = std::max(digit, metrics.horizontalAdvance(QLatin1Char(d)));
  const int text = 6 * digit + 2 * metrics.horizontalAdvance(QLatin1Char(':'));
  const QMargins margins = clock_label_->contentsMargins();
  // + 2: a pixel each side for antialiased glyph edges.
  clock_label_->setFixedWidth(text + margins.left() + margins.right() + 2 * kClockPadding + 2);
  clock_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

// The clock's own font: a change to it (an application font or sheet) does
// not reach the band's changeEvent.
bool StatusBanner::eventFilter(QObject* watched, QEvent* event) {
  if (watched == clock_label_ && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
    fit_clock();
  }
  return QFrame::eventFilter(watched, event);
}

void StatusBanner::showEvent(QShowEvent* event) {
  QFrame::showEvent(event);
  fit_clock();
}

void StatusBanner::tick() { clock_label_->setText(clock_().time().toString(QStringLiteral("HH:mm:ss"))); }

QString StatusBanner::mode_text() const { return mode_->text(); }
QString StatusBanner::line_text() const { return line_->text(); }
QString StatusBanner::executor_text() const { return executor_->text(); }
QString StatusBanner::alarms_text() const { return alarms_->text(); }
QString StatusBanner::health_text() const { return health_->text(); }
QString StatusBanner::clock_text() const { return clock_label_->text(); }

// ---- AlarmStrip -------------------------------------------------------------

AlarmStrip::AlarmStrip(QWidget* parent) : QFrame(parent), layout_(new QVBoxLayout(this)) {
  setObjectName(QStringLiteral("alarmStrip"));
  setCursor(Qt::PointingHandCursor);
  setToolTip(tr("Show the Alarms panel, where alarms are acknowledged"));
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(0);
  hide();
}

void AlarmStrip::set_alarms(const std::vector<AlarmRow>& rows) {
  while (QLayoutItem* item = layout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  lines_.clear();
  std::vector<const AlarmRow*> shown;
  for (const AlarmRow& row : rows) {
    if (row.severity != AlarmSeverity::Info) shown.push_back(&row);
  }
  setVisible(!shown.empty());
  const auto more = static_cast<int>(shown.size()) - kShown;
  if (more > 0) shown.resize(kShown);
  for (const AlarmRow* row : shown) {
    const bool critical = row->severity == AlarmSeverity::Critical;
    auto* line = new QWidget(this);
    line->setAutoFillBackground(true);
    QPalette look = line->palette();
    look.setColor(QPalette::Window, critical ? theme().error_bg : theme().warning_bg);
    look.setColor(QPalette::WindowText, critical ? theme().on_error_bg : theme().warning_text);
    line->setPalette(look);
    auto* cells = new QHBoxLayout(line);
    cells->setContentsMargins(10, 2, 10, 2);
    cells->setSpacing(12);
    const QString source = QString::fromStdString(row->source);
    const QString message = QString::fromStdString(row->message);
    const QString time = row->time.time().toString(QStringLiteral("HH:mm:ss"));
    auto* severity = new QLabel(severity_word(row->severity), line);
    QFont bold = severity->font();
    bold.setBold(true);
    severity->setFont(bold);
    severity->setMinimumWidth(severity->fontMetrics().horizontalAdvance(tr("CRITICAL")));
    auto* who = new QLabel(source, line);
    who->setFont(bold);
    auto* what = new QLabel(message, line);
    what->setTextFormat(Qt::PlainText);
    auto* when = new QLabel(time, line);
    cells->addWidget(severity);
    cells->addWidget(who);
    cells->addWidget(what, 1);
    cells->addWidget(when);
    layout_->addWidget(line);
    lines_ += QStringLiteral("%1  %2  %3  %4").arg(severity_word(row->severity), source, message, time);
  }
  if (more > 0) {
    auto* rest = new QLabel(tr("+%1 more").arg(more), this);
    rest->setContentsMargins(10, 1, 10, 2);
    layout_->addWidget(rest);
    lines_ += rest->text();
  }
}

void AlarmStrip::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit clicked();
    event->accept();
    return;
  }
  QFrame::mousePressEvent(event);
}

}  // namespace pychron::ui
