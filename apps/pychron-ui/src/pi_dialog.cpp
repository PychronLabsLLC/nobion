#include "pi_dialog.hpp"

#include <algorithm>
#include <cctype>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "theme.hpp"

namespace pychron::ui {

namespace ps = persistence;

namespace {

std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::optional<std::string> optional_text(const QLineEdit* edit) {
  const std::string t = entry::trim(edit->text().toStdString());
  return t.empty() ? std::nullopt : std::optional<std::string>(t);
}

ps::CatalogValue value_of(const std::optional<std::string>& v) { return v ? ps::CatalogValue{*v} : ps::CatalogValue{}; }

}  // namespace

PiFields pi_fields_of(const ps::PrincipalInvestigatorRow& row) {
  return PiFields{row.kind, row.family_name, row.given_name, row.orcid, row.ror, row.affiliation};
}

entry::PiName pi_name_of(const PiFields& fields) { return {fields.family_name, fields.given_name, fields.kind}; }

std::vector<ps::PrincipalInvestigatorRow> pi_namesakes(const PiFields& fields,
                                                       const std::vector<ps::PrincipalInvestigatorRow>& all) {
  std::vector<ps::PrincipalInvestigatorRow> out;
  const std::string family = ascii_lower(fields.family_name);
  const std::string initial = entry::initial_of(fields.given_name);
  std::copy_if(all.begin(), all.end(), std::back_inserter(out), [&](const ps::PrincipalInvestigatorRow& p) {
    if (p.kind != fields.kind || ascii_lower(p.family_name) != family) return false;
    if (p.kind == ps::PiKind::Organisation) return true;
    const std::string theirs = entry::initial_of(p.given_name);
    return initial.empty() || theirs.empty() || theirs == initial;
  });
  return out;
}

QString pi_label(const ps::PrincipalInvestigatorRow& row, const std::vector<ps::PrincipalInvestigatorRow>& all) {
  QString shown = QString::fromStdString(row.display_name);
  const bool shared = std::any_of(all.begin(), all.end(), [&](const ps::PrincipalInvestigatorRow& p) {
    return p.uuid != row.uuid && p.display_name == row.display_name;
  });
  if (!shared) return shown;
  if (row.affiliation && !row.affiliation->empty())
    return QStringLiteral("%1 (%2)").arg(shown, QString::fromStdString(*row.affiliation));
  if (row.orcid && row.orcid->size() >= 4)
    return QStringLiteral("%1 (%2)").arg(shown, QString::fromStdString(row.orcid->substr(row.orcid->size() - 4)));
  return shown;
}

ps::CatalogEditBatch pi_update(const ps::PrincipalInvestigatorRow& stored, const PiFields& fields) {
  ps::CatalogUpdate u{ps::CatalogTable::PrincipalInvestigator, stored.uuid, {}, {}};
  const auto change = [&u](const char* column, ps::CatalogValue before, ps::CatalogValue after) {
    if (before == after) return;
    u.expected[column] = std::move(before);
    u.values[column] = std::move(after);
  };
  change("kind", std::string(ps::pi_kind_name(stored.kind)), std::string(ps::pi_kind_name(fields.kind)));
  change("family_name", stored.family_name, fields.family_name);
  change("given_name", stored.given_name, fields.given_name);
  change("orcid", value_of(stored.orcid), value_of(fields.orcid));
  change("ror", value_of(stored.ror), value_of(fields.ror));
  change("affiliation", value_of(stored.affiliation), value_of(fields.affiliation));
  ps::CatalogEditBatch batch;
  batch.message = "principal investigator";
  if (!u.values.empty()) batch.edits.emplace_back(std::move(u));
  return batch;
}

PiDialog::PiDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Principal Investigator"));
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();
  kind_ = new QComboBox(this);
  kind_->addItem(tr("Person"), QStringLiteral("person"));
  kind_->addItem(tr("Organisation"), QStringLiteral("organisation"));
  family_ = new QLineEdit(this);
  given_ = new QLineEdit(this);
  orcid_ = new QLineEdit(this);
  affiliation_ = new QLineEdit(this);
  ror_ = new QLineEdit(this);
  family_->setPlaceholderText(tr("in full, as written"));
  given_->setPlaceholderText(tr("in full, or an initial"));
  orcid_->setPlaceholderText(tr("0000-0002-1825-0097 (optional)"));
  ror_->setPlaceholderText(tr("03yrm5c26 (optional)"));
  form->addRow(tr("Kind"), kind_);
  form->addRow(tr("Family name"), family_);
  form->addRow(tr("Given name"), given_);
  form->addRow(tr("ORCID"), orcid_);
  form->addRow(tr("Affiliation"), affiliation_);
  form->addRow(tr("ROR"), ror_);
  layout->addLayout(form);
  error_ = new QLabel(this);
  error_->setWordWrap(true);
  style::set_tone(error_, style::Tone::Error);
  layout->addWidget(error_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  ok_ = buttons->button(QDialogButtonBox::Ok);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &PiDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(kind_, &QComboBox::currentIndexChanged, this, [this] { kind_changed(); });
  for (QLineEdit* e : {family_, given_, orcid_, affiliation_, ror_})
    connect(e, &QLineEdit::textChanged, this, [this] { check(); });
  check();
}

QString PiDialog::error() const { return error_->text(); }

void PiDialog::set_fields(const PiFields& f) {
  kind_->setCurrentIndex(f.kind == ps::PiKind::Organisation ? 1 : 0);
  family_->setText(QString::fromStdString(f.family_name));
  given_->setText(QString::fromStdString(f.given_name));
  orcid_->setText(QString::fromStdString(f.orcid.value_or("")));
  affiliation_->setText(QString::fromStdString(f.affiliation.value_or("")));
  ror_->setText(QString::fromStdString(f.ror.value_or("")));
  kind_changed();
}

void PiDialog::kind_changed() {
  // An organisation has no given name and no ORCID (PI spec 4): the fields
  // keep what was typed, for a change back, but are not read.
  const bool person = kind_->currentIndex() == 0;
  given_->setEnabled(person);
  orcid_->setEnabled(person);
  family_->setPlaceholderText(person ? tr("in full, as written") : tr("the organisation's name"));
  check();
}

Result<PiFields> PiDialog::fields() const {
  PiFields f;
  f.kind = kind_->currentIndex() == 1 ? ps::PiKind::Organisation : ps::PiKind::Person;
  f.family_name = entry::trim(family_->text().toStdString());
  if (f.family_name.empty())
    return fail(ErrorKind::Config, f.kind == ps::PiKind::Person ? "a person needs a family name"
                                                                 : "an organisation needs a name");
  f.affiliation = optional_text(affiliation_);
  if (auto ror = optional_text(ror_)) {
    auto parsed = entry::parse_ror(*ror);
    if (!parsed) return fail(parsed.error());
    f.ror = std::move(*parsed);
  }
  if (f.kind == ps::PiKind::Person) {
    f.given_name = entry::trim(given_->text().toStdString());
    if (auto orcid = optional_text(orcid_)) {
      auto parsed = entry::parse_orcid(*orcid);
      if (!parsed) return fail(parsed.error());
      f.orcid = std::move(*parsed);
    }
  }
  if (auto ok = entry::check_pi(pi_name_of(f), f.orcid); !ok) return fail(ok.error());
  return f;
}

void PiDialog::check() {
  const auto f = fields();
  // An empty form is not an error yet: it only cannot be accepted.
  const bool blank = family_->text().trimmed().isEmpty();
  error_->setText(f || blank ? QString() : QString::fromStdString(f.error().what));
  style::set_invalid(orcid_, orcid_->isEnabled() && !orcid_->text().trimmed().isEmpty() &&
                                 !entry::parse_orcid(orcid_->text().toStdString()));
  style::set_invalid(ror_, !ror_->text().trimmed().isEmpty() && !entry::parse_ror(ror_->text().toStdString()));
  ok_->setEnabled(f.has_value());
}

void PiDialog::accept() {
  auto f = fields();
  if (!f) {
    error_->setText(QString::fromStdString(f.error().what));
    return;
  }
  accepted_ = std::move(*f);
  QDialog::accept();
}

PiQuestionDialog::PiQuestionDialog(const entry::PiQuestion& question, const std::vector<ps::ProjectRow>& projects,
                                   QWidget* parent)
    : QDialog(parent) {
  const bool person = question.pi.kind == ps::PiKind::Person;
  setWindowTitle(tr("Which PI?"));
  auto* layout = new QVBoxLayout(this);
  auto* text = new QLabel(tr("The catalog has more than one PI that \"%1\" could be. Which is it?")
                              .arg(QString::fromStdString(question.name)),
                          this);
  text->setWordWrap(true);
  layout->addWidget(text);
  table_ = new QTableWidget(static_cast<int>(question.candidates.size()), 4, this);
  table_->setHorizontalHeaderLabels({tr("Name"), tr("Affiliation"), tr("ORCID"), tr("Projects")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  for (std::size_t i = 0; i < question.candidates.size(); ++i) {
    const auto& c = question.candidates[i];
    uuids_.push_back(c.uuid);
    QStringList names;
    for (const auto& p : projects)
      if (p.principal_investigator == c.uuid) names << QString::fromStdString(p.name);
    const QStringList cells{QString::fromStdString(c.display_name), QString::fromStdString(c.affiliation.value_or("")),
                            QString::fromStdString(c.orcid.value_or("")), names.join(QStringLiteral(", "))};
    for (int col = 0; col < cells.size(); ++col) table_->setItem(static_cast<int>(i), col, new QTableWidgetItem(cells[col]));
  }
  layout->addWidget(table_, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  this_one_ = buttons->addButton(tr("This One"), QDialogButtonBox::AcceptRole);
  another_ = buttons->addButton(person ? tr("Another Person") : tr("Another Organisation"), QDialogButtonBox::AcceptRole);
  this_one_->setEnabled(false);
  layout->addWidget(buttons);
  connect(table_, &QTableWidget::itemSelectionChanged, this,
          [this] { this_one_->setEnabled(!table_->selectionModel()->selectedRows().isEmpty()); });
  connect(this_one_, &QPushButton::clicked, this, [this] {
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    answer_ = entry::PiAnswer{uuids_[static_cast<std::size_t>(rows.front().row())]};
    QDialog::accept();
  });
  connect(another_, &QPushButton::clicked, this, [this] {
    answer_ = entry::PiAnswer{std::nullopt};
    QDialog::accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  resize(640, 320);
}

std::optional<entry::PiAnswer> ask_pi_modal(QWidget* parent, const entry::PiQuestion& question,
                                            const std::vector<ps::ProjectRow>& projects) {
  PiQuestionDialog d(question, projects, parent);
  if (d.exec() != QDialog::Accepted) return std::nullopt;
  return d.answer();
}

}  // namespace pychron::ui
