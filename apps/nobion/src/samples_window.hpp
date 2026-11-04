#pragma once

// Entry > Samples (entry spec, section 9.1): browse, edit, add and delete
// samples. Edits stay in the table until Save, which writes them as one batch;
// rows another client changed meanwhile come back tinted and nothing is
// written.

#include <optional>

#include <QMainWindow>
#include <QPointer>

#include "entry_bridge.hpp"
#include "pi_dialog.hpp"
#include "sample_table_model.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;

namespace pychron::ui {

class SampleImportDialog;

class SamplesWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `bridge` must outlive the window.
  explicit SamplesWindow(EntryBridge& bridge, QWidget* parent = nullptr);

  SampleTableModel* model() const noexcept { return model_; }
  QTableView* table() const noexcept { return table_; }
  QString message() const;
  bool busy() const noexcept { return busy_ > 0; }

  // Reads the catalog and the samples the filters select (asynchronous).
  void reload();
  // Writes the edits (asynchronous); a no-op without edits.
  void save();
  // Stages the sample in the new-sample form; false (with a message) when it
  // is incomplete.
  bool add_from_form();
  // Fills the form; its PI is chosen when the sample names one (tests).
  void set_form(const NewSample& sample);
  SampleImportDialog* open_import();

  // The PI form for a new PI (PI spec 6), shown without blocking. Accepted,
  // a PI with namesakes is asked about first: one of them is chosen, or a new
  // PI is staged; either becomes the form's PI.
  PiDialog* new_pi();
  // The PI form over a stored PI (the PI filter's when `pi` is not given);
  // accepted, its changes are written at once. Null without a PI.
  PiDialog* edit_pi(std::optional<persistence::Uuid> pi = std::nullopt);
  // How the window asks a namesake question (tests answer without a dialog).
  void set_ask_pi(PiAsk ask) { ask_pi_ = std::move(ask); }
  QComboBox* pi_box() const noexcept { return new_pi_; }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void apply_loaded(entry::CatalogSnapshot catalog, std::vector<persistence::SampleRow> rows);
  void fill_filter(QComboBox* box, const std::vector<std::pair<QString, persistence::Uuid>>& items);
  void show_message(const QString& text, bool error = false);
  void check_duplicates();
  void fill_pi_box(const std::optional<persistence::Uuid>& select);
  void choose_pi(const PiFields& fields);
  void save_pi(const persistence::PrincipalInvestigatorRow& stored, const PiFields& fields);

  EntryBridge& bridge_;
  SampleTableModel* model_;
  QTableView* table_;
  QComboBox *pi_filter_, *project_filter_, *material_filter_;
  QLineEdit* search_;
  // The new-sample form.
  QLineEdit *new_name_, *new_grainsize_, *new_lat_, *new_lon_, *new_note_;
  QComboBox *new_pi_, *new_project_, *new_material_;
  QLabel* duplicates_;
  QLabel* message_;
  entry::CatalogSnapshot catalog_;
  int busy_ = 0;
  QPointer<SampleImportDialog> import_;
  PiAsk ask_pi_;
};

}  // namespace pychron::ui
