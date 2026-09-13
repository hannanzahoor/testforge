#pragma once

/// Unprefixed assertion spellings: ASSERT_TRUE, ASSERT_EQ, ...
///
/// These are the names test authors expect, but ASSERT_* is also GoogleTest's
/// namespace. Rather than shadow it and produce a confusing error the first
/// time somebody includes both headers, the short names are opt-in: TestForge
/// suites include this header, TestForge's *own* GoogleTest-based unit tests
/// do not.
///
/// Including this after <gtest/gtest.h> is a hard error rather than a silent
/// redefinition.

#include "testforge/testing/Assertions.hpp"

#if defined(ASSERT_TRUE) || defined(ASSERT_EQ)
#error \
    "testforge/testing/ShortAssertions.hpp conflicts with an already-included assertion library (GoogleTest?). Use the TF_ASSERT_* macros from testforge/testing/Assertions.hpp instead."
#endif

#define ASSERT_TRUE(expr) TF_ASSERT_TRUE(expr)
#define ASSERT_FALSE(expr) TF_ASSERT_FALSE(expr)
#define ASSERT_EQ(actual, expected) TF_ASSERT_EQ(actual, expected)
#define ASSERT_NE(actual, forbidden) TF_ASSERT_NE(actual, forbidden)
#define ASSERT_LT(actual, bound) TF_ASSERT_LT(actual, bound)
#define ASSERT_LE(actual, bound) TF_ASSERT_LE(actual, bound)
#define ASSERT_GT(actual, bound) TF_ASSERT_GT(actual, bound)
#define ASSERT_GE(actual, bound) TF_ASSERT_GE(actual, bound)
#define ASSERT_CONTAINS(haystack, needle) TF_ASSERT_CONTAINS(haystack, needle)
#define ASSERT_NOT_CONTAINS(haystack, needle) TF_ASSERT_NOT_CONTAINS(haystack, needle)
#define ASSERT_MATCHES(text, pattern) TF_ASSERT_MATCHES(text, pattern)
#define ASSERT_NEAR_TO(actual, expected, tolerance) TF_ASSERT_NEAR(actual, expected, tolerance)
#define ASSERT_STATUS_CODE(response, expected) TF_ASSERT_STATUS_CODE(response, expected)
#define ASSERT_STATUS_CODE_IN(response, allowed) TF_ASSERT_STATUS_CODE_IN(response, allowed)
#define ASSERT_RESPONSE_TIME(response, budgetMs) TF_ASSERT_RESPONSE_TIME(response, budgetMs)
#define ASSERT_JSON_FIELD(document, path) TF_ASSERT_JSON_FIELD(document, path)
#define ASSERT_JSON_EQ(document, path, expected) TF_ASSERT_JSON_EQ(document, path, expected)
#define ASSERT_THROWS(statement, ExceptionType) TF_ASSERT_THROWS(statement, ExceptionType)
#define ASSERT_NO_THROW_TF(statement) TF_ASSERT_NO_THROW(statement)
#define FAIL_TEST(message) TF_FAIL(message)
