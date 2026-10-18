#pragma once

// A PI entered by its parts (PI spec, section 6): kind, family name, given
// name, ORCID, affiliation and ROR, each its own field, nothing parsed. The
// same form adds a PI and edits one. PiQuestionDialog asks which PI a name
// means when stored PIs share it: one of them, or another person.

#include <functional>
#include <optional>
#include <vector>

#include <QDialog>

#include "entry_headers.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

// A PI as the form holds it: every field checked (PiDialog::fields).
struct PiFields {
  persistence::PiKind kind = persistence::PiKind::Person;
  std::string family_name, given_name;
  std::optional<std::string> orcid, ror, affiliation;
  friend bool operator==(const PiFields&, const PiFields&) = default;
};

PiFields pi_fields_of(const persistence::PrincipalInvestigatorRow& row);
entry::PiName pi_name_of(const PiFields& fields);

// The stored PIs that `fields` could be (PI spec 6): persons of the same
// family name, case aside, whose given names share an initial (every person
// of the family name when either has no given name); organisations of the
// same name, case aside. In `all`'s order.
std::vector<persistence::PrincipalInvestigatorRow> pi_namesakes(
    const PiFields& fields, const std::vector<persistence::PrincipalInvestigatorRow>& all);

// How a PI is listed (PI spec 5.1): its display name, and when another PI of
// `all` shows the same text, its affiliation in brackets, else the last four
// characters of its ORCID.
QString pi_label(const persistence::PrincipalInvestigatorRow& row,
                 const std::vector<persistence::PrincipalInvestigatorRow>& all);

// The catalog edits that turn `stored` into `fields` (changed columns only,
// the stored values as expected); empty when nothing changed.
persistence::CatalogEditBatch pi_update(const persistence::PrincipalInvestigatorRow& stored, const PiFields& fields);

class PiDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PiDialog(QWidget* parent = nullptr);

  void set_fields(const PiFields& fields);
  // The fields, checked: a family name, an ORCID and a ROR of the right
  // form; an organisation has no given name and no ORCID.
  Result<PiFields> fields() const;
  // The fields of the last accept().
  const PiFields& accepted_fields() const noexcept { return accepted_; }

  // Accepts only when fields() does; otherwise the error stays shown.
  void accept() override;

  QComboBox* kind_box() const noexcept { return kind_; }
  QLineEdit* family_edit() const noexcept { return family_; }
  QLineEdit* given_edit() const noexcept { return given_; }
  QLineEdit* orcid_edit() const noexcept { return orcid_; }
  QLineEdit* affiliation_edit() const noexcept { return affiliation_; }
  QLineEdit* ror_edit() const noexcept { return ror_; }
  QPushButton* ok_button() const noexcept { return ok_; }
  QString error() const;

 private:
  void check();
  void kind_changed();

  QComboBox* kind_;
  QLineEdit *family_, *given_, *orcid_, *affiliation_, *ror_;
  QLabel* error_;
  QPushButton* ok_;
  PiFields accepted_;
};

class PiQuestionDialog : public QDialog {
  Q_OBJECT

 public:
  // `projects`: the catalog's, listed beside the PI they belong to.
  PiQuestionDialog(const entry::PiQuestion& question, const std::vector<persistence::ProjectRow>& projects,
                   QWidget* parent = nullptr);

  // After accept: the candidate chosen, or no PI for another person.
  const std::optional<entry::PiAnswer>& answer() const noexcept { return answer_; }

  QTableWidget* candidates() const noexcept { return table_; }
  QPushButton* this_one_button() const noexcept { return this_one_; }
  QPushButton* another_button() const noexcept { return another_; }

 private:
  std::vector<persistence::Uuid> uuids_;
  QTableWidget* table_;
  QPushButton *this_one_, *another_;
  std::optional<entry::PiAnswer> answer_;
};

// How a window asks a namesake question: the answer, or nullopt when the
// person would not say (tests answer without a dialog).
using PiAsk = std::function<std::optional<entry::PiAnswer>(const entry::PiQuestion&)>;
// Asks with a modal PiQuestionDialog over `parent`.
std::optional<entry::PiAnswer> ask_pi_modal(QWidget* parent, const entry::PiQuestion& question,
                                            const std::vector<persistence::ProjectRow>& projects);

}  // namespace pychron::ui
