#include <iostream>
#include <string_view>
#include <utility>

#include "borophene/common/error.hpp"
#include "borophene/execution/workers/columnar_reader.hpp"
#include "borophene/io/file.hpp"

namespace {

void PrintUsage(std::ostream& output) {
  output << "borophene " << BOROPHENE_VERSION << '\n'
         << "usage: borophene <command> [arguments]\n"
         << "commands:\n"
         << "  inspect <file>  print columnar file metadata\n";
}

void PrintEscapedBytes(std::ostream& output, std::string_view value) {
  constexpr std::string_view kHexDigits = "0123456789ABCDEF";
  for (const unsigned char byte : value) {
    if (byte >= 0x20U && byte <= 0x7EU && byte != static_cast<unsigned char>('\\')) {
      output.put(static_cast<char>(byte));
      continue;
    }
    output << "\\x" << kHexDigits[byte >> 4U] << kHexDigits[byte & 0x0FU];
  }
}

int PrintError(const borophene::Error& error) {
  std::cerr << borophene::ToString(error.Code()) << ": ";
  PrintEscapedBytes(std::cerr, error.Message());
  std::cerr << '\n';
  return 1;
}

int Inspect(std::string_view path) {
  auto input = borophene::io::OpenLocalInput(path);
  if (!input) {
    return PrintError(input.error());
  }

  auto reader = borophene::execution::ColumnarReader::Open(std::move(*input));
  if (!reader) {
    return PrintError(reader.error());
  }

  std::cout << "rows: " << reader->RowCount() << '\n';
  std::cout << "row groups: " << reader->RowGroupCount() << '\n';
  std::cout << "columns: " << reader->GetSchema().Size() << '\n';
  for (const auto& field : reader->GetSchema().Fields()) {
    std::cout << "  ";
    PrintEscapedBytes(std::cout, field.name);
    std::cout << ": " << borophene::ToString(field.type);
    if (field.nullable) {
      std::cout << " nullable";
    }
    std::cout << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))) {
    PrintUsage(std::cout);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << BOROPHENE_VERSION << '\n';
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "inspect") {
    return Inspect(argv[2]);
  }

  std::cerr << "unknown or incomplete command: " << argv[1] << '\n';
  PrintUsage(std::cerr);
  return 2;
}
