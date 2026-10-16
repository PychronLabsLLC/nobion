#pragma once

// StepHeatEditorWindow (step heat template design 7): edits the templates of
// <lab>/queues/step_heats.
//
//   left    the lab's templates (+ new, duplicate, - delete)
//   right   the open template's name (read-only: rename is Save As),
//           description and defaults (units, duration, cleanup; empty is the
//           run factory's); its steps; totals; diagnostics
//
// One file is open at a time. The value rules (check_step_heat) are listed at
// once and keep the file from being saved. Saving writes the canonical TOML,
// so a hand-written file loses its comments (asked first). A file that does
// not load is shown with the reason and cannot be edited. Nothing outside
// <lab>/queues/step_heats is written.

#include <functional>
#include <memory>
#include <optional>

#include <QMainWindow>
#include <QSettings>

#include "pychron/experiment/factory/step_heat_template.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "step_heat_table_model.hpp"

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableView;
class QToolButton;

namespace pychron::ui {

class StepHeatEditorWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum class Unsaved { Save, Discard, Cancel };

  // `lab` must outlive the window.
  explicit StepHeatEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr,
                                QWidget* parent = nullptr);
  ~StepHeatEditorWindow() override;

  StepHeatEditorWindow(const StepHeatEditorWindow&) = delete;
  StepHeatEditorWindow& operator=(const StepHeatEditorWindow&) = delete;
  StepHeatEditorWindow(StepHeatEditorWindow&&) = delete;
  StepHeatEditorWindow& operator=(StepHeatEditorWindow&&) = delete;

  [[nodiscard]] QStringList file_names() const;  // sorted; a new, unsaved file included
  // Opens a file (asks about unsaved changes first). false on Cancel, a
  // failed save, or a name that is not listed.
  bool open(const QString& name);
  // Starts a file with no steps; it is written by the first save. false
  // (with `error`) for a bad or existing name.
  bool new_file(const QString& name, QString* error = nullptr);
  // As new_file, the file holding `t` (named `name`): Save as template.
  bool new_from(experiment::StepHeatTemplate t, const QString& name, QString* error = nullptr);
  // Edit > New from Run Factory Text: the from-factory hook's template, under
  // a name from the name dialog. false when either answers nothing.
  bool new_from_factory(QString* error = nullptr);
  // Writes a copy of `name` as `as` and opens it.
  bool duplicate(const QString& name, const QString& as, QString* error = nullptr);
  bool delete_file(const QString& name, QString* error = nullptr);  // asks first
  bool save(QString* error = nullptr);
  // Writes the open template as `name` and opens that; the old file is kept.
  bool save_as(const QString& name, QString* error = nullptr);
  // Back to the file as last read or saved.
  void revert();
  [[nodiscard]] bool modified() const;
  [[nodiscard]] const QString& current_name() const noexcept { return current_; }
  [[nodiscard]] bool editable() const { return editable_; }

  StepHeatTableModel& model() noexcept { return model_; }
  [[nodiscard]] QStringList diagnostic_lines() const;
  // "12 steps, 21 min extraction" (extraction_time against the form).
  [[nodiscard]] QString totals_text() const;

  // The run factory's extraction, which inherited values come from. Asked
  // when set and whenever the window is activated.
  void set_form_extraction(std::function<experiment::ExtractionSpec()> f);
  // The run factory's step heat text as a template; nullopt when it has none.
  void set_from_factory(std::function<std::optional<experiment::StepHeatTemplate>()> f);

  // Dialog hooks; defaults are dialogs. Tests replace them.
  void set_ask_unsaved(std::function<Unsaved(const QString& name)> ask) { ask_unsaved_ = std::move(ask); }
  void set_confirm(std::function<bool(const QString& question)> confirm) { confirm_ = std::move(confirm); }
  void set_ask_name(std::function<std::optional<QString>()> ask) { ask_name_ = std::move(ask); }

 signals:
  void saved(const QString& name);
  // A file was created or deleted.
  void filesChanged();

 protected:
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  bool resolve_unsaved();  // false: stay on the current file
  bool check_new_name(const QString& name, QString* error) const;
  bool write(const experiment::StepHeatTemplate& t, QString* error);
  void load(const QString& name);
  void start_new(experiment::StepHeatTemplate t, const QString& name);
  void show_template(const experiment::StepHeatTemplate& t);
  void show_nothing();
  void fill_files();
  void fill_defaults();
  void push_defaults();
  void refresh();  // diagnostics, totals, title, actions
  void refresh_form();
  void set_editable(bool on);
  [[nodiscard]] std::vector<int> selected_rows() const;
  void select_row(int row);
  void fill_dialog();
  void fill_text_dialog();

  experiment::StepHeatFiles files_;
  std::unique_ptr<QSettings> settings_;
  StepHeatTableModel model_;
  experiment::StepHeatTemplate saved_;  // as last read or written
  QString current_;
  QString new_name_;  // a file that exists only here until saved
  bool editable_ = false;
  bool has_comments_ = false;  // the file on disk has comments a save would drop
  bool filling_ = false;       // the default fields are being set from the model

  QListWidget* file_list_;
  QLabel* name_;
  QLineEdit* description_;
  QComboBox* units_;
  QLineEdit* duration_;
  QLineEdit* cleanup_;
  QTableView* table_;
  QLabel* totals_;
  QListWidget* diagnostics_;
  QLabel* status_;
  QList<QWidget*> edit_controls_;
  QAction* save_action_;
  QAction* save_as_action_;
  QAction* revert_action_;
  QAction* from_factory_action_;
  std::function<experiment::ExtractionSpec()> form_extraction_;
  std::function<std::optional<experiment::StepHeatTemplate>()> from_factory_;
  std::function<Unsaved(const QString&)> ask_unsaved_;
  std::function<bool(const QString&)> confirm_;
  std::function<std::optional<QString>()> ask_name_;
};

}  // namespace pychron::ui
