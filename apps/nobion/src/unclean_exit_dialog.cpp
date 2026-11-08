#include "unclean_exit_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "nobion/core/process_log.hpp"

namespace nobion::ui {

UncleanExitDialog::UncleanExitDialog(const PreviousSession& previous, QWidget* parent) : QDialog(parent) {
  // The sentinel's program: programs share a log folder, so it can be elctl's.
  const QString program =
      previous.program.empty() ? QStringLiteral("nobion") : QString::fromStdString(previous.program);
  setWindowTitle(tr("%1 did not close cleanly last time").arg(program));
  setObjectName(QStringLiteral("uncleanExitDialog"));
  auto* layout = new QVBoxLayout(this);

  QString text = tr("The previous %1 session (build %2, started %3) ended without a clean exit.")
                     .arg(program, QString::fromStdString(previous.build_id),
                          QString::fromStdString(previous.started));
  if (previous.crash_file) {
    text += QStringLiteral("\n\n") + tr("It left a crash file: %1").arg(QString::fromStdString(previous.crash_file->string()));
  }
  text += QStringLiteral("\n\n") +
          tr("Collect Diagnostics gathers the logs and crash files into one file you can send to support.");
  auto* label = new QLabel(text, this);
  label->setObjectName(QStringLiteral("uncleanExitText"));
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(label);

  auto* buttons = new QDialogButtonBox(this);
  auto* collect = buttons->addButton(tr("Collect Diagnostics…"), QDialogButtonBox::ActionRole);
  collect->setObjectName(QStringLiteral("collectDiagnosticsButton"));
  auto* dismiss = buttons->addButton(tr("Dismiss"), QDialogButtonBox::RejectRole);
  dismiss->setObjectName(QStringLiteral("dismissButton"));
  layout->addWidget(buttons);

  connect(collect, &QPushButton::clicked, this, [this] {
    emit collectRequested();
    close();
  });
  connect(dismiss, &QPushButton::clicked, this, &QDialog::reject);
}

UncleanExitDialog* show_unclean_exit_notice(QWidget* window) {
  const auto hub = process_log_hub();
  if (!hub || window == nullptr) return nullptr;
  const auto previous = hub->previous_session();
  if (!previous) return nullptr;
  auto* dialog = new UncleanExitDialog(*previous, window);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::NonModal);
  dialog->show();
  return dialog;
}

}  // namespace nobion::ui
