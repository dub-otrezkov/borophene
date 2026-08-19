#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "borophene/common/error.hpp"
#include "borophene/common/result.hpp"
#include "borophene/common/string_type.hpp"
#include "borophene/common/types.hpp"
#include "borophene/data/column_vector.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/logical_type.hpp"
#include "borophene/data/schema.hpp"

static_assert(sizeof(borophene::StringT) == 16);
static_assert(std::is_trivially_copyable_v<borophene::StringT>);
static_assert(!std::is_default_constructible_v<borophene::StringT>);
static_assert(!std::is_constructible_v<borophene::StringT, const char*>);

namespace {

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void TestIntegerAliases() {
  using namespace borophene;

  Check(
      (std::is_same_v<i8, std::int8_t> && std::is_same_v<ui8, std::uint8_t> && std::is_same_v<i16, std::int16_t> &&
       std::is_same_v<ui16, std::uint16_t> && std::is_same_v<i32, std::int32_t> &&
       std::is_same_v<ui32, std::uint32_t> && std::is_same_v<i64, std::int64_t> && std::is_same_v<ui64, std::uint64_t>),
      "fixed-width integer aliases map to the standard integer types");
}

void TestResultHelpers() {
  using namespace borophene;

  Result<i32> result = MakeUnexpected(Error(ErrorCode::kInvalidState, "expected failure"));
  Check(!result && result.error().Code() == ErrorCode::kInvalidState && result.error().Message() == "expected failure",
        "MakeUnexpected propagates project errors through Result");
}

void TestStringT() {
  using namespace borophene;

  auto empty = StringT::Create(std::string_view{});
  auto inline_limit = StringT::Create("abcdefghijkl", 12);
  const std::string kFirstLong = "abcdefghijklm";
  const std::string kSecondLong = "abcdefghijklm";
  auto long_first = StringT::Create(kFirstLong);
  auto long_second = StringT::Create(kSecondLong);
  auto abc = StringT::Create("abc");
  auto abd = StringT::Create("abd");
  auto abcd = StringT::Create("abcd");
  const std::string kEmbeddedNull("a\0b", 3);
  auto embedded_null = StringT::Create(std::string_view(kEmbeddedNull));

  Check(empty && empty->Empty() && empty->IsInlined() && empty->GetView().empty(), "empty StringT is inline");
  Check(inline_limit && inline_limit->IsInlined() && inline_limit->GetString() == "abcdefghijkl",
        "12 bytes remain inline");
  Check(long_first && !long_first->IsInlined() && long_first->GetData() == kFirstLong.data(),
        "13 bytes use pointer storage");
  Check(long_first && long_second && *long_first == *long_second, "distinct long buffers compare by content");
  if (long_first) {
    const StringT same_pointer = *long_first;
    Check(*long_first == same_pointer, "equal pointer and length take the equality fast path");
  }
  Check(abc && abd && *abc < *abd, "StringT lexicographic ordering uses byte comparison");
  Check(abc && abcd && *abc < *abcd, "StringT length breaks equal-prefix ties");
  Check(abcd && abcd->GetPrefixIntegerComparable() == 0x61626364U, "prefix integer is byte-comparable");
  Check(embedded_null && embedded_null->GetString() == kEmbeddedNull,
        "GetString copies the complete byte sequence after resizing");

  auto null_data = StringT::Create(nullptr, 1);
  auto null_c_string = StringT::Create(static_cast<const char*>(nullptr));
  auto oversized = StringT::Create("x", static_cast<Index>(std::numeric_limits<ui32>::max()) + 1U);
  Check(!null_data && null_data.error().Code() == ErrorCode::kInvalidArgument,
        "null StringT data reports invalid argument");
  Check(!null_c_string && null_c_string.error().Code() == ErrorCode::kInvalidArgument,
        "null C strings report invalid argument");
  Check(!oversized && oversized.error().Code() == ErrorCode::kOutOfRange,
        "oversized StringT values report out of range");
}

void TestSchema() {
  using namespace borophene;

  Check(!Schema::Create({}), "empty schemas are rejected");
  Check(!Schema::Create({{.name = "", .type = LogicalType::kInt32, .nullable = false}}),
        "empty field names are rejected");
  Check(!Schema::Create({{.name = "id", .type = LogicalType::kInt32, .nullable = false},
                         {.name = "id", .type = LogicalType::kString, .nullable = true}}),
        "duplicate field names are rejected");
  const auto invalid_type = static_cast<LogicalType>(255);  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  Check(!Schema::Create({{.name = "invalid", .type = invalid_type, .nullable = false}}),
        "unsupported logical type values are rejected");

  auto schema = Schema::Create({{.name = "id", .type = LogicalType::kInt32, .nullable = false},
                                {.name = "name", .type = LogicalType::kString, .nullable = true}});
  Check(schema.has_value() && schema->Size() == 2, "valid schemas are created");
  auto first_field = schema->FieldAt(0);
  auto missing_field = schema->FieldAt(schema->Size());
  Check(first_field && first_field->get().name == "id", "schema fields are accessed by status-returning index");
  Check(!missing_field && missing_field.error().Code() == ErrorCode::kOutOfRange,
        "schema field bounds failures report out of range");
  Check(schema->Find("name") == Result<Index>(1), "schema fields can be found by name");
  Check(!schema->Find("missing") && schema->Find("missing").error().Code() == ErrorCode::kOutOfRange,
        "missing schema fields report out of range");
  Check(ParseLogicalType("INT32") == Result<LogicalType>(LogicalType::kInt32), "logical types parse");
  Check(!ParseLogicalType("FLOAT"), "unknown logical types are rejected");
}

void TestColumnsAndChunks() {
  using namespace borophene;

  auto mask = ValidityMask::Create(3);
  Check(mask && mask->NullCount() == 0, "validity masks start fully valid");
  auto cleared = mask->SetValid(1, false);
  auto first_valid = mask->IsValid(0);
  auto second_valid = mask->IsValid(1);
  Check(cleared && first_valid && *first_valid && second_valid && !*second_valid && mask->NullCount() == 1,
        "validity bits can be cleared");
  auto invalid_read = mask->IsValid(3);
  auto invalid_write = mask->SetValid(3, false);
  Check(!invalid_read && invalid_read.error().Code() == ErrorCode::kOutOfRange,
        "validity reads report out-of-range status");
  Check(!invalid_write && invalid_write.error().Code() == ErrorCode::kOutOfRange,
        "validity writes report out-of-range status");

  auto short_mask = ValidityMask::Create(1);
  Check(!ColumnVector::CreateInt32({1, 2}, std::move(*short_mask)), "column and validity sizes must match");
  auto ids = ColumnVector::CreateInt32({1, 2, 3}, std::move(*mask));
  auto names = ColumnVector::CreateString({"a", "b", "c"});
  auto id_values = ids->Int32Values();
  auto name_values = names->StringValues();
  Check(ids && ids->Validity().NullCount() == 1 && id_values && id_values->get()[2] == 3, "INT32 columns own values");
  Check(names && name_values && name_values->get()[1] == "b", "STRING columns own values");
  auto wrong_values = ids->StringValues();
  Check(!wrong_values && wrong_values.error().Code() == ErrorCode::kInvalidState,
        "wrong typed access reports invalid state");

  auto schema = Schema::Create({{.name = "id", .type = LogicalType::kInt32, .nullable = true},
                                {.name = "name", .type = LogicalType::kString, .nullable = true}});
  auto chunk = DataChunk::Create(*schema, {std::move(*ids), std::move(*names)});
  auto first_column = chunk->Column(0);
  auto missing_column = chunk->Column(2);
  Check(chunk && chunk->RowCount() == 3 && first_column && first_column->get().Type() == LogicalType::kInt32,
        "valid chunks preserve their columns");
  Check(!missing_column && missing_column.error().Code() == ErrorCode::kOutOfRange,
        "data-chunk column bounds failures report out of range");

  auto short_ids = ColumnVector::CreateInt32({1});
  auto long_names = ColumnVector::CreateString({"a", "b"});
  Check(!DataChunk::Create(*schema, {std::move(*short_ids), std::move(*long_names)}),
        "chunks reject unequal row counts");
  auto wrong_type = ColumnVector::CreateString({"1"});
  auto one_name = ColumnVector::CreateString({"a"});
  Check(!DataChunk::Create(*schema, {std::move(*wrong_type), std::move(*one_name)}),
        "chunks reject schema type mismatches");

  auto required_schema = Schema::Create({{.name = "id", .type = LogicalType::kInt32, .nullable = false}});
  auto null_mask = ValidityMask::Create(1);
  Check(null_mask->SetValid(0, false).has_value(), "nullable test mask is updated");
  auto null_id = ColumnVector::CreateInt32({1}, std::move(*null_mask));
  Check(!DataChunk::Create(*required_schema, {std::move(*null_id)}), "chunks reject nulls in non-nullable fields");
}

}  // namespace

int main() {
  try {
    TestIntegerAliases();
    TestResultHelpers();
    TestStringT();
    TestSchema();
    TestColumnsAndChunks();
  } catch (const std::exception& exception) {
    std::cerr << "unexpected exception: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "unexpected non-standard exception\n";
    return 1;
  }
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
  }
  return failures == 0 ? 0 : 1;
}
