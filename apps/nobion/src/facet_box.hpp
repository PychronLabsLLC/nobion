#pragma once

// FacetBox (data browser search and display design, section 3.3): one of the
// browser's filter lists. Tick values to keep; the line above the list hides
// the values that do not contain its text, without asking the source again.
// A ticked value is never hidden.

#include <QGroupBox>
#include <QString>
#include <QStringList>
#include <vector>

class QLineEdit;
class QListWidget;
class QToolButton;

namespace pychron::ui {

class FacetBox : public QGroupBox {
  Q_OBJECT

 public:
  // What a line shows, and what ticking it filters by: two PIs of one name
  // are two values, whatever they read as.
  struct Item {
    QString value, label;
  };

  explicit FacetBox(const QString& title, QWidget* parent = nullptr);

  // The values to offer. Ticks are kept, and a ticked value that is no longer
  // among them stays, at the end, with the label it had. The filter text
  // stays and is applied again, to the labels. Emits nothing.
  void set_items(const std::vector<Item>& items);
  // Values that are their own label.
  void set_values(const QStringList& values);
  // The values ticked, not their labels.
  QStringList checked() const;
  // Unticks everything; changed() once when anything was ticked.
  void clear_checked();

  QListWidget* list() const noexcept { return list_; }
  QLineEdit* filter() const noexcept { return filter_; }
  QToolButton* clear_button() const noexcept { return clear_; }

 signals:
  // The user ticked or unticked a value, or cleared the ticks.
  void changed();

 private:
  void apply_filter();
  void update_header();

  QString title_;
  QLineEdit* filter_;
  QToolButton* clear_;
  QListWidget* list_;
  bool updating_ = false;
};

}  // namespace pychron::ui
