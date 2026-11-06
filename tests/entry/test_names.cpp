#include <gtest/gtest.h>

#include "nobion/entry/names.hpp"

using namespace pychron::entry;
using pychron::persistence::PiKind;

// A family name is written as its owner writes it: particles, spaces and any
// letters. What follows the comma is the given name, kept in full.
// A sheet or a web page gives these; on screen they are a plain space and
// nothing, and kept they would make two PIs that read the same.
TEST(Names, AnOddSpaceIsASpaceAndAnInvisibleCharacterIsRefused) {
  EXPECT_EQ(*parse_pi("Ross,\xC2\xA0" "J"), (PiName{"Ross", "J"}));
  EXPECT_EQ(*parse_pi("Ruiz\xC2\xA0" "Garc\xC3\xAD" "a"), (PiName{"Ruiz Garc\xC3\xAD" "a", ""}));
  EXPECT_EQ(*parse_pi("\xC2\xA0Ross\xC2\xA0"), (PiName{"Ross", ""}));
  EXPECT_FALSE(parse_pi("Ro\xE2\x80\x8Bss"));
  EXPECT_FALSE(parse_pi("Ross, \xEF\xBB\xBFJ"));
  EXPECT_EQ(parse_organisation("New\xC2\xA0Mexico Tech")->family_name, "New Mexico Tech");
}

TEST(Names, AFamilyNameMayHaveParticlesSpacesAndAnyLetters) {
  EXPECT_EQ(*parse_pi("van der Pluijm"), (PiName{"van der Pluijm", ""}));
  EXPECT_EQ(*parse_pi("van der Pluijm, B"), (PiName{"van der Pluijm", "B"}));
  EXPECT_EQ(*parse_pi("de la Cruz, M"), (PiName{"de la Cruz", "M"}));
  EXPECT_EQ(*parse_pi("García"), (PiName{"García", ""}));
  EXPECT_EQ(*parse_pi("Nguyễn, T"), (PiName{"Nguyễn", "T"}));
  EXPECT_EQ(*parse_pi("St. Clair, R"), (PiName{"St. Clair", "R"}));
  EXPECT_EQ(*parse_pi("O’Brien"), (PiName{"O’Brien", ""}));
  EXPECT_EQ(*parse_pi("Ruiz   García"), (PiName{"Ruiz García", ""}));  // one space between words
  // a cell without a comma is a family name alone (PI spec 6), even of two words
  EXPECT_EQ(*parse_pi("Jake Ross"), (PiName{"Jake Ross", ""}));
  EXPECT_EQ(*parse_pi("NMGRL lab"), (PiName{"NMGRL lab", ""}));
}

TEST(Names, AGivenNameIsKeptInFull) {
  EXPECT_EQ(*parse_pi("Ross, Jake"), (PiName{"Ross", "Jake"}));
  EXPECT_EQ(*parse_pi("Ross, jake"), (PiName{"Ross", "jake"}));
  EXPECT_EQ(*parse_pi("Ross, J."), (PiName{"Ross", "J"}));  // a lone initial loses its full stop
  EXPECT_EQ(*parse_pi("Ross,Jake  Isaac"), (PiName{"Ross", "Jake Isaac"}));
  EXPECT_EQ(*parse_pi("Ross, J. I."), (PiName{"Ross", "J. I."}));
  EXPECT_EQ(*parse_pi("Ruiz García, María José"), (PiName{"Ruiz García", "María José"}));
  EXPECT_EQ(initial_of("Jake"), "J");
  EXPECT_EQ(initial_of("jake"), "J");
  EXPECT_EQ(initial_of("Åse"), "Å");  // the whole letter, not its first byte
  EXPECT_EQ(initial_of(""), "");
}

TEST(Names, WhatIsStillNoPrincipalInvestigator) {
  for (const char* bad : {"", "   ", "R", "ross", "jake ross", "Ross,", "Ross, ", ", J", "Ross, J, K", "Ross2", "Lab 7",
                          "Ross, 7", "Ross; J", "Ross (NMT)", "-Ross", "Ross, -", "Ross\tJ", "a@b.c"}) {
    auto r = parse_pi(bad);
    EXPECT_FALSE(r) << "'" << bad << "'";
    if (!r) {
      EXPECT_NE(r.error().what.find("Family, Given"), std::string::npos) << r.error().what;
    }
  }
  // such a name is an organisation's, as written (PI spec 6.1: no allowed list)
  EXPECT_EQ(*parse_organisation("Lab 7"), (PiName{"Lab 7", "", PiKind::Organisation}));
}

TEST(Names, PrincipalInvestigator) {
  EXPECT_EQ(*parse_pi("Ross"), (PiName{"Ross", ""}));
  EXPECT_EQ(*parse_pi("Ross, J"), (PiName{"Ross", "J"}));
  EXPECT_EQ(*parse_pi(" Ross,J "), (PiName{"Ross", "J"}));
  EXPECT_EQ(*parse_pi("O'Brien-Smith, K"), (PiName{"O'Brien-Smith", "K"}));
  EXPECT_EQ(sheet_name({"Ross", "Jake"}), "Ross, Jake");
  EXPECT_EQ(sheet_name({"Ross", ""}), "Ross");
  // as the store shows it (PI spec 5.1)
  EXPECT_EQ(display_name({"Ross", "Jake"}), "Jake Ross");
  EXPECT_EQ(display_name({"Ross", "J"}), "J Ross");
  EXPECT_EQ(display_name({"Ross", ""}), "Ross");
  EXPECT_EQ(display_name({"NMGRL", "", PiKind::Organisation}), "NMGRL");
}

TEST(Names, AnOrganisationIsItsNameWithNoGivenNameOrOrcid) {
  EXPECT_EQ(*parse_organisation(" Lab 7, NMT "), (PiName{"Lab 7, NMT", "", PiKind::Organisation}));
  EXPECT_FALSE(parse_organisation("  "));
  EXPECT_TRUE(check_pi({"NMGRL", "", PiKind::Organisation}, std::nullopt));
  EXPECT_TRUE(check_pi({"Ross", "Jake"}, std::string("0000-0002-1825-0097")));
  const auto given = check_pi({"NMGRL", "Lab", PiKind::Organisation}, std::nullopt);
  ASSERT_FALSE(given);
  EXPECT_NE(given.error().what.find("'NMGRL'"), std::string::npos) << given.error().what;
  EXPECT_FALSE(check_pi({"NMGRL", "", PiKind::Organisation}, std::string("0000-0002-1825-0097")));
}

TEST(Names, AnOrcidIsCheckedByItsChecksum) {
  EXPECT_EQ(*parse_orcid("0000-0002-1825-0097"), "0000-0002-1825-0097");
  EXPECT_EQ(*parse_orcid("https://orcid.org/0000-0002-1825-0097"), "0000-0002-1825-0097");
  EXPECT_EQ(*parse_orcid(" http://orcid.org/0000-0002-1825-0097 "), "0000-0002-1825-0097");
  EXPECT_EQ(*parse_orcid("0000-0001-5109-3700"), "0000-0001-5109-3700");
  EXPECT_EQ(*parse_orcid("0000-0002-1694-233X"), "0000-0002-1694-233X");
  EXPECT_FALSE(parse_orcid("0000-0002-1825-0098"));  // a changed digit
  EXPECT_FALSE(parse_orcid("0000-0002-1694-2330"));  // the X made a digit
  EXPECT_FALSE(parse_orcid("0000-0002-1825-009X"));
  EXPECT_FALSE(parse_orcid("0000000218250097"));  // wrong grouping
  EXPECT_FALSE(parse_orcid("000-00002-1825-0097"));
  EXPECT_FALSE(parse_orcid("0000-0002-1825-0097-"));
  EXPECT_FALSE(parse_orcid("abcd-0002-1825-0097"));
  EXPECT_FALSE(parse_orcid("0000-000X-1825-0097"));  // X only last
  EXPECT_FALSE(parse_orcid(""));
  const auto bad = parse_orcid("0000-0002-1825-0098");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("'0000-0002-1825-0098'"), std::string::npos) << bad.error().what;
}

TEST(Names, ARorIsItsNineCharacters) {
  EXPECT_EQ(*parse_ror("03yrm5c26"), "03yrm5c26");
  EXPECT_EQ(*parse_ror("https://ror.org/03yrm5c26"), "03yrm5c26");
  EXPECT_EQ(*parse_ror(" 05dxps055 "), "05dxps055");
  EXPECT_FALSE(parse_ror("03yrm5c2"));    // short
  EXPECT_FALSE(parse_ror("03yrm5c266"));  // long
  EXPECT_FALSE(parse_ror("03yrl5c26"));   // 'l' is not in the set
  EXPECT_FALSE(parse_ror("03yru5c26"));   // nor 'u'
  EXPECT_FALSE(parse_ror("13yrm5c26"));   // not starting with 0
  EXPECT_FALSE(parse_ror("03yrm5c2a"));   // the last two are digits
  EXPECT_FALSE(parse_ror(""));
  const auto bad = parse_ror("13yrm5c26");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("'13yrm5c26'"), std::string::npos) << bad.error().what;
}

TEST(Names, Project) {
  EXPECT_TRUE(valid_project_name("IR-1010_a"));
  EXPECT_TRUE(valid_project_name("a"));
  EXPECT_FALSE(valid_project_name("1abc"));
  EXPECT_FALSE(valid_project_name("a b"));
  EXPECT_FALSE(valid_project_name(""));
}

TEST(Names, PackageIncrement) {
  EXPECT_EQ(next_package_name({"NM-001"}, "NM-"), "NM-002");
  EXPECT_EQ(next_package_name({"NM-299", "NM-300", "NM-298"}, "NM-"), "NM-301");
  EXPECT_EQ(next_package_name({"NM-999"}, "NM-"), "NM-1000");
  EXPECT_EQ(next_package_name({"NM001"}, "NM"), "NM002");
  EXPECT_EQ(next_package_name({"NM-ABC-001", "NM-002"}, "NM-ABC-"), "NM-ABC-002");
  EXPECT_EQ(next_package_name({"NM-1", "NM-x7"}, "NM-"), "NM-002");
  EXPECT_EQ(next_package_name({"Other-5"}, "NM-"), "NM-001");
  EXPECT_EQ(next_package_name({}, "NM-"), "NM-001");
  EXPECT_TRUE(valid_package_name("NM-300"));
  EXPECT_FALSE(valid_package_name("NM 300"));
  EXPECT_FALSE(valid_package_name(""));
}

TEST(Names, LevelLetters) {
  EXPECT_EQ(next_letters("A"), "B");
  EXPECT_EQ(next_letters("Z"), "AA");
  EXPECT_EQ(next_letters("AZ"), "BA");
  EXPECT_EQ(next_letters("ZZ"), "AAA");
  EXPECT_EQ(next_level_name({"A", "C", "B"}), "D");
  EXPECT_EQ(next_level_name({"Z", "AA"}), "AB");
  EXPECT_EQ(next_level_name({}), "A");
  EXPECT_EQ(next_level_name({"top"}), "A");
}

TEST(Names, Packets) {
  EXPECT_TRUE(valid_packet("P7"));
  EXPECT_TRUE(valid_packet("12"));
  EXPECT_FALSE(valid_packet("P"));
  EXPECT_FALSE(valid_packet("7P"));
  EXPECT_FALSE(valid_packet(""));
  EXPECT_EQ(next_packet("P7"), "P8");
  EXPECT_EQ(next_packet("9"), "10");
  EXPECT_EQ(next_packet("P09"), "P10");
  EXPECT_EQ(next_packet("P99"), "P100");
  EXPECT_FALSE(next_packet("x"));
}
