#include "samples_window.hpp"

#include <algorithm>

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "nobion/entry/names.hpp"
#include "error_report.hpp"
#include "sample_import_dialog.hpp"
#include "theme.hpp"

namespace nobion::ui {

namespace ps = persistence;

namespace {

struct Loaded {
  entry::CatalogSnapshot catalog;
  std::vector<ps::SampleRow> rows;
};

std::optional<ps::Uuid> selected(const QComboBox* box) {
  const QVariant v = box->currentData();
  if (!v.isValid() || v.toString().isEmpty()) return std::nullopt;
  return ps::Uuid::parse(v.toString().toStdString());
}

std::optional<double> number(const QLineEdit* edit, bool* bad) {
  const QString t = edit->text().trimmed();
  if (t.isEmpty()) return std::nullopt;
  bool ok = false;
  const double v = t.toDouble(&ok);
  if (!ok) *bad = true;
  return ok ? std::optional<double>(v) : std::nullopt;
}

}  // namespace

SamplesWindow::SamplesWindow(EntryBridge& bridge, QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), model_(new SampleTableModel(this)) {
  setWindowTitle(tr("Samples"));
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);

  // Filters.
  auto* filters = new QHBoxLayout();
  pi_filter_ = new QComboBox(central);
  project_filter_ = new QComboBox(central);
  material_filter_ = new QComboBox(central);
  search_ = new QLineEdit(central);
  search_->setPlaceholderText(tr("Search sample names"));
  search_->setClearButtonEnabled(true);
  filters->addWidget(new QLabel(tr("PI"), central));
  filters->addWidget(pi_filter_, 1);
  filters->addWidget(new QLabel(tr("Project"), central));
  filters->addWidget(project_filter_, 1);
  filters->addWidget(new QLabel(tr("Material"), central));
  filters->addWidget(material_filter_, 1);
  filters->addWidget(search_, 2);
  layout->addLayout(filters);
  for (QComboBox* box : {pi_filter_, project_filter_, material_filter_})
    connect(box, &QComboBox::activated, this, [this] { reload(); });
  connect(search_, &QLineEdit::returnPressed, this, [this] { reload(); });

  // The new-sample form.
  auto* form_box = new QGroupBox(tr("New sample"), central);
  auto* form = new QHBoxLayout(form_box);
  new_name_ = new QLineEdit(form_box);
  new_pi_ = new QComboBox(form_box);
  new_project_ = new QComboBox(form_box);
  new_material_ = new QComboBox(form_box);
  // A PI is chosen, or made with New PI…; never typed (PI spec 6).
  new_pi_->setMinimumContentsLength(14);
  for (QComboBox* box : {new_project_, new_material_}) {
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
  }
  new_grainsize_ = new QLineEdit(form_box);
  new_lat_ = new QLineEdit(form_box);
  new_lon_ = new QLineEdit(form_box);
  new_note_ = new QLineEdit(form_box);
  new_name_->setPlaceholderText(tr("name"));
  new_pi_->setPlaceholderText(tr("PI"));
  auto* new_pi_button = new QPushButton(tr("New PI…"), form_box);
  new_project_->lineEdit()->setPlaceholderText(tr("project"));
  new_material_->lineEdit()->setPlaceholderText(tr("material"));
  new_grainsize_->setPlaceholderText(tr("grainsize"));
  new_lat_->setPlaceholderText(tr("lat"));
  new_lon_->setPlaceholderText(tr("lon"));
  new_note_->setPlaceholderText(tr("note"));
  auto* add = new QPushButton(tr("Add"), form_box);
  for (QWidget* w : std::initializer_list<QWidget*>{new_name_, new_pi_, new_pi_button, new_project_, new_material_, new_grainsize_,
                                                    new_lat_, new_lon_, new_note_, add})
    form->addWidget(w);
  connect(add, &QPushButton::clicked, this, [this] { add_from_form(); });
  connect(new_pi_button, &QPushButton::clicked, this, [this] { new_pi(); });
  connect(new_name_, &QLineEdit::textChanged, this, [this] { check_duplicates(); });
  layout->addWidget(form_box);
  duplicates_ = new QLabel(central);
  style::set_tone(duplicates_, style::Tone::Warning);
  layout->addWidget(duplicates_);

  table_ = new QTableView(central);
  table_->setModel(model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                          QAbstractItemView::AnyKeyPressed);
  layout->addWidget(table_, 1);
  message_ = new QLabel(central);
  message_->setWordWrap(true);
  layout->addWidget(message_);
  setCentralWidget(central);

  auto* bar = addToolBar(tr("Samples"));
  bar->setObjectName(QStringLiteral("samples_toolbar"));
  bar->addAction(tr("Save"), this, [this] { save(); });
  bar->addAction(tr("Revert"), this, [this] {
    model_->revert();
    show_message(tr("Edits dropped"));
  });
  bar->addAction(tr("Delete"), this, [this] {
    int refused = 0;
    for (const QModelIndex& i : table_->selectionModel()->selectedRows())
      if (!model_->mark_delete(i.row())) ++refused;
    if (refused) show_message(tr("%1 samples are in positions or analyzed and stay").arg(refused), true);
  });
  bar->addAction(tr("Reload"), this, [this] { reload(); });
  bar->addAction(tr("Import…"), this, [this] { open_import(); });
  bar->addAction(tr("Edit PI…"), this, [this] {
    if (!edit_pi()) show_message(tr("Choose a PI in the PI filter to edit it"), true);
  });

  ask_pi_ = [this](const entry::PiQuestion& q) { return ask_pi_modal(this, q, catalog_.projects); };

  connect(&bridge_, &EntryBridge::changed, this, [this] {
    if (!model_->dirty()) reload();
  });
  resize(1200, 700);
  reload();
}

QString SamplesWindow::message() const { return message_->text(); }

void SamplesWindow::show_message(const QString& text, bool error) {
  if (error) log_shown("samples", windowTitle(), text);
  message_->setText(text);
  style::set_tone(message_, error ? style::Tone::Error : style::Tone::Normal);
}

void SamplesWindow::reload() {
  ps::SampleQuery query;
  query.text = search_->text().trimmed().toStdString();
  query.principal_investigator = selected(pi_filter_);
  query.project = selected(project_filter_);
  query.material = selected(material_filter_);
  ++busy_;
  bridge_.run<Loaded>(
      this,
      [query](ps::IStore& s, const ps::Actor&) -> Result<Loaded> {
        Loaded out;
        auto catalog = entry::read_snapshot(s);
        if (!catalog) return fail(catalog.error());
        out.catalog = std::move(*catalog);
        auto rows = s.samples(query);
        if (!rows) return fail(rows.error());
        out.rows = std::move(*rows);
        return out;
      },
      [this](Result<Loaded> r) {
        --busy_;
        if (!r) return show_message(QString::fromStdString(to_string(r.error())), true);
        apply_loaded(std::move(r->catalog), std::move(r->rows));
      });
}

void SamplesWindow::fill_filter(QComboBox* box, const std::vector<std::pair<QString, ps::Uuid>>& items) {
  const QString keep = box->currentData().toString();
  box->clear();
  box->addItem(tr("Any"), QString());
  for (const auto& [label, id] : items) box->addItem(label, QString::fromStdString(id.str()));
  const int at = box->findData(keep);
  box->setCurrentIndex(at < 0 ? 0 : at);
}

void SamplesWindow::apply_loaded(entry::CatalogSnapshot catalog, std::vector<ps::SampleRow> rows) {
  catalog_ = std::move(catalog);
  std::vector<std::pair<QString, ps::Uuid>> pis, projects, materials;
  QStringList project_names, material_names;
  for (const auto& p : catalog_.principal_investigators)
    pis.emplace_back(pi_label(p, catalog_.principal_investigators), p.uuid);
  for (const auto& p : catalog_.projects) {
    projects.emplace_back(QStringLiteral("%1 (%2)").arg(QString::fromStdString(p.name),
                                                        QString::fromStdString(p.principal_investigator_name)),
                          p.uuid);
    if (!project_names.contains(QString::fromStdString(p.name))) project_names << QString::fromStdString(p.name);
  }
  for (const auto& m : catalog_.materials) {
    const QString label = m.grainsize.empty() ? QString::fromStdString(m.name)
                                              : QStringLiteral("%1 (%2)").arg(QString::fromStdString(m.name),
                                                                               QString::fromStdString(m.grainsize));
    materials.emplace_back(label, m.uuid);
    if (!material_names.contains(QString::fromStdString(m.name))) material_names << QString::fromStdString(m.name);
  }
  fill_filter(pi_filter_, pis);
  fill_filter(project_filter_, projects);
  fill_filter(material_filter_, materials);
  for (auto [box, names] : {std::pair{new_project_, project_names}, std::pair{new_material_, material_names}}) {
    const QString text = box->currentText();
    box->clear();
    box->addItems(names);
    box->setCurrentText(text);
  }
  const auto pi = selected(new_pi_);
  model_->set_rows(std::move(rows));
  fill_pi_box(pi);
  show_message(tr("%1 samples").arg(model_->stored_count()));
}

void SamplesWindow::fill_pi_box(const std::optional<ps::Uuid>& select) {
  new_pi_->clear();
  for (const auto& p : catalog_.principal_investigators)
    new_pi_->addItem(pi_label(p, catalog_.principal_investigators), QString::fromStdString(p.uuid.str()));
  for (const auto& p : model_->new_pis())
    new_pi_->addItem(tr("%1 (new)").arg(QString::fromStdString(
                         entry::display_name({p.family_name, p.given_name, p.kind}))),
                     QString::fromStdString(p.uuid->str()));
  new_pi_->setCurrentIndex(select ? new_pi_->findData(QString::fromStdString(select->str())) : -1);
}

PiDialog* SamplesWindow::new_pi() {
  auto* d = new PiDialog(this);
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowTitle(tr("New PI"));
  connect(d, &QDialog::accepted, this, [this, d] { choose_pi(d->accepted_fields()); });
  d->show();
  return d;
}

void SamplesWindow::choose_pi(const PiFields& f) {
  const auto& stored = catalog_.principal_investigators;
  const auto select = [this](const ps::Uuid& id) {
    new_pi_->setCurrentIndex(new_pi_->findData(QString::fromStdString(id.str())));
  };
  // An ORCID is one person, and unique in the store: that PI, not a new one.
  if (f.orcid) {
    for (const auto& p : stored)
      if (p.orcid == f.orcid) {
        select(p.uuid);
        return show_message(tr("ORCID %1 is %2's: chosen").arg(QString::fromStdString(*f.orcid),
                                                                 QString::fromStdString(p.display_name)));
      }
  }
  // A name stored PIs share is asked about, never guessed (PI spec 6).
  if (auto namesakes = pi_namesakes(f, stored); !namesakes.empty()) {
    entry::PiQuestion q;
    q.pi = pi_name_of(f);
    q.name = entry::question_name(q.pi);
    q.candidates = std::move(namesakes);
    const auto answer = ask_pi_ ? ask_pi_(q) : std::nullopt;
    if (!answer) return show_message(tr("No PI chosen"));
    if (answer->pi) return select(*answer->pi);
  }
  ps::PrincipalInvestigatorSpec spec;
  spec.kind = f.kind;
  spec.family_name = f.family_name;
  spec.given_name = f.given_name;
  spec.orcid = f.orcid;
  spec.ror = f.ror;
  spec.affiliation = f.affiliation;
  spec.uuid = ps::Uuid::v7();
  const ps::Uuid id = *spec.uuid;
  model_->add_pi(std::move(spec));
  fill_pi_box(id);
  show_message(tr("New PI; Save writes it"));
}

PiDialog* SamplesWindow::edit_pi(std::optional<ps::Uuid> pi) {
  if (!pi) pi = selected(pi_filter_);
  if (!pi) return nullptr;
  const auto& all = catalog_.principal_investigators;
  const auto row = std::find_if(all.begin(), all.end(), [&pi](const ps::PrincipalInvestigatorRow& p) { return p.uuid == *pi; });
  if (row == all.end()) return nullptr;
  auto* d = new PiDialog(this);
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->set_fields(pi_fields_of(*row));
  connect(d, &QDialog::accepted, this, [this, d, stored = *row] { save_pi(stored, d->accepted_fields()); });
  d->show();
  return d;
}

void SamplesWindow::save_pi(const ps::PrincipalInvestigatorRow& stored, const PiFields& f) {
  auto batch = pi_update(stored, f);
  if (batch.edits.empty()) return;
  ++busy_;
  bridge_.run<ps::CatalogOutcome>(
      this,
      [batch = std::move(batch)](ps::IStore& s, const ps::Actor& a) { return s.apply_catalog_edits(a.client, batch); },
      [this](Result<ps::CatalogOutcome> r) {
        --busy_;
        if (!r) return show_message(QString::fromStdString(to_string(r.error())), true);
        if (std::holds_alternative<ps::CatalogApplied>(*r)) {
          show_message(tr("PI saved"));
          bridge_.notify_changed();
          if (model_->dirty()) return;  // a reload would drop the edits; the next one shows the PI
          reload();
        }
        if (const auto* refused = std::get_if<std::vector<ps::Refusal>>(&*r)) {
          QStringList lines;
          for (const auto& x : *refused) lines << QString::fromStdString(x.what);
          return show_message(tr("The PI was not saved: %1").arg(lines.join(QStringLiteral("; "))), true);
        }
        show_message(tr("The PI was changed by another client; nothing was saved. Reload, then edit again."), true);
      });
}

void SamplesWindow::check_duplicates() {
  const auto near = entry::near_duplicates(new_name_->text().toStdString(), catalog_.samples);
  if (near.empty()) {
    duplicates_->clear();
    return;
  }
  QStringList where;
  for (const auto& s : near)
    where << QStringLiteral("%1 (%2, %3)").arg(QString::fromStdString(s.name), QString::fromStdString(s.project_name),
                                               QString::fromStdString(s.material_name));
  duplicates_->setText(tr("Similar samples exist: %1").arg(where.join(QStringLiteral(", "))));
}

void SamplesWindow::set_form(const NewSample& s) {
  new_name_->setText(QString::fromStdString(s.name));
  if (s.principal_investigator)
    new_pi_->setCurrentIndex(new_pi_->findData(QString::fromStdString(s.principal_investigator->str())));
  new_project_->setCurrentText(QString::fromStdString(s.project));
  new_material_->setCurrentText(QString::fromStdString(s.material));
  new_grainsize_->setText(QString::fromStdString(s.grainsize));
  new_lat_->setText(s.fields.lat ? QString::number(*s.fields.lat, 'g', 10) : QString());
  new_lon_->setText(s.fields.lon ? QString::number(*s.fields.lon, 'g', 10) : QString());
  new_note_->setText(s.fields.note ? QString::fromStdString(*s.fields.note) : QString());
}

bool SamplesWindow::add_from_form() {
  NewSample s;
  s.name = entry::trim(new_name_->text().toStdString());
  s.principal_investigator = selected(new_pi_);
  s.principal_investigator_name = new_pi_->currentText().toStdString();
  s.project = entry::trim(new_project_->currentText().toStdString());
  s.material = entry::trim(new_material_->currentText().toStdString());
  s.grainsize = entry::trim(new_grainsize_->text().toStdString());
  bool bad = false;
  s.fields.lat = number(new_lat_, &bad);
  s.fields.lon = number(new_lon_, &bad);
  if (const QString note = new_note_->text().trimmed(); !note.isEmpty()) s.fields.note = note.toStdString();
  if (bad) {
    show_message(tr("Latitude and longitude are numbers"), true);
    return false;
  }
  if (s.name.empty() || !s.principal_investigator || s.project.empty() || s.material.empty()) {
    show_message(tr("A new sample needs a name, a PI, a project and a material"), true);
    return false;
  }
  if (auto ok = entry::check_lat_lon(s.fields.lat, s.fields.lon); !ok) {
    show_message(QString::fromStdString(ok.error().what), true);
    return false;
  }
  model_->add_new(std::move(s));
  new_name_->clear();
  show_message(tr("Added; Save writes it"));
  return true;
}

void SamplesWindow::save() {
  if (!model_->dirty()) return;
  auto batch = model_->to_batch(catalog_);
  if (!batch) return show_message(QString::fromStdString(batch.error().what), true);
  ++busy_;
  bridge_.run<ps::CatalogOutcome>(
      this,
      [batch = std::move(*batch)](ps::IStore& s, const ps::Actor& a) { return s.apply_catalog_edits(a.client, batch); },
      [this](Result<ps::CatalogOutcome> r) {
        --busy_;
        if (!r) return show_message(QString::fromStdString(to_string(r.error())), true);
        if (std::holds_alternative<ps::CatalogApplied>(*r)) {
          show_message(tr("Saved"));
          model_->revert();
          bridge_.notify_changed();
          reload();
          return;
        }
        if (const auto* stale = std::get_if<std::vector<ps::StaleRow>>(&*r)) {
          model_->mark_stale(*stale);
          return show_message(tr("%1 samples were changed by another client; nothing was saved. Reload, then edit "
                                 "again.")
                                  .arg(stale->size()),
                              true);
        }
        if (const auto* refused = std::get_if<std::vector<ps::Refusal>>(&*r)) {
          QStringList lines;
          for (const auto& x : *refused) lines << QString::fromStdString(x.what);
          return show_message(tr("Nothing was saved: %1").arg(lines.join(QStringLiteral("; "))), true);
        }
        show_message(tr("Nothing was saved"), true);
      });
}

SampleImportDialog* SamplesWindow::open_import() {
  if (!import_) {
    import_ = new SampleImportDialog(bridge_, this);
    import_->setAttribute(Qt::WA_DeleteOnClose);
  }
  import_->show();
  import_->raise();
  return import_;
}

void SamplesWindow::closeEvent(QCloseEvent* event) {
  if (model_->dirty()) {
    const auto answer = QMessageBox::question(this, windowTitle(), tr("Save the sample edits?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) return event->ignore();
    if (answer == QMessageBox::Save) save();
    else model_->revert();
  }
  event->accept();
}

}  // namespace nobion::ui
