// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/engine/basic/file_sql.h"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <functional>
#include <random>
using namespace oceanbase::sql::file_sql;
namespace fs=std::filesystem;
int checks=0;
void check(bool b,const char *what){++checks;if(!b)throw std::runtime_error(what);}
void fails(const std::function<void()> &f,const char *text=""){
  bool failed=false;try{f();}catch(const Error &e){failed=true;check(std::string(e.what()).find(text)!=std::string::npos,"diagnostic mismatch");}
  check(failed,"expected failure");
}
void write(const fs::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;if(!f)throw std::runtime_error("write fixture failed");}
int run_file_scan_candidate(int argc,char **argv){
 try{
  if(argc==4 || argc==5){
    Options o;o.format=argv[2];auto b=oceanbase::sql::file_sql::bind(argv[1],argv[3],o);Scan scan(b,argv[3]);Row row;
    if(argc==5) { std::vector<size_t> columns; std::string list=argv[4]; size_t at=0; while(at<list.size()){size_t end=list.find(',',at);columns.push_back(std::stoul(list.substr(at,end-at)));if(end==std::string::npos)break;at=end+1;} scan.project(columns); }
    for(auto &c:b.columns)std::cout<<c.name<<":"<<type_name(c.type)<<"\t";std::cout<<"\n";
    while(scan.next(row)){for(auto &v:row)std::cout<<(v.type==Type::Null?"<NULL>":v.text)<<"\t";std::cout<<"\n";}return 0;
  }
  fs::path root=fs::temp_directory_path()/("seekdb-file-test-"+std::to_string(std::random_device{}()));fs::create_directory(root);
  struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove_all(p,ec);}}cleanup{root};
  std::string secure=path_utf8(root);Options o;auto p=root/"data.csv";
  auto bindcsv=[&](){return oceanbase::sql::file_sql::bind(path_utf8(p),secure,o);};
  check(infer("9223372036854775807")==Type::Bigint,"BIGINT max");
  check(infer("-9223372036854775808")==Type::Bigint,"BIGINT min");
  check(infer("9223372036854775808")==Type::Varchar,"BIGINT overflow");
  check(infer("-9223372036854775809")==Type::Varchar,"BIGINT underflow");
  check(infer("1e309")==Type::Varchar,"DOUBLE overflow");
  check(infer("NaN")==Type::Varchar,"NaN");check(infer("0x10")==Type::Varchar,"hex");
  check(infer("1.25e-3")==Type::Double,"DOUBLE");
  check(infer("True")==Type::Boolean,"title-case boolean");
  check(infer("fAlSe")==Type::Boolean,"mixed-case boolean");
  check(infer("2024-02-29")==Type::Date,"leap date");
  check(infer("2023-02-29")==Type::Varchar,"invalid date");
  check(infer("1900-02-29")==Type::Varchar,"century date");
  check(infer("2000-02-29")==Type::Date,"400 year date");
  check(infer("2024-01-01 24:00:00")==Type::Varchar,"invalid hour");
  check(infer("2024-01-01T23:59:59.123456")==Type::Datetime,"DATETIME");
  check(temporal_value("1970-01-01",Type::Date)==0,"epoch date");
  check(temporal_value("1969-12-31 23:59:59.999999",Type::Datetime)==-1,"negative timestamp");
  for(int a=0;a<7;++a)for(int b=0;b<7;++b){
    check(widen(Type(a),Type(b))==widen(Type(b),Type(a)),"widen commutative");
    for(int c=0;c<7;++c)check(widen(widen(Type(a),Type(b)),Type(c))==widen(Type(a),widen(Type(b),Type(c))),"widen associative");
  }
  check(!valid_utf8(std::string("\xc0\x80",2)),"overlong UTF8");
  check(!valid_utf8(std::string("\xed\xa0\x80",3)),"surrogate UTF8");
  write(p,"\xef\xbb\xbf" "id,name,flag,amount,day,stamp,none\r\n1,\"中,文\",true,2.5,2024-02-29,2024-02-29 01:02:03,\r\n2,\"line1\r\nline2 \"\"quoted\"\"\",false,3,2024-03-01,2024-03-01 00:00:00,\r\n");
  auto b=bindcsv();check(b.columns.size()==7,"column count");
  check(b.columns[0].type==Type::Bigint&&b.columns[2].type==Type::Boolean&&b.columns[3].type==Type::Double,"types");
  check(b.columns[4].type==Type::Date&&b.columns[5].type==Type::Datetime&&b.columns[6].type==Type::Null,"temporal and NULL");
  auto decoded=Binding::decode(b.encode());check(decoded.encode()==b.encode(),"binding roundtrip");
  {Scan scan(decoded,secure);auto rows=scan.batch(10);check(rows.size()==2,"batch count");check(rows[1][1].text=="line1\r\nline2 \"quoted\"","multiline CSV");scan.rescan();Row r;check(scan.next(r)&&r[0].text=="1","rescan");}
  fails([&]{Binding::decode(b.encode().substr(1));},"signature");
  fails([&]{oceanbase::sql::file_sql::bind(path_utf8(p),"",o);},"secure_file_priv");
  fails([&]{oceanbase::sql::file_sql::bind(path_utf8(p),path_utf8((root/"other")),o);});
  auto out=root/"out.csv";
  {CsvOutput csv(path_utf8(out),secure,o);Row row;for(auto &c:b.columns)row.push_back({c.name,Type::Varchar});csv.row(row);Scan scan(b,secure);while(scan.next(row))csv.row(row);csv.commit();}
  auto exported=oceanbase::sql::file_sql::bind(path_utf8(out),secure,o);check(exported.columns.size()==7,"COPY roundtrip schema");
  {Scan scan(exported,secure);auto r=scan.batch(4);check(r.size()==2&&r[0][6].type==Type::Null,"COPY roundtrip NULL");}
  fails([&]{CsvOutput csv(path_utf8(out),secure,o);},"exists");
  {CsvOutput csv(path_utf8((root/"aborted.csv")),secure,o);csv.row({{"a",Type::Varchar}});}
  check(!fs::exists(root/"aborted.csv"),"failed COPY cleanup");
  {
    auto bad_out=root/"invalid-output.csv";
    {CsvOutput csv(path_utf8(bad_out),secure,o);
     fails([&]{csv.row({{std::string("left\0right",10),Type::Varchar}});},"NUL");}
    check(!fs::exists(bad_out),"NUL output is never published");
    {CsvOutput csv(path_utf8(bad_out),secure,o);
     fails([&]{csv.row({{std::string("\xc0\x80",2),Type::Varchar}});},"UTF-8");}
    check(!fs::exists(bad_out),"invalid UTF-8 output is never published");
    Options limited;limited.header=false;limited.max_record_bytes=8;
    auto edge=root/"boundary-output.csv";
    {CsvOutput csv(path_utf8(edge),secure,limited);csv.row({{"12345",Type::Varchar}});csv.commit();}
    auto bound=oceanbase::sql::file_sql::bind(path_utf8(edge),secure,limited);
    {Scan scan(bound,secure);Row row;check(scan.next(row)&&row[0].text=="12345","COPY record limit includes LF and reads back");}
    {CsvOutput csv(path_utf8(bad_out),secure,limited);
     fails([&]{csv.row({{"123456",Type::Varchar}});},"record limit");}
    {CsvOutput csv(path_utf8(bad_out),secure,limited);
     fails([&]{csv.row({{"\"\"\"",Type::Varchar}});},"record limit");}
    check(!fs::exists(bad_out),"oversized encoded output is never published");
    fs::remove(edge);
  }
  for(auto &f:fs::directory_iterator(root))check(path_utf8(f.path().filename()).find(".seekdb-")==std::string::npos,"no temp leak");
  write(p,"a,b\n1\n");fails(bindcsv,"expected 2 fields");fails(bindcsv,"physical line 2");
  write(p,"a\n\"unterminated\n");fails(bindcsv,"record 2");
  write(p,"a\n\"closed\"x\n");fails(bindcsv,"closing quote");
  write(p,"a,A\n1,2\n");check(bindcsv().columns[1].name=="A__2","duplicate header suffix");
  write(p,"a,a,a__2,a\n1,2,3,4\n");
  {auto b=bindcsv();check(b.columns[1].name=="a__3"&&b.columns[3].name=="a__4","suffix collision");}
  write(p,"a,b\n1,\n2,3\n");
  {auto b=bindcsv();check(!b.columns[0].nullable&&b.columns[1].nullable,"inferred nullability");
   auto round=Binding::decode(b.encode());check(round.columns[1].nullable,"serialized nullability");}
  write(p,"a\n\xc0\x80\n");fails(bindcsv,"UTF-8");
  write(p,"a\n1\n");o.sample_size=1;fails(bindcsv,"sample_size");o.sample_size=-1;
  o.max_record_bytes=1;fails(bindcsv,"record size");o.max_record_bytes=16*1024*1024;
  write(p,"a;b\n1;NULL\n2;\"NULL\"\n");o.delimiter=';';o.nullstr="NULL";
  {auto x=bindcsv();Scan scan(x,secure);auto rows=scan.batch(2);check(rows[0][1].type==Type::Null&&rows[1][1].text=="NULL"&&rows[1][1].type==Type::Varchar,"quoted NULL");}
  o=Options{};o.header=false;write(p,"1,x\n2,y\n");check(bindcsv().columns[0].name=="column1","header false");o.header=true;
  write(p,"a\n1\n");b=bindcsv();write(p,"a\n12\n");fails([&]{Scan scan(b,secure);},"changed");
  auto j=root/"data.jsonl";o.format="json";
  write(j,"{\"id\":1,\"x\":null}\n{\"id\":2,\"late\":true}\n{\"id\":\"three\",\"x\":2.5}\n");
  b=oceanbase::sql::file_sql::bind(path_utf8(j),secure,o);check(b.columns.size()==3&&b.columns[0].type==Type::Varchar,"JSON union and conflict");
  check(!b.columns[0].nullable&&b.columns[1].nullable&&b.columns[2].nullable,"JSON null and late-field nullability");
  {Scan scan(b,secure);auto r=scan.batch(10);check(r[0][2].type==Type::Null&&r[1][1].type==Type::Null&&r[1][2].text=="true","JSON missing NULL");scan.rescan();check(scan.batch(10).size()==3,"JSON rescan");}
  for(auto text:{"{\"a\":{}}\n","{\"a\":[]}\n","[]\n","{\"a\":1,\"a\":2}\n","{bad}\n"}){write(j,text);fails([&]{oceanbase::sql::file_sql::bind(path_utf8(j),secure,o);});}
  write(j,"{\"huge\":18446744073709551616,\"big\":9223372036854775808,\"string\":\"1\",\"number\":1}\n");
  b=oceanbase::sql::file_sql::bind(path_utf8(j),secure,o);
  check(b.columns[0].type==Type::Varchar&&b.columns[1].type==Type::Varchar&&b.columns[2].type==Type::Varchar&&b.columns[3].type==Type::Bigint,"JSON numeric boundaries and quoted numbers");
  {Scan scan(b,secure);Row r;check(scan.next(r)&&r[0].text=="18446744073709551616"&&r[1].text=="9223372036854775808","JSON overflowing integers preserve original digits");}
  write(j,"\n{bad}\n");fails([&]{oceanbase::sql::file_sql::bind(path_utf8(j),secure,o);},"physical line 2");
  auto files=discover(path_utf8((root/"*.csv")),secure);check(files.size()==2,"glob count");
  check(std::is_sorted(files.begin(),files.end()),"glob order");
  check(discover(path_utf8((root/"nomatch-*.csv")),secure).empty(),"glob no match");
  fails([&]{discover(path_utf8((root/"*.csv")),secure,1);},"limit");
  check(discover(path_utf8((root/"[do]*.csv")),secure).size()==2,"glob character class");
  fs::create_directory(root/"sub");write(root/"sub"/"small.csv","a\n1\n");
  check(discover(path_utf8((root/"s*"/"?.csv")),secure).empty(),"glob directory no file match");
  check(discover(path_utf8((root/"s*"/"*.csv")),secure).size()==1,"glob directory pattern");
  o=Options{};o.quote='\'';o.escape='\\';o.delimiter=';';
  write(p,"a;b\n1;'can\\'t;stop'\n");b=bindcsv();
  {Scan scan(b,secure);Row r;check(scan.next(r)&&r[1].text=="can't;stop","explicit escape");fails([&]{scan.batch(4097);},"batch");}
  o=Options{};write(p,"a\n\n\"\"\n");b=bindcsv();
  {Scan scan(b,secure);auto r=scan.batch(2);check(r[0][0].type==Type::Null&&r[1][0].type==Type::Varchar&&r[1][0].text.empty(),"quoted empty vs NULL");}
  write(p,"a,b\n");b=bindcsv();check(b.columns.size()==2&&b.columns[0].type==Type::Null,"header-only schema");
  check(b.columns[0].nullable,"NULL schema is nullable");
  write(p,"Ａ\n1\n");check(bindcsv().columns[0].name=="Ａ","UTF8 header beginning with EF is not a BOM");
  auto unicode=root/fs::u8path("中文.csv");write(unicode,"名称\n内容\n");
  check(oceanbase::sql::file_sql::bind(path_utf8(unicode),secure,o).columns[0].name=="名称","UTF8 filesystem path");
  write(p,"");fails(bindcsv,"no columns");
  write(p,"a,b\n1,2\n");o.max_columns=1;fails(bindcsv,"column limit");o=Options{};
  std::string late="a\n";for(int i=0;i<2048;++i)late+="1\n";late+="1.5\n";write(p,late);
  check(bindcsv().columns[0].type==Type::Double,"full inference includes late type widening");
  fails([&]{oceanbase::sql::file_sql::bind(std::string("x\0y",3),secure,o);},"local filesystem");
  fails([&]{oceanbase::sql::file_sql::bind("https://example.com/data.csv",secure,o);},"local filesystem");
  auto sibling=root.parent_path()/(path_utf8(root.filename())+"-sibling");fs::create_directory(sibling);
  Cleanup sibling_cleanup{sibling};write(sibling/"escape.csv","a\n1\n");
  fails([&]{oceanbase::sql::file_sql::bind(path_utf8((sibling/"escape.csv")),secure,o);},"outside");
  fails([&]{oceanbase::sql::file_sql::bind(path_utf8((root/".."/sibling.filename()/"escape.csv")),secure,o);},"outside");
  fails([&]{CsvOutput csv(path_utf8((sibling/"escape-out.csv")),secure,o);},"outside");
  write(p,"a\n1\n2\n");b=bindcsv();
  {Scan scan(b,secure);Row r;check(scan.next(r),"first row before mutation");
#ifdef _WIN32
    std::ofstream mutate(p,std::ios::app);check(!mutate,"Windows input rejects concurrent writes");
    fails([&]{CsvOutput csv(path_utf8(p)+":alternate",secure,o);},"alternate data streams");
#else
    write(p,"a\n1\n3\n");fails([&]{scan.next(r);},"changed");
#endif
  }
#ifndef _WIN32
  write(p,"a\n1\n2\n");b=bindcsv();
  {Scan scan(b,secure);fs::rename(p,root/"original.csv");write(p,"a\n9\n9\n");Row r;fails([&]{scan.next(r);},"replaced");}
  std::error_code link_error;fs::create_symlink(sibling/"escape.csv",root/"escape-link.csv",link_error);
  if(!link_error){fails([&]{oceanbase::sql::file_sql::bind(path_utf8((root/"escape-link.csv")),secure,o);},"outside");}
#endif
  {std::ofstream f(p,std::ios::binary);f<<"a\n";std::string field(1024*1024,'x');for(int i=0;i<16;++i)f<<field<<'\n';}
  b=bindcsv();
  {Scan scan(b,secure);auto part=scan.batch(4096);check(!part.empty()&&part.size()<16,"batch byte budget");size_t count=part.size();part.clear();for(;;){part=scan.batch(4096);if(part.empty())break;count+=part.size();}check(count==16,"byte-limited batches do not lose rows");}
  std::cout<<"PASS "<<checks<<" checks\n";
  return 0;
 }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
