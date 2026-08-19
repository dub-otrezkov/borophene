#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
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

static_assert(!std::is_constructible_v<borophene::StringT, std::string&&>);
static_assert(!std::is_constructible_v<borophene::StringT, const std::string&&>);

namespace {

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

template <typename Exception, typename Function, typename... Arguments>
void CheckThrows(Function&& function, std::string_view message, Arguments&&... arguments) {
  try {
    std::invoke(std::forward<Function>(function), std::forward<Arguments>(arguments)...);
  } catch (const Exception&) {
    return;
  } catch (...) {
    Check(false, "function threw an unexpected exception type");
    return;
  }
  Check(false, message);
}

void ConstructStringWithNullData() {
  [[maybe_unused]] borophene::StringT invalid(nullptr, 1);
}

void ConstructStringFromNullCString() {
  [[maybe_unused]] borophene::StringT invalid(nullptr);
}

void ReadInvalidValidityIndex(const borophene::ValidityMask& mask) {
  (void)mask.IsValid(3);
}

void ReadStringValues(const borophene::Result<borophene::ColumnVector>& column) {
  (void)column->StringValues();
}

void TestStringT() {
  using borophene::StringT;

  const StringT kEmpty(std::string_view{});
  const StringT kInlineLimit("abcdefghijkl", 12);
  const std::string kFirstLong = "abcdefghijklm";
  const std::string kSecondLong = "abcdefghijklm";
  const StringT kBorrowedFirst(kFirstLong);
  const StringT kBorrowedSecond(kSecondLong);

  Check(kEmpty.Empty() && kEmpty.IsInlined() && kEmpty.GetView().empty(), "empty StringT is inline");
  Check(kInlineLimit.IsInlined() && kInlineLimit.GetString() == "abcdefghijkl", "12 bytes remain inline");
  Check(!kBorrowedFirst.IsInlined() && kBorrowedFirst.GetData() == kFirstLong.data(), "13 bytes are borrowed");
  Check(kBorrowedFirst == kBorrowedSecond, "distinct long buffers compare by content");
  Check(StringT("abc") < StringT("abd"), "StringT lexicographic ordering");
  Check(StringT("abc") < StringT("abcd"), "StringT length breaks equal-prefix ties");
  Check(StringT("abcd").GetPrefixIntegerComparable() == 0x61626364U, "prefix integer is byte-comparable");
  CheckThrows<std::invalid_argument>(ConstructStringWithNullData, "null StringT data is rejected");
  CheckThrows<std::invalid_argument>(ConstructStringFromNullCString, "null C strings are rejected");
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
  Check(schema->Find("name") == Result<Index>(1), "schema fields can be found by name");
  Check(!schema->Find("missing") && schema->Find("missing").error().Code() == ErrorCode::kOutOfRange,
        "missing schema fields report out of range");
  Check(ParseLogicalType("INT32") == Result<LogicalType>(LogicalType::kInt32), "logical types parse");
  Check(!ParseLogicalType("FLOAT"), "unknown logical types are rejected");
}

void TestColumnsAndChunks() {
  using namespace borophene;

  ValidityMask mask(3);
  Check(mask.NullCount() == 0, "validity masks start fully valid");
  mask.SetValid(1, false);
  Check(mask.IsValid(0) && !mask.IsValid(1) && mask.NullCount() == 1, "validity bits can be cleared");
  CheckThrows<std::out_of_range>(ReadInvalidValidityIndex, "validity reads are bounds checked", mask);

  Check(!ColumnVector::CreateInt32({1, 2}, ValidityMask(1)), "column and validity sizes must match");
  auto ids = ColumnVector::CreateInt32({1, 2, 3}, mask);
  auto names = ColumnVector::CreateString({"a", "b", "c"});
  Check(ids && ids->Validity().NullCount() == 1 && ids->Int32Values()[2] == 3, "INT32 columns own values");
  Check(names && names->StringValues()[1] == "b", "STRING columns own values");
  CheckThrows<std::logic_error>(ReadStringValues, "wrong typed access is rejected", ids);

  auto schema = Schema::Create({{.name = "id", .type = LogicalType::kInt32, .nullable = true},
                                {.name = "name", .type = LogicalType::kString, .nullable = true}});
  auto chunk = DataChunk::Create(*schema, {std::move(*ids), std::move(*names)});
  Check(chunk && chunk->RowCount() == 3 && chunk->Column(0).Type() == LogicalType::kInt32,
        "valid chunks preserve their columns");

  auto short_ids = ColumnVector::CreateInt32({1});
  auto long_names = ColumnVector::CreateString({"a", "b"});
  Check(!DataChunk::Create(*schema, {std::move(*short_ids), std::move(*long_names)}),
        "chunks reject unequal row counts");
  auto wrong_type = ColumnVector::CreateString({"1"});
  auto one_name = ColumnVector::CreateString({"a"});
  Check(!DataChunk::Create(*schema, {std::move(*wrong_type), std::move(*one_name)}),
        "chunks reject schema type mismatches");

  auto required_schema = Schema::Create({{.name = "id", .type = LogicalType::kInt32, .nullable = false}});
  ValidityMask null_mask(1);
  null_mask.SetValid(0, false);
  auto null_id = ColumnVector::CreateInt32({1}, std::move(null_mask));
  Check(!DataChunk::Create(*required_schema, {std::move(*null_id)}), "chunks reject nulls in non-nullable fields");
}

}  // namespace

int main() {
  try {
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
