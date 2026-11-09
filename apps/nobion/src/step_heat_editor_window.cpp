#include "step_heat_editor_window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleValidator>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include "menu_hub.hpp"
#include "theme.hpp"
#include "toml_comments.hpp"

namespace nobion::ui {

using experiment::StepHeatTemplate;
using experiment::Unit;

namespace {

const QString kSettingsGroup = QStringLiteral("step_heat_editor");

constexpr std::array<Unit, 5> kUnits{Unit::Watts, Unit::Percent, Unit::Temp, Unit::Amps, Unit::Volts};

QString unit_text(Unit u) {
  const std::string_view s = experiment::to_string(u);
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString seconds_text(const std::optional<experiment::Duration>& d) {
  return d ? QString::number(d->count()) : QString();
}

// "21 min", "1 h 5 min", "40 s": as the run factory's step heat preview says it.
QString time_text(experiment::Duration d) {
  const double seconds = d.count();
  const auto whole_seconds = std::llround(seconds);
  const auto minutes = std::llround(seconds / 60);
  if (whole_seconds < 60) return QStringLiteral("%1 s").arg(whole_seconds);
  if (minutes < 60) return QStringLiteral("%1 min").arg(minutes);
  QString out = QStringLiteral("%1 h").arg(minutes / 60);
  if (minutes % 60 != 0) out += QStringLiteral(" %1 min").arg(minutes % 60);
  return out;
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

QToolButton* tool(const QString& text, const QString& tip, const char* name) {
  auto* b = new QToolButton;
  b->setText(text);
  b->setToolTip(tip);
  b->setObjectName(QString::fromLatin1(name));
  style::make_outlined(b);
  return b;
}

}  // namespace

StepHeatEditorWindow::StepHeatEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings,
                                           QWidget* parent)
    : QMainWindow(parent),
      files_(lab.paths.step_heats()),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      file_list_(new QListWidget),
      name_(new QLabel),
      description_(new QLineEdit),
      units_(new QComboBox),
      duration_(new QLineEdit),
      cleanup_(new QLineEdit),
      table_(new QTableView),
      totals_(new QLabel),
      diagnostics_(new QListWidget),
      status_(new QLabel),
      save_action_(new QAction(tr("&Save"), this)),
      save_as_action_(new QAction(tr("Save &As..."), this)),
      revert_action_(new QAction(tr("&Revert Step Heat Template"), this)),
      from_factory_action_(new QAction(tr("New Step Heat Template from Run Factory Text"), this)) {
  resize(900, 680);
  file_list_->setObjectName(QStringLiteral("files"));
  name_->setObjectName(QStringLiteral("name"));
  description_->setObjectName(QStringLiteral("description"));
  units_->setObjectName(QStringLiteral("units"));
  duration_->setObjectName(QStringLiteral("duration"));
  cleanup_->setObjectName(QStringLiteral("cleanup"));
  totals_->setObjectName(QStringLiteral("totals"));
  diagnostics_->setObjectName(QStringLiteral("diagnostics"));

  // Template defaults: empty (or "(form)") is the run factory's value.
  units_->addItem(tr("(form)"));
  for (const Unit u : kUnits) units_->addItem(unit_text(u), static_cast<int>(u));
  for (QLineEdit* e : {duration_, cleanup_}) {
    e->setPlaceholderText(tr("(form)"));
    auto* v = new QDoubleValidator(0, 1e6, 3, e);
    v->setNotation(QDoubleValidator::StandardNotation);
    e->setValidator(v);
  }

  table_->setModel(&model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectItems);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setStretchLastSection(true);

  // Files.
  auto* new_button = tool(QStringLiteral("+"), tr("New step heat template"), "new_file");
  auto* duplicate_button = tool(tr("Duplicate"), tr("Copy the open template under a new name"), "duplicate_file");
  auto* delete_button = tool(QStringLiteral("−"), tr("Delete the open template"), "delete_file");
  auto* file_buttons = new QHBoxLayout;
  for (QWidget* b : {static_cast<QWidget*>(new_button), static_cast<QWidget*>(duplicate_button),
                     static_cast<QWidget*>(delete_button)})
    file_buttons->addWidget(b);
  file_buttons->addStretch(1);
  auto* left = new QWidget;
  auto* left_col = new QVBoxLayout(left);
  left_col->addWidget(new QLabel(tr("Templates")));
  left_col->addWidget(file_list_, 1);
  left_col->addLayout(file_buttons);

  // Header of the open template.
  auto* head = new QFormLayout;
  head->addRow(tr("Name"), name_);
  head->addRow(tr("Description"), description_);
  auto* defaults = new QHBoxLayout;
  defaults->addWidget(new QLabel(tr("Units")));
  defaults->addWidget(units_);
  defaults->addWidget(new QLabel(tr("Duration (s)")));
  defaults->addWidget(duration_);
  defaults->addWidget(new QLabel(tr("Cleanup (s)")));
  defaults->addWidget(cleanup_);
  defaults->addStretch(1);
  head->addRow(tr("Defaults"), defaults);

  // Rows.
  auto* add_button = tool(tr("Add"), tr("Add a step after the selected or last one"), "add_step");
  auto* insert_button = tool(tr("Insert"), tr("Insert a copy of the selected step before it"), "insert_step");
  auto* remove_button = tool(tr("Delete"), tr("Delete the selected steps"), "delete_steps");
  auto* up_button = tool(tr("Up"), tr("Move the selected step up"), "step_up");
  auto* down_button = tool(tr("Down"), tr("Move the selected step down"), "step_down");
  auto* fill_button = tool(tr("Fill..."), tr("Set one column on the selected steps (every step when none)"), "fill");
  auto* fill_text_button =
      tool(tr("Fill from text..."), tr("Values such as 5:2.5:4 or 1, 2, 3: on the selected steps, else appended"),
           "fill_text");
  auto* row_buttons = new QHBoxLayout;
  for (QWidget* b : {add_button, insert_button, remove_button, up_button, down_button, fill_button, fill_text_button})
    row_buttons->addWidget(b);
  row_buttons->addStretch(1);

  auto* right = new QWidget;
  auto* right_col = new QVBoxLayout(right);
  right_col->addLayout(head);
  right_col->addLayout(row_buttons);
  right_col->addWidget(table_, 3);
  right_col->addWidget(totals_);
  right_col->addWidget(new QLabel(tr("Diagnostics")));
  right_col->addWidget(diagnostics_, 1);

  auto* split = new QSplitter;
  split->addWidget(left);
  split->addWidget(right);
  split->setStretchFactor(1, 1);
  split->setSizes({180, 720});
  setCentralWidget(split);
  statusBar()->addWidget(status_, 1);

  edit_controls_ = {description_,  units_,       duration_,   cleanup_,     table_,          add_button,
                    insert_button, remove_button, up_button, down_button, fill_button, fill_text_button,
                    duplicate_button};

  // File > Save and Save As (MenuHub) act here while this window is in front.
  MenuHub& menus = MenuHub::instance();
  menus.set_file_action(this, MenuHub::FileRole::Save, save_action_, tr("Step Heat Template"));
  menus.set_file_action(this, MenuHub::FileRole::SaveAs, save_as_action_, tr("Step Heat Template"));
  menus.contribute(this, MenuHub::Menu::File, {revert_action_, from_factory_action_}, MenuHub::Scope::Window);
  from_factory_action_->setEnabled(false);

  connect(save_action_, &QAction::triggered, this, [this] {
    QString error;
    if (!save(&error) && !error.isEmpty()) QMessageBox::warning(this, tr("Save"), error);
  });
  connect(save_as_action_, &QAction::triggered, this, [this] {
    const auto name = ask_name_();
    QString error;
    if (name && !save_as(*name, &error) && !error.isEmpty()) QMessageBox::warning(this, tr("Save As"), error);
  });
  connect(revert_action_, &QAction::triggered, this, [this] { revert(); });
  connect(from_factory_action_, &QAction::triggered, this, [this] {
    QString error;
    if (!new_from_factory(&error) && !error.isEmpty()) QMessageBox::warning(this, tr("New template"), error);
  });

  ask_unsaved_ = [this](const QString& name) {
    const auto b = QMessageBox::question(this, tr("Unsaved step heat template"),
                                         tr("%1 has unsaved changes. Save them?").arg(name),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                         QMessageBox::Cancel);
    return b == QMessageBox::Save ? Unsaved::Save : b == QMessageBox::Discard ? Unsaved::Discard : Unsaved::Cancel;
  };
  confirm_ = [this](const QString& question) {
    return QMessageBox::question(this, tr("Step heat templates"), question) == QMessageBox::Yes;
  };
  ask_name_ = [this]() -> std::optional<QString> {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Step heat template"),
                                               tr("Name (a-z, 0-9, '_', '-')"), QLineEdit::Normal, QString(), &ok);
    return ok ? std::optional<QString>(name.trimmed()) : std::nullopt;
  };

  connect(file_list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
    const QString name = item->data(Qt::UserRole).toString();
    if (name != current_ && !open(name)) fill_files();  // put the highlight back
  });
  connect(file_list_, &QListWidget::itemClicked, file_list_, &QListWidget::itemActivated);
  auto warn = [this](const QString& title, const QString& error) {
    if (!error.isEmpty()) QMessageBox::warning(this, title, error);
  };
  connect(new_button, &QToolButton::clicked, this, [this, warn] {
    QString error;
    if (const auto name = ask_name_(); name && !new_file(*name, &error)) warn(tr("New template"), error);
  });
  connect(duplicate_button, &QToolButton::clicked, this, [this, warn] {
    QString error;
    if (current_.isEmpty()) return;
    if (const auto name = ask_name_(); name && !duplicate(current_, *name, &error)) warn(tr("Duplicate"), error);
  });
  connect(delete_button, &QToolButton::clicked, this, [this, warn] {
    QString error;
    if (!current_.isEmpty() && !delete_file(current_, &error)) warn(tr("Delete"), error);
  });

  connect(add_button, &QToolButton::clicked, this, [this] {
    const std::vector<int> rows = selected_rows();
    select_row(model_.add_step(rows.empty() ? std::nullopt : std::optional<int>(rows.back())));
  });
  connect(insert_button, &QToolButton::clicked, this, [this] {
    const std::vector<int> rows = selected_rows();
    if (!rows.empty()) select_row(model_.insert_step(rows.front()));
  });
  connect(remove_button, &QToolButton::clicked, this, [this] {
    const std::vector<int> rows = selected_rows();
    if (rows.empty()) return;
    model_.remove_rows(rows);
    select_row(std::min(rows.front(), model_.rowCount() - 1));
  });
  connect(up_button, &QToolButton::clicked, this, [this] {
    const std::vector<int> rows = selected_rows();
    if (rows.size() == 1 && model_.move_up(rows.front())) select_row(rows.front() - 1);
  });
  connect(down_button, &QToolButton::clicked, this, [this] {
    const std::vector<int> rows = selected_rows();
    if (rows.size() == 1 && model_.move_down(rows.front())) select_row(rows.front() + 1);
  });
  connect(fill_button, &QToolButton::clicked, this, [this] { fill_dialog(); });
  connect(fill_text_button, &QToolButton::clicked, this, [this] { fill_text_dialog(); });

  connect(description_, &QLineEdit::textEdited, this, [this] { push_defaults(); });
  connect(duration_, &QLineEdit::textEdited, this, [this] { push_defaults(); });
  connect(cleanup_, &QLineEdit::textEdited, this, [this] { push_defaults(); });
  connect(units_, &QComboBox::currentIndexChanged, this, [this] { push_defaults(); });
  connect(&model_, &StepHeatTableModel::edited, this, [this] { refresh(); });

  settings_->beginGroup(kSettingsGroup);
  if (auto g = settings_->value(QStringLiteral("geometry")).toByteArray(); !g.isEmpty()) restoreGeometry(g);
  const QString last = settings_->value(QStringLiteral("last")).toString();
  settings_->endGroup();

  show_nothing();
  const QStringList names = file_names();
  if (names.contains(last)) load(last);
  else if (!names.isEmpty()) load(names.first());
}

StepHeatEditorWindow::~StepHeatEditorWindow() = default;

QStringList StepHeatEditorWindow::file_names() const {
  QStringList out;
  if (auto names = files_.list())
    for (const auto& n : *names) out.append(QString::fromStdString(n));
  if (!new_name_.isEmpty() && !out.contains(new_name_)) out.append(new_name_);
  out.sort();
  return out;
}

void StepHeatEditorWindow::fill_files() {
  const QSignalBlocker block(file_list_);
  file_list_->clear();
  for (const QString& name : file_names()) {
    auto* item = new QListWidgetItem(name == new_name_ ? name + QStringLiteral(" *") : name, file_list_);
    item->setData(Qt::UserRole, name);
    if (name == current_) file_list_->setCurrentItem(item);
  }
}

void StepHeatEditorWindow::set_editable(bool on) {
  editable_ = on;
  for (QWidget* w : edit_controls_) w->setEnabled(on);
}

void StepHeatEditorWindow::fill_defaults() {
  filling_ = true;
  const StepHeatTemplate& t = model_.tmpl();
  name_->setText(current_);
  description_->setText(QString::fromStdString(t.description));
  units_->setCurrentIndex(t.units ? units_->findData(static_cast<int>(*t.units)) : 0);
  duration_->setText(seconds_text(t.duration));
  cleanup_->setText(seconds_text(t.cleanup));
  filling_ = false;
}

void StepHeatEditorWindow::push_defaults() {
  if (filling_ || !editable_) return;
  const StepHeatTemplate& t = model_.tmpl();
  // Text that does not read as a number (yet) leaves the default as it was.
  auto seconds = [](const QLineEdit* e, std::optional<experiment::Duration> was) {
    const QString text = e->text().trimmed();
    if (text.isEmpty()) return std::optional<experiment::Duration>();
    bool ok = false;
    const double v = e->locale().toDouble(text, &ok);
    return ok ? std::optional<experiment::Duration>(experiment::Duration{v}) : was;
  };
  const QVariant unit = units_->currentData();
  model_.set_template_defaults(unit.isValid() ? std::optional<Unit>(static_cast<Unit>(unit.toInt())) : std::nullopt,
                               seconds(duration_, t.duration), seconds(cleanup_, t.cleanup),
                               description_->text().toStdString());
}

void StepHeatEditorWindow::show_template(const StepHeatTemplate& t) {
  model_.set_template(t);
  fill_defaults();
  fill_files();
  refresh();
}

void StepHeatEditorWindow::show_nothing() {
  current_.clear();
  has_comments_ = false;
  saved_ = {};
  set_editable(false);
  show_template({});
  status_->setText(tr("No step heat template. + creates one."));
}

void StepHeatEditorWindow::load(const QString& name) {
  current_ = name;
  has_comments_ = false;
  auto t = files_.read(name.toStdString());
  if (!t) {
    saved_ = {};
    set_editable(false);
    show_template({});
    auto* item = new QListWidgetItem(tr("error: %1").arg(QString::fromStdString(t.error().what)), diagnostics_);
    item->setForeground(theme().error_text);
    status_->setText(tr("%1 cannot be edited: %2").arg(name, QString::fromStdString(t.error().what)));
    return;
  }
  if (const auto text = read_text(files_.path(name.toStdString()))) has_comments_ = has_toml_comment(*text);
  saved_ = std::move(*t);
  set_editable(true);
  show_template(saved_);
  status_->setText(QString::fromStdString(files_.path(name.toStdString()).string()));
}

void StepHeatEditorWindow::start_new(StepHeatTemplate t, const QString& name) {
  t.name = name.toStdString();
  new_name_ = name;
  current_ = name;
  has_comments_ = false;
  saved_ = std::move(t);
  set_editable(true);
  show_template(saved_);
  status_->setText(tr("%1 is written by the first save.").arg(name));
}

bool StepHeatEditorWindow::modified() const {
  if (!editable_ || current_.isEmpty()) return false;
  return current_ == new_name_ || experiment::normalized(model_.tmpl()) != experiment::normalized(saved_);
}

bool StepHeatEditorWindow::resolve_unsaved() {
  if (!modified()) return true;
  switch (ask_unsaved_(current_)) {
    case Unsaved::Cancel: return false;
    case Unsaved::Save: {
      QString error;
      if (save(&error)) return true;
      if (!error.isEmpty()) status_->setText(error);
      return false;
    }
    case Unsaved::Discard:
      new_name_.clear();  // a new file that was never saved is gone
      return true;
  }
  return false;
}

bool StepHeatEditorWindow::open(const QString& name) {
  if (name == current_) return true;
  if (!file_names().contains(name) || name == new_name_) return false;
  if (!resolve_unsaved()) return false;
  load(name);
  return true;
}

bool StepHeatEditorWindow::check_new_name(const QString& name, QString* error) const {
  auto refuse = [&](const QString& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (!experiment::valid_step_heat_name(name.toStdString()))
    return refuse(tr("'%1' is not a template name (a-z, 0-9, '_', '-')").arg(name));
  // The filesystem is asked too: on a case-insensitive one names can meet.
  if (file_names().contains(name) || files_.exists(name.toStdString()))
    return refuse(tr("%1 already exists").arg(name));
  return true;
}

bool StepHeatEditorWindow::new_file(const QString& name, QString* error) {
  return new_from(StepHeatTemplate{}, name, error);
}

bool StepHeatEditorWindow::new_from(StepHeatTemplate t, const QString& name, QString* error) {
  if (!check_new_name(name, error)) return false;
  if (!resolve_unsaved()) {
    if (error != nullptr) error->clear();
    return false;
  }
  start_new(std::move(t), name);
  return true;
}

bool StepHeatEditorWindow::new_from_factory(QString* error) {
  if (!from_factory_) return false;
  auto t = from_factory_();
  if (!t) return false;
  const auto name = ask_name_();
  if (!name) return false;
  return new_from(std::move(*t), *name, error);
}

bool StepHeatEditorWindow::write(const StepHeatTemplate& t, QString* error) {
  if (auto r = files_.write(t); !r) {
    const QString why = QString::fromStdString(r.error().what);
    if (error != nullptr) *error = why;
    status_->setText(why);
    return false;
  }
  return true;
}

bool StepHeatEditorWindow::duplicate(const QString& name, const QString& as, QString* error) {
  auto refuse = [&](const QString& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (!file_names().contains(name)) return refuse(tr("no template named %1").arg(name));
  if (!check_new_name(as, error)) return false;
  StepHeatTemplate t;
  if (name == current_ && name == new_name_) {
    t = model_.tmpl();
  } else {
    auto read = files_.read(name.toStdString());
    if (!read) return refuse(QString::fromStdString(read.error().what));
    t = std::move(*read);
  }
  t.name = as.toStdString();
  if (!resolve_unsaved()) return refuse(QString());
  if (!write(t, error)) return false;
  load(as);
  emit filesChanged();
  return true;
}

bool StepHeatEditorWindow::delete_file(const QString& name, QString* error) {
  if (!file_names().contains(name)) {
    if (error != nullptr) *error = tr("no template named %1").arg(name);
    return false;
  }
  if (!confirm_(tr("Delete %1.toml?").arg(name))) return false;
  if (name == new_name_) {
    new_name_.clear();
  } else if (auto r = files_.remove(name.toStdString()); !r) {
    if (error != nullptr) *error = QString::fromStdString(r.error().what);
    return false;
  }
  if (name == current_) {
    const QStringList names = file_names();
    if (names.isEmpty()) show_nothing();
    else load(names.first());
  } else {
    fill_files();
  }
  emit filesChanged();
  return true;
}

bool StepHeatEditorWindow::save(QString* error) {
  auto refuse = [&](const QString& why) {
    if (error != nullptr) *error = why;
    if (!why.isEmpty()) status_->setText(why);
    return false;
  };
  if (current_.isEmpty()) return refuse(tr("No template is open"));
  if (!editable_) return refuse(tr("%1 cannot be edited").arg(current_));
  if (const QStringList lines = diagnostic_lines(); !lines.isEmpty())
    return refuse(tr("Fix the problems before saving: %1").arg(lines.join(QStringLiteral("; "))));
  if (has_comments_ &&
      !confirm_(tr("%1.toml has comments; saving rewrites the file and drops them. Save?").arg(current_)))
    return refuse(QString());
  StepHeatTemplate t = model_.tmpl();
  t.name = current_.toStdString();
  if (!write(t, error)) return false;
  const bool created = current_ == new_name_;
  new_name_.clear();
  has_comments_ = false;
  saved_ = std::move(t);
  if (created) fill_files();
  refresh();
  status_->setText(tr("Saved %1. The run factory's next Add uses it.").arg(current_));
  emit saved(current_);
  if (created) emit filesChanged();
  return true;
}

bool StepHeatEditorWindow::save_as(const QString& name, QString* error) {
  auto refuse = [&](const QString& why) {
    if (error != nullptr) *error = why;
    if (!why.isEmpty()) status_->setText(why);
    return false;
  };
  if (current_.isEmpty()) return refuse(tr("No template is open"));
  if (!editable_) return refuse(tr("%1 cannot be edited").arg(current_));
  if (name == current_ && name == new_name_) return save(error);
  if (const QStringList lines = diagnostic_lines(); !lines.isEmpty())
    return refuse(tr("Fix the problems before saving: %1").arg(lines.join(QStringLiteral("; "))));
  if (!check_new_name(name, error)) return false;
  StepHeatTemplate t = model_.tmpl();
  t.name = name.toStdString();
  if (!write(t, error)) return false;
  // A new file saved under another name is that file now; the old one is kept.
  new_name_.clear();
  current_ = name;
  has_comments_ = false;
  saved_ = t;
  model_.set_template(std::move(t));
  fill_defaults();
  fill_files();
  refresh();
  status_->setText(tr("Saved %1.").arg(current_));
  emit saved(current_);
  emit filesChanged();
  return true;
}

void StepHeatEditorWindow::revert() {
  if (!editable_) return;
  show_template(saved_);
}

void StepHeatEditorWindow::set_form_extraction(std::function<experiment::ExtractionSpec()> f) {
  form_extraction_ = std::move(f);
  refresh_form();
}

void StepHeatEditorWindow::set_from_factory(std::function<std::optional<StepHeatTemplate>()> f) {
  from_factory_ = std::move(f);
  refresh_form();
}

void StepHeatEditorWindow::refresh_form() {
  model_.set_form_extraction(form_extraction_ ? form_extraction_() : experiment::ExtractionSpec{});
  from_factory_action_->setEnabled(from_factory_ && from_factory_().has_value());
  refresh();
}

void StepHeatEditorWindow::refresh() {
  diagnostics_->clear();
  if (editable_) {
    for (const auto& line : experiment::check_step_heat(model_.tmpl())) {
      auto* item = new QListWidgetItem(QString::fromStdString(line), diagnostics_);
      item->setForeground(theme().error_text);
    }
  }
  const bool good = editable_ && diagnostics_->count() == 0;
  save_action_->setEnabled(good);
  save_as_action_->setEnabled(good);
  revert_action_->setEnabled(modified() && current_ != new_name_);
  totals_->setText(totals_text());
  setWindowTitle(current_.isEmpty()
                     ? tr("Step Heat Templates")
                     : tr("Step Heat Templates - %1%2").arg(current_, modified() ? QStringLiteral(" *") : QString()));
}

QStringList StepHeatEditorWindow::diagnostic_lines() const {
  QStringList out;
  // The load error of a file that cannot be edited is shown, not a rule.
  if (!editable_) return out;
  for (int i = 0; i < diagnostics_->count(); ++i) out.append(diagnostics_->item(i)->text());
  return out;
}

QString StepHeatEditorWindow::totals_text() const {
  const auto n = model_.tmpl().steps.size();
  if (n == 0) return tr("No steps");
  const experiment::ExtractionSpec form = form_extraction_ ? form_extraction_() : experiment::ExtractionSpec{};
  const QString steps = n == 1 ? tr("1 step") : tr("%1 steps").arg(n);
  return tr("%1, %2 extraction").arg(steps, time_text(experiment::extraction_time(form, model_.tmpl())));
}

std::vector<int> StepHeatEditorWindow::selected_rows() const {
  std::set<int> rows;
  for (const QModelIndex& i : table_->selectionModel()->selectedIndexes()) rows.insert(i.row());
  if (rows.empty() && table_->currentIndex().isValid()) rows.insert(table_->currentIndex().row());
  return {rows.begin(), rows.end()};
}

void StepHeatEditorWindow::select_row(int row) {
  if (row < 0 || row >= model_.rowCount()) return;
  table_->setCurrentIndex(model_.index(row, StepHeatTableModel::Value));
}

void StepHeatEditorWindow::fill_dialog() {
  if (!editable_ || model_.rowCount() == 0) return;
  QStringList columns;
  for (int c = StepHeatTableModel::Value; c < StepHeatTableModel::ColumnCount; ++c)
    columns.append(model_.headerData(c, Qt::Horizontal).toString());
  bool ok = false;
  const QString column = QInputDialog::getItem(this, tr("Fill"), tr("Column"), columns, 0, false, &ok);
  if (!ok) return;
  const QString value = QInputDialog::getText(this, tr("Fill"), tr("%1 (empty: inherit)").arg(column),
                                              QLineEdit::Normal, QString(), &ok);
  if (!ok) return;
  const auto c = static_cast<StepHeatTableModel::Column>(StepHeatTableModel::Value + columns.indexOf(column));
  model_.fill(c, value.trimmed(), selected_rows());
}

void StepHeatEditorWindow::fill_text_dialog() {
  if (!editable_) return;
  const std::vector<int> rows = selected_rows();
  bool ok = false;
  const QString text = QInputDialog::getText(
      this, tr("Fill from text"),
      rows.empty() ? tr("Values to append (5:2.5:4 or 1, 2, 3)") : tr("Values for the %1 selected steps").arg(rows.size()),
      QLineEdit::Normal, QString(), &ok);
  if (!ok || text.trimmed().isEmpty()) return;
  if (auto r = model_.fill_values(rows, text); !r)
    QMessageBox::warning(this, tr("Fill from text"), QString::fromStdString(r.error().what));
}

void StepHeatEditorWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::ActivationChange && isActiveWindow()) refresh_form();
  QMainWindow::changeEvent(event);
}

void StepHeatEditorWindow::closeEvent(QCloseEvent* event) {
  const QString was = current_;
  if (!resolve_unsaved()) {
    event->ignore();
    return;
  }
  // Discarded edits are gone the next time the window shows.
  if (modified() || !file_names().contains(was)) {
    if (file_names().contains(was)) load(was);
    else if (const QStringList names = file_names(); !names.isEmpty()) load(names.first());
    else show_nothing();
  }
  settings_->beginGroup(kSettingsGroup);
  settings_->setValue(QStringLiteral("geometry"), saveGeometry());
  settings_->setValue(QStringLiteral("last"), current_);
  settings_->endGroup();
  QMainWindow::closeEvent(event);
}

}  // namespace nobion::ui
