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

TEST_F(AITest, embedding_indices_restore_input_order) {
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, object(
      R"({"data":[{"index":1,"embedding":[2,3]},{"index":0,"embedding":[0,1]}]})"), result));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(allocator, result, 2, 2));
  auto *first = static_cast<ObJsonArray *>(static_cast<ObJsonArray *>(result)->get_value(0));
  EXPECT_EQ(0, first->get_value(0)->get_int());
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(allocator, result, 2, 3));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(allocator, result, 1, 2));
}

TEST_F(AITest, embedding_invalid_indices_and_shapes) {
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (auto text : {R"({"data":{}})", R"({"data":[]})", R"({"data":[null]})",
       R"({"data":[{"embedding":[1]}]})", R"({"data":[{"index":-1,"embedding":[1]}]})",
       R"({"data":[{"index":1,"embedding":[1]}]})",
       R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})"}) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(text), result)) << text;
  }
}

TEST_F(AITest, embedding_rejects_non_numeric_and_inconsistent_dimensions) {
  for (auto text : {R"({"v":[[1],[1,2]]})", R"({"v":[[]]})", R"({"v":[[true]]})",
       R"({"v":[["1"]]})", R"({"v":[[null]]})", R"({"v":[{}]})"}) {
    ObJsonNode *value = object(text)->get_value("v");
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(allocator, value, value->element_count(), 0));
  }
  auto *value = static_cast<ObJsonArray *>(object(R"({"v":[[1]]})")->get_value("v"));
  auto *vector = static_cast<ObJsonArray *>(value->get_value(0));
  ObJsonDouble *infinity = OB_NEWx(ObJsonDouble, &allocator, std::numeric_limits<double>::infinity());
  ASSERT_NE(nullptr, infinity);
  ASSERT_EQ(OB_SUCCESS, vector->append(infinity));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(allocator, value, 1, 0));
}

TEST_F(AITest, completion_config_and_nonempty_response) {
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("arbitrary-model"), system, content("prompt");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, content,
      object(R"({"temperature":0.4,"max_tokens":20})"), body));
  EXPECT_EQ(20, body->get_value("max_tokens")->get_int());
  for (auto text : {R"({})", R"({"choices":[]})",
       R"({"choices":[{"message":{"content":""}}]})",
       R"({"choices":[{"message":{"content":null}}]})",
       R"({"choices":[{"message":{"content":{}}}]})"}) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(text), result));
  }
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      object(R"({"choices":[{"message":{"content":"ok"}}]})"), result));
}

TEST_F(AITest, ollama_native_protocol_and_empty_key) {
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString("oLlAmA"), provider));
  ObArray<ObString> headers;
  ObString key, model("any-model"), system("system"), content("question");
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator, key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ("Content-Type: application/json", string(headers.at(0)));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator, model, system, content,
      object(R"({"stream":true,"temperature":0.5,"max_tokens":8})"), body));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(8, options->get_value("num_predict")->get_int());
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider->parse_output(allocator, object(R"({"response":"ok"})"), result));
  EXPECT_NE(OB_SUCCESS, provider->parse_output(allocator, object(R"({"response":""})"), result));
  ObOllamaUtils::ObOllamaEmbed embed;
  EXPECT_EQ(OB_SUCCESS, embed.parse_output(allocator, object(R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_NE(OB_SUCCESS, embed.parse_output(allocator, object(R"({"embeddings":[[null]]})"), result));
}

TEST_F(AITest, rerank_stable_order_and_full_permutation) {
  auto *value = object(R"({"v":[{"index":2,"relevance_score":1},{"index":1,"relevance_score":0.5},{"index":0,"relevance_score":1}]})")->get_value("v");
  ObJsonArray *sorted = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_rerank_result(allocator, value, 3, sorted));
  const int expected[] = {0,2,1};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(expected[i], static_cast<ObJsonObject *>(sorted->get_value(i))->get_value("index")->get_int());
  }
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_rerank_result(allocator, value, 4, sorted));
  for (auto text : {R"({"v":[{"index":1,"relevance_score":1}]})",
       R"({"v":[{"index":0,"relevance_score":"1"}]})",
       R"({"v":[{"index":0,"relevance_score":null}]})",
       R"({"v":[{"index":0,"relevance_score":1},{"index":0,"relevance_score":1}]})"}) {
    auto *bad = object(text)->get_value("v");
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_rerank_result(allocator, bad, bad->element_count(), sorted));
  }
}

} // namespace
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
