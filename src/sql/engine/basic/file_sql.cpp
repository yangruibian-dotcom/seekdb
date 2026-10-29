// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "file_sql.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <system_error>
#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include <fast_float/fast_float.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
namespace fs = std::filesystem;
namespace {
[[noreturn]] void fail(const std::string &message) { throw Error("file SQL: " + message); }
std::string canonical(const std::string &s)
{
  if (s.empty() || s.size()>4096 || s.find('\0') != std::string::npos || s.find("://") != std::string::npos)
    fail("expected a local filesystem path");
  std::error_code ec;
  auto raw = fs::u8path(s);
#ifdef _WIN32
  auto absolute = fs::absolute(raw);
  auto drive = path_utf8(absolute.root_name());
  if (drive.size()!=2 || drive[1]!=':' || path_utf8(absolute.relative_path()).find(':')!=std::string::npos)
    fail("device paths, remote paths and alternate data streams are unsupported");
#endif
  auto p = fs::canonical(raw, ec);
  if (ec) fail("cannot resolve path: " + s);
  return path_utf8(p);
}
bool within(const fs::path &path, const fs::path &root)
{
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p) {
    if (p == path.end()) return false;
#ifdef _WIN32
    if (_wcsicmp(p->c_str(), r->c_str()) != 0) return false;
#else
    if (*p != *r) return false;
#endif
  }
  return true;
}
std::string allowed(const std::string &path, const std::string &root)
{
  if (root.empty() || root == "NULL" || root == "null") fail("secure_file_priv denies access");
  auto r = fs::u8path(canonical(root));
  if (!fs::is_directory(r)) fail("secure_file_priv must name a directory");
  auto p = fs::u8path(canonical(path));
  if (!within(p, r)) fail("path is outside secure_file_priv");
  return path_utf8(p);
}
#ifndef _WIN32
// Walk from / with openat, refusing symlinks at every component. The descriptor
// pins each directory so renaming a parent cannot redirect subsequent opens.
int open_directory(const fs::path &p)
{
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) fail("cannot open filesystem root");
  for (const auto &part : p.relative_path()) {
    int next = ::openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(fd);
    if (next < 0) fail("directory changed or is a symlink");
    fd = next;
  }
  return fd;
}
Identity stat_identity(int fd)
{
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) fail("input must be a regular file");
  Identity id;
  id.device = st.st_dev; id.inode = st.st_ino; id.size = st.st_size;
#ifdef __APPLE__
  id.mtime = int64_t(uint64_t(st.st_mtimespec.tv_sec) * 1000000000ULL + st.st_mtimespec.tv_nsec);
  id.ctime = int64_t(uint64_t(st.st_ctimespec.tv_sec) * 1000000000ULL + st.st_ctimespec.tv_nsec);
#else
  id.mtime = int64_t(uint64_t(st.st_mtim.tv_sec) * 1000000000ULL + st.st_mtim.tv_nsec);
  id.ctime = int64_t(uint64_t(st.st_ctim.tv_sec) * 1000000000ULL + st.st_ctim.tv_nsec);
#endif
  return id;
}
#else
void reject_reparse(const fs::path &p)
{
  fs::path current;
  for (const auto &part : p) {
    current /= part;
    DWORD attr = GetFileAttributesW(current.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
      fail("missing path component or reparse point");
  }
}
void release_directories(std::vector<void *> &handles)
{for(auto h:handles)CloseHandle(h);handles.clear();}
void pin_directories(const fs::path &p,std::vector<void *> &handles)
{
  handles.reserve(256);
  try {
    fs::path current=p.root_path();
    auto pin=[&]() {
      if(handles.size()>=256)fail("directory depth limit exceeded");
      HANDLE h=CreateFileW(current.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
      if(h==INVALID_HANDLE_VALUE)fail("cannot pin secure directory");
      BY_HANDLE_FILE_INFORMATION info;
      if(!GetFileInformationByHandle(h,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)
          ||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){CloseHandle(h);fail("directory is a reparse point");}
      handles.push_back(h);
    };
    pin();for(const auto &part:p.relative_path()){current/=part;pin();}
  }catch(...){release_directories(handles);throw;}
}
Identity stat_identity(int fd)
{
  BY_HANDLE_FILE_INFORMATION info;
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      || GetFileType(h) != FILE_TYPE_DISK) fail("input must be a regular file");
  Identity id;
  id.device = info.dwVolumeSerialNumber;
  id.inode = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  id.size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  id.mtime = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
  FILE_BASIC_INFO basic;
  if(!GetFileInformationByHandleEx(h,FileBasicInfo,&basic,sizeof(basic)))fail("cannot read file change time");
  id.ctime = basic.ChangeTime.QuadPart;
  return id;
}
#endif
bool digits(const std::string &s, size_t pos, size_t count)
{
  if (pos + count > s.size()) return false;
  for (size_t i = pos; i < pos + count; ++i) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}
int number(const std::string &s, size_t pos, size_t n)
{
  int v = 0; for (size_t i = pos; i < pos + n; ++i) v = v * 10 + s[i] - '0'; return v;
}
bool date_ok(const std::string &s)
{
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !digits(s,0,4) || !digits(s,5,2)
      || !digits(s,8,2)) return false;
  int y=number(s,0,4), m=number(s,5,2), d=number(s,8,2);
  static const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  return y >= 1 && m >= 1 && m <= 12 && d >= 1
      && d <= days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
bool time_ok(const std::string &s)
{
  if (s.size() < 19 || s.size() > 26 || (s[10] != ' ' && s[10] != 'T') || s[13] != ':'
      || s[16] != ':' || !digits(s,11,2) || !digits(s,14,2) || !digits(s,17,2)) return false;
  if (s.size() > 19 && (s[19] != '.' || s.size() == 20 || !digits(s,20,s.size()-20))) return false;
  return number(s,11,2)<24 && number(s,14,2)<60 && number(s,17,2)<60;
}
bool integer_lexeme(const std::string &s)
{
  size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
  return i < s.size() && digits(s,i,s.size()-i);
}
bool float_lexeme(const std::string &s)
{
  size_t i = (!s.empty() && (s[0]=='-' || s[0]=='+')) ? 1 : 0;
  size_t before=i; while (i<s.size() && s[i]>='0' && s[i]<='9') ++i;
  bool have = i>before;
  if (i<s.size() && s[i]=='.') { before=++i; while(i<s.size() && s[i]>='0' && s[i]<='9') ++i; have |= i>before; }
  if (!have) return false;
  if (i<s.size() && (s[i]=='e'||s[i]=='E')) {
    ++i; if(i<s.size() && (s[i]=='+'||s[i]=='-')) ++i;
    before=i; while(i<s.size() && s[i]>='0' && s[i]<='9') ++i;
    if(i==before) return false;
  }
  return i==s.size();
}
void names_valid(const std::vector<Column> &cols)
{
  std::set<std::string> names;
  for (const auto &c : cols) {
    if (c.name.empty() || c.name.size()>256 || c.name.find('\0')!=std::string::npos || !valid_utf8(c.name))
      fail("empty, overlong or invalid column name");
    std::string key=c.name;
    for(char &ch:key) if(ch>='A' && ch<='Z') ch+= 'a'-'A';
    if(!names.insert(key).second) fail("duplicate column name: " + c.name);
  }
}
struct FlatJson : rapidjson::BaseReaderHandler<rapidjson::UTF8<>,FlatJson> {
  std::vector<std::pair<std::string,Cell>> fields;
  std::set<std::string> names;
  std::string key,diagnostic;
  bool started=false,ended=false;
  uint32_t limit;
  explicit FlatJson(uint32_t n):limit(n){}
  bool reject(const char *message){diagnostic=message;return false;}
  bool StartObject(){if(started)return reject("nested objects are unsupported");started=true;return true;}
  bool EndObject(rapidjson::SizeType){ended=true;return true;}
  bool StartArray(){return reject("arrays are unsupported");}
  bool Key(const char *s,rapidjson::SizeType n,bool){key.assign(s,n);return true;}
  bool add(Cell value){
    if(!started||ended)return reject("JSONL record must be an object");
    if(fields.size()>=limit)return reject("column limit exceeded");
    if(!names.insert(key).second)return reject("duplicate JSON key");
    if(value.text.find('\0')!=std::string::npos)return reject("NUL byte in JSON string");
    fields.emplace_back(key,std::move(value));return true;
  }
  bool Null(){return add(Cell{});}
  bool Bool(bool b){return add({b?"true":"false",Type::Boolean});}
  bool String(const char *s,rapidjson::SizeType n,bool){
    std::string text(s,n);Type t=infer(text);
    return add({std::move(text),(t==Type::Date||t==Type::Datetime)?t:Type::Varchar});
  }
  bool RawNumber(const char *s,rapidjson::SizeType n,bool){
    std::string text(s,n);Type t=infer(text);return add({std::move(text),t});
  }
};
class TextReader final : public IReader {
  Input &in_; Options opt_; std::vector<Column> cols_; uint64_t line_=1, record_line_=1, record_=0;
  bool json_;
  [[noreturn]] void bad(const std::string &s) const {
    fail(in_.path()+": record "+std::to_string(record_)+", physical line "+std::to_string(record_line_)+": "+s);
  }
  bool csv(Row &out) {
    out.clear(); if(in_.peek()==EOF) return false;
    record_line_=line_;++record_; uint64_t bytes=0;
    for(;;) {
      Cell c; bool quoted=false, closed=false;
      if(in_.peek()==opt_.quote) { quoted=true; in_.get(); ++bytes; }
      bool end=false;
      for(;;) {
        int ch=in_.get(); if(ch!=EOF && ++bytes>opt_.max_record_bytes) bad("record size limit exceeded");
        if(ch==EOF) { if(quoted && !closed) bad("unterminated quoted field"); end=true; break; }
        if(quoted && !closed) {
          if(ch==opt_.quote) {
            if(opt_.escape==opt_.quote && in_.peek()==opt_.quote) {in_.get(); ++bytes; c.text+=opt_.quote;}
            else closed=true;
          } else if(ch==opt_.escape && opt_.escape!=opt_.quote) {
            int next=in_.get(); ++bytes; if(next==EOF) bad("trailing escape");
            c.text+=char(next); if(next=='\n') ++line_;
          } else { c.text+=char(ch); if(ch=='\n') ++line_; }
        } else if(ch==opt_.delimiter) break;
        else if(ch=='\n' || ch=='\r') {
          if(ch=='\r' && in_.peek()=='\n') {in_.get(); ++bytes;}
          ++line_; end=true; break;
        } else {
          if(closed) bad("characters after closing quote");
          if(ch==opt_.quote) bad("quote inside unquoted field");
          c.text+=char(ch);
        }
      }
      if(bytes>opt_.max_record_bytes) bad("record size limit exceeded");
      if(!valid_utf8(c.text) || c.text.find('\0')!=std::string::npos) bad("invalid UTF-8 or NUL byte");
      c.type=(!quoted && c.text==opt_.nullstr) ? Type::Null : infer(c.text);
      out.push_back(std::move(c));
      if(out.size()>opt_.max_columns) bad("column limit exceeded");
      if(end) return true;
    }
  }
  bool json(Row &out) {
    std::string line;
    for(;;) {
      if(in_.peek()==EOF) return false;
      record_line_=line_;++record_; line.clear();
      for(int ch=in_.get(); ch!=EOF && ch!='\n'; ch=in_.get()) {
        line+=char(ch); if(line.size()>opt_.max_record_bytes) bad("record size limit exceeded");
      }
      ++line_;
      if(line.find_first_not_of(" \t\r")==std::string::npos) continue;
      break;
    }
    rapidjson::MemoryStream stream(line.data(),line.size());
    rapidjson::Reader reader;FlatJson handler(opt_.max_columns);
    auto parsed=reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag>(stream,handler);
    if(!parsed)bad((handler.diagnostic.empty()?"invalid JSON":handler.diagnostic)+" at byte "+std::to_string(parsed.Offset()));
    if(!handler.started||!handler.ended||stream.Tell()!=line.size())bad("JSONL record must be a complete object");
    std::map<std::string,Cell> values;
    for(auto &field:handler.fields) {
      const std::string &name=field.first;
      values.emplace(name,std::move(field.second));
      auto col=std::find_if(cols_.begin(),cols_.end(),[&](const Column &c){return c.name==name;});
      if(col==cols_.end()) {
        if(cols_.size()>=opt_.max_columns) bad("column limit exceeded");
        cols_.push_back({name,Type::Null,0}); names_valid(cols_);
      }
    }
    out.assign(cols_.size(),Cell{});
    for(size_t i=0;i<cols_.size();++i) {
      auto v=values.find(cols_[i].name); if(v!=values.end()) out[i]=std::move(v->second);
    }
    return true;
  }
public:
  TextReader(Input &in, const Options &opt):in_(in),opt_(opt),json_(opt.format!="csv") {rescan();}
  bool next(Row &out) override {
    in_.check();
    bool found=json_?json(out):csv(out);
    if(found && !json_) {
      if(cols_.empty()) { for(size_t i=0;i<out.size();++i) cols_.push_back({"column"+std::to_string(i+1),Type::Null,0}); }
      if(out.size()!=cols_.size()) bad("expected "+std::to_string(cols_.size())+" fields, got "+std::to_string(out.size()));
    }
    in_.check(); return found;
  }
  void rescan() override {
    in_.rewind(); line_=1; record_line_=1; record_=0;
    if(in_.identity().size>=3 && in_.peek()==0xef) {
      if(in_.read(3)!="\xef\xbb\xbf")in_.seek(0);
    }
    if(!json_ && opt_.header) {
      Row header;
      if(csv(header)) {
        std::vector<Column> cols;
        // Reserve original names before suffixing duplicates so a later id__2
        // cannot collide with the generated name of an earlier duplicate id.
        auto key=[](std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+='a'-'A';return s;};
        std::set<std::string> reserved, used;
        for(const auto &c:header) reserved.insert(key(c.text));
        for(auto &c:header) {
          std::string name=c.text;
          if(used.count(key(name))) {
            unsigned suffix=2;
            do {name=c.text+"__"+std::to_string(suffix++);}
            while(reserved.count(key(name))||used.count(key(name)));
          }
          used.insert(key(name));cols.push_back({std::move(name),Type::Null,0});
        }
        names_valid(cols);
        if(!cols_.empty()) {
          if(cols.size()!=cols_.size()) bad("header changed");
          for(size_t i=0;i<cols.size();++i) if(cols[i].name!=cols_[i].name) bad("header changed");
        } else cols_=std::move(cols);
      }
    }
  }
  const std::vector<Column> &columns() const override {return cols_;}
};
}
void Options::validate() const
{
  if(format!="csv" && format!="json" && format!="ndjson" && format!="parquet" && format!="glob") fail("unsupported format");
  if(sample_size!=-1) fail("sample_size must be -1 (full schema inference)");
  if(!delimiter || !quote || !escape || delimiter==quote || delimiter=='\r' || delimiter=='\n'
      || quote=='\r'||quote=='\n'||escape=='\r'||escape=='\n'
      || uint8_t(delimiter)>=128||uint8_t(quote)>=128||uint8_t(escape)>=128) fail("invalid CSV delimiter, quote or escape");
  if(!max_record_bytes||max_record_bytes>64*1024*1024||!max_columns||max_columns>4096) fail("invalid resource limit");
  if(!max_file_bytes||max_file_bytes>(uint64_t(1)<<40)||!max_metadata_bytes||max_metadata_bytes>16*1024*1024
      ||!max_page_bytes||max_page_bytes>32*1024*1024||nullstr.size()>max_record_bytes)fail("invalid file or metadata resource limit");
  if(nullstr.find_first_of(std::string()+delimiter+quote+'\r'+'\n')!=std::string::npos
      || nullstr.find('\0')!=std::string::npos || !valid_utf8(nullstr)) fail("NULL text cannot contain CSV structural characters");
}
bool Identity::operator==(const Identity &o) const
{return device==o.device && inode==o.inode && size==o.size && mtime==o.mtime && ctime==o.ctime;}
bool valid_utf8(const std::string &s)
{
  for(size_t i=0;i<s.size();) {
    unsigned c=static_cast<unsigned char>(s[i++]); if(c<128) continue;
    int n; uint32_t v,min;
    if(c>=0xc2&&c<=0xdf){n=1;v=c&31;min=0x80;}
    else if(c>=0xe0&&c<=0xef){n=2;v=c&15;min=0x800;}
    else if(c>=0xf0&&c<=0xf4){n=3;v=c&7;min=0x10000;}
    else return false;
    if(i+n>s.size()) return false;
    while(n--){unsigned b=static_cast<unsigned char>(s[i++]);if((b&192)!=128)return false;v=(v<<6)|(b&63);}
    if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return false;
  }return true;
}
Type infer(const std::string &s)
{
  auto equal_ascii=[&](const char *word){
    size_t n=std::strlen(word);if(s.size()!=n)return false;
    for(size_t i=0;i<n;++i){char c=s[i];if(c>='A'&&c<='Z')c+='a'-'A';if(c!=word[i])return false;}
    return true;
  };
  if(equal_ascii("true")||equal_ascii("false"))return Type::Boolean;
  if(integer_lexeme(s)) {
    int64_t value; const char *begin=s.data()+(s[0]=='+');
    auto r=std::from_chars(begin,s.data()+s.size(),value);
    return r.ec==std::errc() ? Type::Bigint : Type::Varchar;
  }
  if(float_lexeme(s)) {
    double value; const char *begin=s.data()+(!s.empty()&&s[0]=='+');
    auto r=fast_float::from_chars(begin,s.data()+s.size(),value);
    if(r.ec==std::errc() && std::isfinite(value))return Type::Double;
  }
  if(date_ok(s)) {if(s.size()==10)return Type::Date;if(time_ok(s))return Type::Datetime;}
  return Type::Varchar;
}
Type widen(Type a, Type b)
{
  if(a==Type::Null)return b;if(b==Type::Null||a==b)return a;
  if((a==Type::Bigint&&b==Type::Double)||(a==Type::Double&&b==Type::Bigint))return Type::Double;
  if((a==Type::Date&&b==Type::Datetime)||(a==Type::Datetime&&b==Type::Date))return Type::Datetime;
  return Type::Varchar;
}
const char *type_name(Type t)
{static const char *names[]={"NULL","BIGINT","DOUBLE","BOOLEAN","VARCHAR","DATE","DATETIME"};return names[static_cast<int>(t)];}
int64_t temporal_value(const std::string &s, Type t)
{
  if(!date_ok(s) || (s.size()!=10&&!time_ok(s)))fail("invalid date/time");
  int y=number(s,0,4),m=number(s,5,2),d=number(s,8,2); y-=m<=2;
  const int era=(y>=0?y:y-399)/400; unsigned yoe=y-era*400;
  unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
  int64_t days=int64_t(era)*146097+yoe*365+yoe/4-yoe/100+doy-719468;
  if(t==Type::Date)return days;
  int64_t micros=days*86400000000LL;
  if(s.size()>10) {
    micros+=(number(s,11,2)*3600LL+number(s,14,2)*60+number(s,17,2))*1000000;
    int frac=0;for(size_t i=20;i<26;++i)frac=frac*10+(i<s.size()?s[i]-'0':0);micros+=frac;
  }return micros;
}
Input::Input(const std::string &path,const std::string &root,const Options &opt)
{
  opt.validate();path_=allowed(path,root);
#ifdef _WIN32
  pin_directories(fs::u8path(path_).parent_path(),directory_handles_);
  try {
  reject_reparse(fs::u8path(path_));
  HANDLE h=CreateFileW(fs::u8path(path_).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(h==INVALID_HANDLE_VALUE)fail("cannot open read-only file");
  int fd=_open_osfhandle(reinterpret_cast<intptr_t>(h),_O_RDONLY|_O_BINARY);
  if(fd<0){CloseHandle(h);fail("cannot create file descriptor");}
  file_=_fdopen(fd,"rb");if(!file_){_close(fd);fail("cannot open stream");}
  }catch(...){release_directories(directory_handles_);throw;}
#else
  auto p=fs::u8path(path_);int parent=open_directory(p.parent_path());
  int fd=::openat(parent,p.filename().c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);::close(parent);
  if(fd<0)fail("cannot open read-only file");
  file_=fdopen(fd,"rb");if(!file_){::close(fd);fail("cannot open stream");}
#endif
  try {identity_=stat_identity(fileno(file_));if(identity_.size>opt.max_file_bytes)fail("file size limit exceeded");check();}
  catch(...){fclose(file_);file_=nullptr;
#ifdef _WIN32
    release_directories(directory_handles_);
#endif
    throw;}
}
Input::~Input(){if(file_)fclose(file_);
#ifdef _WIN32
  release_directories(directory_handles_);
#endif
}
void Input::check() const
{
  if(!(stat_identity(fileno(file_))==identity_))fail("file changed during query: "+path_);
#ifndef _WIN32
  struct stat st;
  if(::lstat(path_.c_str(),&st)!=0||!S_ISREG(st.st_mode)||uint64_t(st.st_dev)!=identity_.device
      ||uint64_t(st.st_ino)!=identity_.inode)fail("file replaced during query: "+path_);
#endif
}
int Input::get(){int c=std::fgetc(file_);if(c==EOF&&ferror(file_))fail("file read failed");return c;}
int Input::peek(){int c=get();if(c!=EOF)ungetc(c,file_);return c;}
void Input::seek(uint64_t pos){
  if(pos>identity_.size)fail("file offset outside file");
#ifdef _WIN32
  if(_fseeki64(file_,pos,SEEK_SET)!=0)fail("seek failed");
#else
  if(fseeko(file_,pos,SEEK_SET)!=0)fail("seek failed");
#endif
}
void Input::rewind(){check();seek(0);clearerr(file_);}
std::string Input::read(size_t n){std::string out(n,'\0');if(fread(out.data(),1,n,file_)!=n)fail("truncated file");return out;}
std::unique_ptr<IReader> make_reader(Input &input,const Options &options)
{if(options.format=="parquet")return make_parquet_reader(input,options);return std::make_unique<TextReader>(input,options);}
Binding bind(const std::string &path,const std::string &root,const Options &opt,const std::function<void()> &status)
{
  Input input(path,root,opt);Binding b;b.path=input.path();b.options=opt;b.identity=input.identity();
  auto reader=make_reader(input,opt);Row row;uint64_t count=0;
  if(opt.format=="parquet") b.columns=reader->columns();
  else {
    while(reader->next(row)) {
      ++count;
      if(status && (count%256)==0)status();
      auto &cols=reader->columns();
      while(b.columns.size()<cols.size()) {
        b.columns.push_back(cols[b.columns.size()]);
        b.columns.back().nullable=count>1;
      }
      for(size_t i=0;i<row.size();++i) {
        b.columns[i].type=widen(b.columns[i].type,row[i].type);
        b.columns[i].nullable|=row[i].type==Type::Null;
        b.columns[i].width=std::max(b.columns[i].width,static_cast<uint32_t>(row[i].text.size()));
      }
    }
    if(b.columns.empty())b.columns=reader->columns();
  }
  if(b.columns.empty())fail("file has no columns");
  for(auto &column:b.columns)if(column.type==Type::Null)column.nullable=true;
  names_valid(b.columns);input.check();return b;
}
Scan::Scan(const Binding &b,const std::string &root):binding_(b),input_(b.path,root,b.options)
{
  if(!(input_.identity()==b.identity))fail("file changed since schema inference");
  reader_=make_reader(input_,b.options);
}
bool Scan::next(Row &row)
{
  input_.check();bool found=reader_->next(row);input_.check();
  if(found) {
    if(row.size()>binding_.columns.size())fail("schema changed");
    row.resize(binding_.columns.size());
    // JSON keys encountered in later records have to map to the binding's union,
    // not the order first encountered by this execution's reader.
    if(binding_.options.format=="json"||binding_.options.format=="ndjson") {
      Row mapped(binding_.columns.size());auto &cols=reader_->columns();
      for(size_t i=0;i<cols.size();++i) {
        auto it=std::find_if(binding_.columns.begin(),binding_.columns.end(),[&](const Column &c){return c.name==cols[i].name;});
        if(it==binding_.columns.end())fail("JSON schema changed");mapped[it-binding_.columns.begin()]=std::move(row[i]);
      }row=std::move(mapped);
    }
  }return found;
}
std::vector<Row> Scan::batch(size_t n)
{
  if(n>4096)fail("batch row limit exceeded");
  std::vector<Row> rows;Row row;uint64_t bytes=0;
  while(rows.size()<n&&next(row)){
    bytes+=row.capacity()*sizeof(Cell);for(const auto &cell:row)bytes+=cell.text.capacity();
    rows.push_back(std::move(row));
    if(bytes>=8*1024*1024)break;
  }return rows;
}
void Scan::rescan(){input_.check();reader_->rescan();}

namespace {
void put(std::string &s,const std::string &v){uint64_t n=v.size();for(int i=0;i<8;++i)s+=char(n>>(i*8));s+=v;}
std::string take(const std::string &s,size_t &p){if(s.size()-p<8)fail("invalid binding");uint64_t n=0;for(int i=0;i<8;++i)n|=uint64_t(uint8_t(s[p++]))<<(i*8);if(n>s.size()-p)fail("invalid binding length");auto v=s.substr(p,n);p+=n;return v;}
uint64_t take_num(const std::string &s,size_t &p){auto v=take(s,p);uint64_t n;auto r=std::from_chars(v.data(),v.data()+v.size(),n);if(r.ec!=std::errc()||r.ptr!=v.data()+v.size())fail("invalid binding integer");return n;}
}
std::string Binding::encode() const
{
  std::string s="SEEKDB_FILE_SQL_2";put(s,path);put(s,options.format);put(s,options.header?"1":"0");
  put(s,std::string(1,options.delimiter));put(s,std::string(1,options.quote));put(s,std::string(1,options.escape));put(s,options.nullstr);
  for(uint64_t v:{identity.device,identity.inode,identity.size,uint64_t(identity.mtime),uint64_t(identity.ctime),
      options.max_record_bytes,options.max_file_bytes,uint64_t(options.max_columns),options.max_metadata_bytes,options.max_page_bytes,uint64_t(columns.size())})put(s,std::to_string(v));
  for(auto &c:columns){put(s,c.name);put(s,std::to_string(int(c.type)));put(s,std::to_string(c.width));put(s,c.nullable?"1":"0");}return s;
}
Binding Binding::decode(const std::string &s)
{
  if(s.size()>32*1024*1024||s.compare(0,17,"SEEKDB_FILE_SQL_2")!=0)fail("invalid binding signature");
  size_t p=17;Binding b;b.path=take(s,p);b.options.format=take(s,p);b.options.header=take(s,p)=="1";
  auto ch=[&](){auto v=take(s,p);if(v.size()!=1)fail("invalid CSV character");return v[0];};
  b.options.delimiter=ch();b.options.quote=ch();b.options.escape=ch();b.options.nullstr=take(s,p);
  b.identity.device=take_num(s,p);b.identity.inode=take_num(s,p);b.identity.size=take_num(s,p);
  b.identity.mtime=int64_t(take_num(s,p));b.identity.ctime=int64_t(take_num(s,p));
  b.options.max_record_bytes=take_num(s,p);b.options.max_file_bytes=take_num(s,p);b.options.max_columns=take_num(s,p);
  b.options.max_metadata_bytes=take_num(s,p);b.options.max_page_bytes=take_num(s,p);auto n=take_num(s,p);
  if(n>4096)fail("binding column limit exceeded");
  for(uint64_t i=0;i<n;++i){Column c;c.name=take(s,p);auto t=take_num(s,p);if(t>6)fail("invalid column type");c.type=Type(t);c.width=take_num(s,p);auto nullable=take_num(s,p);if(nullable>1)fail("invalid nullability");c.nullable=nullable;b.columns.push_back(std::move(c));}
  if(p!=s.size())fail("trailing binding bytes");b.options.validate();names_valid(b.columns);return b;
}
namespace {
bool match(const std::string &p,const std::string &s)
{
  size_t i=0,j=0,star=std::string::npos,saved=0;
  while(j<s.size()) {
    bool matched=false;size_t next=i+1;
    if(i<p.size() && p[i]=='[') {
      size_t end=p.find(']',i+1);if(end==std::string::npos)fail("unterminated glob character class");
      size_t at=i+1;bool neg=at<end&&(p[at]=='!'||p[at]=='^');if(neg)++at;
      bool member=false;
      while(at<end){char first=p[at++];if(at+1<end&&p[at]=='-'){++at;char last=p[at++];member|=s[j]>=first&&s[j]<=last;}else member|=s[j]==first;}
      matched=neg?!member:member;next=end+1;
    }else if(i<p.size()&&(p[i]=='?'||p[i]==s[j]))matched=true;
    if(matched){i=next;++j;}
    else if(i<p.size()&&p[i]=='*'){star=i++;saved=j;}
    else if(star!=std::string::npos){i=star+1;j=++saved;}
    else return false;
  }while(i<p.size()&&p[i]=='*')++i;return i==p.size();
}
}
std::vector<std::string> discover(const std::string &pattern,const std::string &root,size_t limit)
{
  if(pattern.empty()||pattern.find('\0')!=std::string::npos||pattern.find("://")!=std::string::npos)fail("invalid glob path");
  auto p=fs::absolute(fs::u8path(pattern));fs::path prefix=p.root_path();
  std::vector<std::string> parts;bool wildcard=false;
  for(const auto &part:p.relative_path()) {
    auto s=path_utf8(part);
    if(s=="**")fail("recursive glob is unsupported");
    if(s.find_first_of("*?[")!=std::string::npos)wildcard=true;
    if(wildcard)parts.push_back(s);else prefix/=part;
  }
  if(parts.empty()){parts.push_back(path_utf8(prefix.filename()));prefix=prefix.parent_path();}
  std::vector<std::string> dirs={allowed(path_utf8(prefix),root)},files;
  for(size_t depth=0;depth<parts.size();++depth) {
    std::vector<std::string> next;
    for(auto &dir:dirs) {
      std::error_code ec;fs::directory_iterator iter(fs::u8path(allowed(dir,root)),ec),end;
      if(ec)fail("cannot list glob directory");
      for(;iter!=end;iter.increment(ec)) {
        if(ec)fail("glob directory changed");
        auto leaf=path_utf8(iter->path().filename());
        if(!leaf.empty()&&leaf[0]=='.'&&(parts[depth].empty()||parts[depth][0]!='.'))continue;
        if(!match(parts[depth],leaf))continue;
        auto status=iter->symlink_status(ec);if(ec)fail("glob entry changed");
        if(fs::is_symlink(status))fail("glob matched a symlink");
        if(depth+1==parts.size()) {
          if(!fs::is_regular_file(status))continue;
          Input check(path_utf8(iter->path()),root,Options{});
          if(files.size()>=limit)fail("glob result limit exceeded");files.push_back(check.path());
        }else if(fs::is_directory(status)){
          if(next.size()>=limit)fail("glob directory limit exceeded");next.push_back(allowed(path_utf8(iter->path()),root));
        }
      }if(ec)fail("glob directory changed");
    }dirs=std::move(next);
  }
  std::sort(files.begin(),files.end());return files;
}
CsvOutput::CsvOutput(const std::string &path,const std::string &root,const Options &opt):options_(opt)
{
  opt.validate();if(path.empty()||path.find('\0')!=std::string::npos)fail("invalid output path");
  auto p=fs::absolute(fs::u8path(path)).lexically_normal();
#ifdef _WIN32
  if(path_utf8(p.root_name()).size()!=2 || path_utf8(p.relative_path()).find(':')!=std::string::npos)
    fail("device paths, remote paths and alternate data streams are unsupported");
#endif
  std::string parent=allowed(path_utf8(p.parent_path()),root);path_=path_utf8((fs::u8path(parent)/p.filename()));
  std::error_code ec;if(fs::exists(fs::symlink_status(fs::u8path(path_),ec)))fail("output already exists");
#ifdef _WIN32
  pin_directories(fs::u8path(parent),directory_handles_);
  try {
  for(unsigned i=0;i<100;++i) {
    temporary_=path_+".seekdb-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(i);
    HANDLE h=CreateFileW(fs::u8path(temporary_).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)continue;
    int fd=_open_osfhandle(reinterpret_cast<intptr_t>(h),_O_WRONLY|_O_BINARY);
    if(fd<0){CloseHandle(h);DeleteFileW(fs::u8path(temporary_).c_str());fail("cannot create output descriptor");}
    file_=_fdopen(fd,"wb");if(!file_){_close(fd);DeleteFileW(fs::u8path(temporary_).c_str());fail("cannot create output stream");}break;
  }
  }catch(...){release_directories(directory_handles_);throw;}
#else
  directory_=open_directory(fs::u8path(parent));
  try {
  for(unsigned i=0;i<100;++i) {
    temporary_=path_utf8(p.filename())+".seekdb-"+std::to_string(getpid())+"-"+std::to_string(i);
    int fd=openat(directory_,temporary_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)continue;
    file_=fdopen(fd,"wb");if(!file_){close(fd);unlinkat(directory_,temporary_.c_str(),0);close(directory_);directory_=-1;fail("cannot create output stream");}break;
  }
  }catch(...){if(directory_>=0)close(directory_);directory_=-1;throw;}
#endif
  if(!file_){
#ifdef _WIN32
    release_directories(directory_handles_);
#else
    close(directory_);directory_=-1;
#endif
    fail("cannot create temporary output");
  }
}
CsvOutput::~CsvOutput()
{
  if(file_)fclose(file_);
#ifdef _WIN32
  if(!temporary_.empty())DeleteFileW(fs::u8path(temporary_).c_str());
  release_directories(directory_handles_);
#else
  if(directory_>=0){if(!temporary_.empty())unlinkat(directory_,temporary_.c_str(),0);close(directory_);}
#endif
}
void CsvOutput::row(const Row &r)
{
  std::string line;
  for(size_t i=0;i<r.size();++i) {
    if(i)line+=options_.delimiter;auto &c=r[i];
    if(c.type==Type::Null)line+=options_.nullstr;
    else {
      line+=options_.quote;
      for(char ch:c.text){if(ch==options_.quote||ch==options_.escape)line+=options_.escape;line+=ch;}
      line+=options_.quote;
    }
    if(line.size()>options_.max_record_bytes)fail("output record limit exceeded");
  }line+='\n';if(fwrite(line.data(),1,line.size(),file_)!=line.size())fail("output write failed");
}
void CsvOutput::commit()
{
  if(!file_)fail("output already committed");if(fflush(file_)!=0)fail("output flush failed");
#ifdef _WIN32
  if(_commit(_fileno(file_))!=0)fail("output sync failed");
#else
  if(fsync(fileno(file_))!=0)fail("output sync failed");
#endif
  int rc=fclose(file_);file_=nullptr;if(rc!=0)fail("output close failed");
#ifdef _WIN32
  if(!MoveFileW(fs::u8path(temporary_).c_str(),fs::u8path(path_).c_str()))fail("output exists or cannot be published");
#else
  auto leaf=path_utf8(fs::u8path(path_).filename());
  if(linkat(directory_,temporary_.c_str(),directory_,leaf.c_str(),0)!=0)fail("output exists or cannot be published");
  unlinkat(directory_,temporary_.c_str(),0);
#endif
  temporary_.clear();
}
}}}
