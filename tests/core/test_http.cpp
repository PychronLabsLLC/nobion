#include <chrono>

#include <gtest/gtest.h>

#include "nobion/core/http.hpp"

namespace pychron::http {
namespace {

TEST(Http, RequestDefaultsToThirtySeconds) {
  EXPECT_EQ(HttpRequest{}.timeout, std::chrono::milliseconds{30000});
}

TEST(Http, NotSentCode) { EXPECT_EQ(kHttpNotSent, "http_not_sent"); }

}  // namespace
}  // namespace pychron::http
