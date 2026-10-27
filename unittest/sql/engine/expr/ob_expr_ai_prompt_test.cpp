#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <string>
#include <limits>
using namespace oceanbase::common;

namespace {
class AITest : public ::testing::Test {
protected:
  ObArenaAllocator allocator;
  ObJsonObject *object(const char *text) {
    ObString input(text);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, result));
    return result;
  }
  std::string string(const ObString &value) { return std::string(value.ptr(), value.length()); }
};

TEST_F(AITest, repeated_placeholder_and_no_recursive_substitution) {
  auto *prompt = object(R"({"template":"{0}/{0}/{1}","args":["a very long repeated argument", "{0}"]})");
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("a very long repeated argument/a very long repeated argument/{0}", string(result));
}

TEST_F(AITest, json_array_object_nested_prompt_and_unicode) {
  auto *prompt = object(R"({"template":"你好:{0}|{1}|{2}","args":[[1,"雪"],{"x":true},{"template":"<{0}>","args":["line\n\"quote\""]}]})");
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("你好:[1, \"雪\"]|{\"x\": true}|<line\n\"quote\">", string(result));
}

TEST_F(AITest, index_bounds_overflow_and_literal_braces) {
  for (auto text : {R"({"template":"{1}","args":["x"]})",
                    R"({"template":"{-1}","args":["x"]})",
                    R"({"template":"{184467440737095516160}","args":["x"]})"}) {
    ObString result;
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object(text), result));
  }
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator, object(R"({"template":"{name} {} {0","args":[]})"), result));
  EXPECT_EQ("{name} {} {0", string(result));
}

TEST_F(AITest, long_repeated_values) {
  std::string argument(100000, 'x');
  ObJsonArray *args = nullptr;
  ObJsonString *value = nullptr;
  ObString input(static_cast<int32_t>(argument.size()), argument.data());
  ObString text("{0}{0}{0}");
  ObJsonObject *prompt = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator, input, value));
  ASSERT_EQ(OB_SUCCESS, args->append(value));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, text, args, prompt));
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ(argument + argument + argument, string(result));
}

TEST_F(AITest, rejects_malformed_prompt_object) {
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator, object(R"({"template":1,"args":[]})"), result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator, object(R"({"template":"a","args":{}})"), result));
}

} // namespace
