// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

// Format decoding only. SQL evaluation belongs to seekdb's native operators.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql {
inline std::string path_utf8(const std::filesystem::path &path)
{
  const auto bytes = path.u8string();
  return std::string(reinterpret_cast<const char *>(bytes.data()),bytes.size());
}
enum class Type : uint8_t { Null, Bigint, Double, Boolean, Varchar, Date, Datetime };
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct Cell {
  std::string text;
  Type type = Type::Null;
};
using Row = std::vector<Cell>;
struct Column {
  std::string name;
  Type type = Type::Null;
  uint32_t width = 0;
};
struct Options {
  std::string format = "csv";
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
  int64_t sample_size = -1;
  uint64_t max_record_bytes = 16 * 1024 * 1024;
  uint64_t max_file_bytes = uint64_t(1) << 40;
  uint32_t max_columns = 4096;
  uint64_t max_metadata_bytes = 16 * 1024 * 1024;
  uint64_t max_page_bytes = 32 * 1024 * 1024;
  void validate() const;
};
// Fingerprint is taken from the open descriptor, never solely from a pathname.
struct Identity {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime = 0, ctime = 0;
  bool operator==(const Identity &o) const;
};
struct Binding {
  std::string path;
  Options options;
  Identity identity;
  std::vector<Column> columns;
  std::string encode() const;
  static Binding decode(const std::string &bytes);
};
Type infer(const std::string &text);
Type widen(Type a, Type b);
bool valid_utf8(const std::string &s);
// Date/time values use SQL's civil calendar and are independent of local TZ.
int64_t temporal_value(const std::string &s, Type type);
const char *type_name(Type type);

class Input {
public:
  Input(const std::string &path, const std::string &secure_root, const Options &options);
  ~Input();
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;
  int get();
  int peek();
  void rewind();
  void check() const;
  void seek(uint64_t offset);
  std::string read(size_t count);
  const Identity &identity() const { return identity_; }
  const std::string &path() const { return path_; }
private:
  FILE *file_ = nullptr;
  std::string path_;
  Identity identity_;
  std::vector<void *> directory_handles_;
};
class IReader {
public:
  virtual ~IReader() = default;
  virtual bool next(Row &row) = 0;
  virtual void rescan() = 0;
  virtual void project(const std::vector<size_t> &) {}
  virtual const std::vector<Column> &columns() const = 0;
};
std::unique_ptr<IReader> make_reader(Input &input, const Options &options);
std::unique_ptr<IReader> make_parquet_reader(Input &input, const Options &options);
Binding bind(const std::string &path, const std::string &secure_root, const Options &options,
             const std::function<void()> &check_status = {});
class Scan {
public:
  Scan(const Binding &binding, const std::string &secure_root);
  bool next(Row &row);
  std::vector<Row> batch(size_t count);
  void rescan();
  void project(const std::vector<size_t> &columns) { reader_->project(columns); }
private:
  Binding binding_;
  Input input_;
  std::unique_ptr<IReader> reader_;
};
std::vector<std::string> discover(const std::string &pattern, const std::string &secure_root,
                                  size_t max_files = 10000);
// Atomic no-replace publication; destruction removes an uncommitted temp file.
class CsvOutput {
public:
  CsvOutput(const std::string &path, const std::string &secure_root, const Options &options);
  ~CsvOutput();
  void row(const Row &row);
  void commit();
private:
  std::string path_, temporary_;
  Options options_;
  FILE *file_ = nullptr;
  int directory_ = -1;
  std::vector<void *> directory_handles_;
};
}}}
