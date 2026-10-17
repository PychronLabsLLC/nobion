#pragma once

// Name rules and generated names for entry (sample and package entry spec,
// section 6, names.hpp). Every name is trimmed of surrounding whitespace
// before it is checked.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/catalog.hpp"

namespace pychron::entry {

std::string trim(std::string_view text);

// A PI as entry reads it: a person's family and given name(s) as written, or
// an organisation's name in family_name (PI spec 5, 6). Fields in this order
// so that {family, given} is a person.
struct PiName {
  std::string family_name;
  std::string given_name;  // "" when none; a person's only
  persistence::PiKind kind = persistence::PiKind::Person;
  friend bool operator==(const PiName&, const PiName&) = default;
};

// A person, "Family" or "Family, Given" (T039's rule, PI spec 6). The family
// name is taken as written: any letters (UTF-8), particles and spaces ("van
// der Pluijm", "Ruiz García"), with ' - and . inside it, at least two
// characters, and not all in small ASCII letters ("ross"). The given name is
// kept in full ("Ross, María José" is {"Ross", "María José"}); a lone initial
// loses its full stop ("Ross, J." is {"Ross", "J"}). A digit or other
// punctuation is an error, unless the whole text is one of the lab's
// `allowed` names, which is then the family name as it stands ("Lab 7").
// "Jake Ross" cannot be told from a family name of two words and is one.
Result<PiName> parse_pi(std::string_view text, const std::vector<std::string>& allowed = {});
// An organisation, by its name as written (trimmed, not empty).
Result<PiName> parse_organisation(std::string_view text);
// An organisation has no given name and no ORCID (PI spec 6); refused with
// the name quoted.
Result<void> check_pi(const PiName& name, const std::optional<std::string>& orcid);
// "Family, Given" or "Family": the form parse_pi reads; an organisation's name.
std::string sheet_name(const PiName& name);
// "Given Family" or "Family"; an organisation's name: as the store's
// display_name (PI spec 5.1).
std::string display_name(const PiName& name);
// The first letter of a given name, upper case when it is an ASCII one, whole
// when it is not ASCII (its UTF-8 bytes); "" for "".
std::string initial_of(std::string_view given);

// An ORCID iD in its bare form ("0000-0002-1825-0097"): four groups of four
// digits joined by '-', the last character a digit or 'X', the ISO 7064 mod
// 11-2 check character right. A leading "https://orcid.org/" (or http) is
// taken off. A bad form is refused with the value quoted. Format only:
// nothing is looked up.
Result<std::string> parse_orcid(std::string_view text);
// A ROR id in its bare form ("03yrm5c26"): '0', six of 0-9 and the lower-case
// letters but i, l, o and u, then two digits. A leading "https://ror.org/"
// (or http) is taken off. A bad form is refused with the value quoted.
Result<std::string> parse_ror(std::string_view text);

// A letter, then letters, digits, '-' or '_'.
bool valid_project_name(std::string_view name);
// Not empty, no whitespace (legacy SpacelessStr).
bool valid_package_name(std::string_view name);

// The next package name with `prefix`: the largest number among `existing`
// names "<prefix><digits>" plus one, zero-padded to that name's width and at
// least 3 digits ("NM-001" -> "NM-002", "NM-999" -> "NM-1000"); "<prefix>001"
// when there is none (legacy labnumber_entry.py:792-870).
std::string next_package_name(const std::vector<std::string>& existing, std::string_view prefix);

// Level letters: "A" -> "B", "Z" -> "AA", "AZ" -> "BA". Empty or not all
// upper-case letters: "A".
std::string next_letters(std::string_view letters);
// The level after the highest letter name among `existing` (shorter first,
// then alphabetical); "A" when there is none.
std::string next_level_name(const std::vector<std::string>& existing);

// Optional letters then digits ("P7", "12").
bool valid_packet(std::string_view packet);
// The packet after `packet`, keeping its letters and digit width:
// "P7" -> "P8", "P09" -> "P10", "9" -> "10". Nullopt for an invalid packet.
std::optional<std::string> next_packet(std::string_view packet);

}  // namespace pychron::entry
