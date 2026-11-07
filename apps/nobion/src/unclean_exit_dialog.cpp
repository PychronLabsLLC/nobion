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
  setWindowTitle(tr("nobion did not close cleanly last time"));
  setObjectName(QStringLiteral("uncleanExitDialog"));
  auto* layout = new QVBoxLayout(this);

  QString text = tr("The previous session (build %1, started %2) ended without a clean exit.")
                     .arg(QString::fromStdString(previous.build_id), QString::fromStdString(previous.started));
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

void show_unclean_exit_notice(QWidget* window) {
  const auto hub = process_log_hub();
  if (!hub || window == nullptr) return;
  const auto previous = hub->previous_session();
  if (!previous) return;
  auto* dialog = new UncleanExitDialog(*previous, window);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::NonModal);
  dialog->show();
}

}  // namespace nobion::ui
