// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "file_sql.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <iomanip>
#include <locale>
#include <sstream>

namespace oceanbase { namespace sql { namespace file_sql { namespace {
[[noreturn]] void bad(const std::string &s){throw Error("file SQL Parquet: "+s);}
uint64_t little(const std::string &s,size_t &p,size_t n)
{if(n>8||p>s.size()||n>s.size()-p)bad("truncated value");uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(uint8_t(s[p++]))<<(i*8);return v;}
uint64_t varint(const std::string &s,size_t &p)
{uint64_t n=0;for(int i=0;i<10;++i){auto b=little(s,p,1);if(i==9&&b>1)bad("varint overflow");n|=(b&127)<<(7*i);if(!(b&128))return n;}bad("invalid varint");}
struct Node {
  int64_t integer=0;std::string bytes;std::map<int,Node> fields;std::vector<Node> items;
  const Node &get(int id)const{auto i=fields.find(id);if(i==fields.end())bad("missing metadata field "+std::to_string(id));return i->second;}
  int64_t num(int id,int64_t def=0)const{auto i=fields.find(id);return i==fields.end()?def:i->second.integer;}
  bool has(int id)const{return fields.find(id)!=fields.end();}
};
class Compact {
  const std::string &s_;size_t p_=0,nodes_=0;
  Node value(int type,int depth) {
    if(++nodes_>200000||depth>24)bad("metadata resource limit");Node n;
    if(type==1||type==2)n.integer=type==1;
    else if(type==3)n.integer=int8_t(little(s_,p_,1));
    else if(type>=4&&type<=6){uint64_t v=varint(s_,p_);n.integer=int64_t(v>>1)^-int64_t(v&1);}
    else if(type==7)n.integer=little(s_,p_,8);
    else if(type==8){uint64_t len=varint(s_,p_);if(len>s_.size()-p_)bad("invalid string size");n.bytes=s_.substr(p_,len);p_+=len;}
    else if(type==9||type==10) {
      auto h=little(s_,p_,1);uint64_t len=h>>4;int elem=h&15;if(len==15)len=varint(s_,p_);
      if(len>200000||len>s_.size()-p_)bad("invalid list size");
      for(uint64_t i=0;i<len;++i)n.items.push_back(value(elem,depth+1));
    } else if(type==11) {
      uint64_t count=varint(s_,p_);if(count>10000)bad("map size limit");
      if(count){auto t=little(s_,p_,1);for(uint64_t i=0;i<count;++i){value(t>>4,depth+1);value(t&15,depth+1);}}
    } else if(type==12) {
      int last=0;
      for(;;){auto h=little(s_,p_,1);int t=h&15;if(!t)break;int id;
        if(h>>4)id=last+(h>>4);else{auto v=varint(s_,p_);id=int(v>>1)^-int(v&1);}
        if(id<=0||n.has(id))bad("invalid or duplicate metadata field");last=id;n.fields.emplace(id,value(t,depth+1));}
    } else bad("unknown compact protocol type");return n;
  }
public:
  explicit Compact(const std::string &s):s_(s){}
  Node structure(){return value(12,0);}
  size_t position()const{return p_;}
};
std::string snappy(const std::string &s,size_t expected)
{
  size_t p=0;if(varint(s,p)!=expected)bad("Snappy length mismatch");std::string out;out.reserve(expected);
  while(p<s.size()) {
    unsigned tag=little(s,p,1),kind=tag&3;size_t n=0,offset=0;
    if(!kind){n=tag>>2;if(n<60)++n;else{unsigned bytes=n-59;if(bytes>4)bad("invalid Snappy literal");n=little(s,p,bytes)+1;}
      if(n>s.size()-p||n>expected-out.size())bad("Snappy literal exceeds page");out.append(s,p,n);p+=n;
    }else{
      if(kind==1){n=4+((tag>>2)&7);offset=((tag&0xe0)<<3)|little(s,p,1);}
      else{n=1+(tag>>2);offset=little(s,p,kind==2?2:4);}
      if(!offset||offset>out.size()||n>expected-out.size())bad("invalid Snappy copy");
      for(size_t i=0;i<n;++i)out+=out[out.size()-offset];
    }
  }if(out.size()!=expected)bad("truncated Snappy page");return out;
}
std::string decompress(const std::string &s,int codec,size_t expected)
{
  if(codec==0){if(s.size()!=expected)bad("uncompressed page length mismatch");return s;}
  if(codec==1)return snappy(s,expected);
bad("unsupported compression codec "+std::to_string(codec));
}
uint32_t crc32(const std::string &s)
{
  uint32_t crc=~uint32_t(0);
  for(uint8_t c:s){crc^=c;for(int i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&-int32_t(crc&1));}
  return ~crc;
}
std::vector<uint32_t> hybrid(const std::string &s,size_t &p,size_t end,unsigned bits,size_t count)
{
  if(bits>32||end>s.size()||p>end||count>64*1024*1024)bad("invalid RLE bounds");
  std::vector<uint32_t> out;out.reserve(count);
  while(out.size()<count) {
    if(p>=end)bad("truncated RLE stream");auto h=varint(s,p);if(p>end||!h)bad("invalid RLE run");
    unsigned scale=h&1?8:1;if((h>>1)>uint64_t(INT32_MAX)/scale)bad("RLE run length overflow");
    uint64_t n=(h>>1)*scale;
    if(n>count-out.size()+(h&1?7:0))bad("RLE run exceeds page");
    if(!(h&1)) {
      uint64_t v=little(s,p,(bits+7)/8);if(p>end||(bits<32&&v>=(uint64_t(1)<<bits)))bad("invalid RLE value");
      for(uint64_t i=0;i<n;++i)out.push_back(v);
    } else {
      uint64_t bytes=n*bits/8;if(bytes>end-p)bad("truncated bit-packed run");size_t start=p;p+=bytes;
      for(uint64_t i=0;i<n && out.size()<count;++i){uint32_t v=0;for(unsigned b=0;b<bits;++b){uint64_t at=i*bits+b;v|=((uint8_t(s[start+at/8])>>(at%8))&1)<<b;}out.push_back(v);}
    }
  }return out;
}
std::string civil(int64_t days,int64_t micros=-1)
{
  int64_t z=days+719468;const int64_t era=(z>=0?z:z-146096)/146097;unsigned doe=z-era*146097;
  unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t y=yoe+era*400;
  unsigned doy=doe-(365*yoe+yoe/4-yoe/100),mp=(5*doy+2)/153;
  unsigned d=doy-(153*mp+2)/5+1,m=mp+(mp<10?3:-9);y+=m<=2;
  if(y<1||y>9999)bad("date outside SQL calendar");char buf[40];
  if(micros<0)std::snprintf(buf,sizeof(buf),"%04d-%02u-%02u",int(y),m,d);
  else std::snprintf(buf,sizeof(buf),"%04d-%02u-%02u %02d:%02d:%02d.%06d",int(y),m,d,int(micros/3600000000LL),int(micros/60000000%60),int(micros/1000000%60),int(micros%1000000));
  return buf;
}
struct Schema {Column column;int physical=0,repetition=0,length=0;int64_t time_scale=0;};
struct Chunk {uint64_t offset=0,end=0,values=0;int codec=0;};
struct Group {uint64_t rows=0;std::vector<Chunk> chunks;};
class Parquet final : public IReader {
  Input &in_;Options opt_;uint64_t footer_=0;std::vector<Schema> schema_;std::vector<Column> columns_;
  std::vector<Group> groups_;size_t group_=0;uint64_t row_=0;
  struct State {
    uint64_t cursor=0,read=0;std::vector<Cell> dictionary,page;size_t at=0;
    size_t memory()const{size_t n=(dictionary.capacity()+page.capacity())*sizeof(Cell);for(auto &v:dictionary)n+=v.text.capacity();for(auto &v:page)n+=v.text.capacity();return n;}
  };
  std::vector<State> states_;std::vector<bool> projected_;
  Cell plain(const Schema &sc,const std::string &s,size_t &p,size_t index) {
    Cell c;c.type=sc.column.type;
    if(sc.physical==0){size_t off=p+index/8;if(off>=s.size())bad("truncated boolean");c.text=(uint8_t(s[off])>>(index%8))&1?"true":"false";}
    else if(sc.physical==1||sc.physical==2) {
      uint64_t u=little(s,p,sc.physical==1?4:8);int64_t v=sc.physical==1?int64_t(int32_t(u)):int64_t(u);
      if(c.type==Type::Date)c.text=civil(v);
      else if(c.type==Type::Datetime){
        if(sc.time_scale==1000){if(v>INT64_MAX/1000||v<INT64_MIN/1000)bad("timestamp overflow");v*=1000;}
        else if(sc.time_scale==-1000){if(v%1000)bad("nanosecond precision exceeds DATETIME(6)");v/=1000;}
        int64_t day=v/86400000000LL,us=v%86400000000LL;if(us<0){--day;us+=86400000000LL;}c.text=civil(day,us);
      }else c.text=std::to_string(v);
    } else if(sc.physical==3) {
      uint64_t ns=little(s,p,8),julian=little(s,p,4);if(ns>=86400000000000ULL||ns%1000)bad("invalid INT96 timestamp");
      c.text=civil(int64_t(julian)-2440588,ns/1000);
    } else if(sc.physical==4||sc.physical==5) {
      double v; if(sc.physical==4){uint32_t u=little(s,p,4);float f;std::memcpy(&f,&u,4);v=f;}
      else{uint64_t u=little(s,p,8);std::memcpy(&v,&u,8);}
      if(!std::isfinite(v))bad("non-finite floating point value");
      std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(17)<<v;c.text=text.str();
    } else if(sc.physical==6||sc.physical==7) {
      uint64_t n=sc.physical==6?little(s,p,4):sc.length;
      if(n>opt_.max_record_bytes||n>s.size()-p)bad("invalid byte array length");c.text=s.substr(p,n);p+=n;
      if(!valid_utf8(c.text)||c.text.find('\0')!=std::string::npos)bad("byte array is not UTF-8 text");
    }else bad("unsupported physical type");return c;
  }
  static constexpr uint64_t CACHE_LIMIT=96*1024*1024;
  uint64_t memory_used() const {
    uint64_t total=0;for(auto &s:states_){total+=s.memory();if(total>CACHE_LIMIT)bad("scan memory limit exceeded");}return total;
  }
  uint64_t reserve(std::vector<Cell> &target,size_t count) {
    uint64_t used=memory_used();
    if(count>target.capacity() && (count-target.capacity())*sizeof(Cell)>CACHE_LIMIT-used)bad("scan memory limit exceeded");
    target.reserve(count);return memory_used();
  }
  void append(std::vector<Cell> &target,Cell value,uint64_t &used) {
    if(value.text.capacity()>CACHE_LIMIT-used)bad("scan memory limit exceeded");
    used+=value.text.capacity();target.push_back(std::move(value));
  }
  void page(size_t column) {
    State &st=states_[column];auto &sc=schema_[column];auto &chunk=groups_[group_].chunks[column];
    st.page.clear();st.at=0;
    for(;;) {
      if(st.cursor>=chunk.end)bad("column chunk ended before row group");
      size_t n=std::min<uint64_t>(64*1024,chunk.end-st.cursor);in_.seek(st.cursor);std::string header=in_.read(n);
      Compact compact(header);Node h=compact.structure();st.cursor+=compact.position();
      int64_t compressed=h.num(3,-1),uncompressed=h.num(2,-1);int type=h.num(1,-1);
      if(compressed<0||uncompressed<0||uint64_t(compressed)>opt_.max_page_bytes||uint64_t(uncompressed)>opt_.max_page_bytes
          ||st.cursor>chunk.end||uint64_t(compressed)>chunk.end-st.cursor)bad("page size limit or invalid page extent");
      in_.seek(st.cursor);std::string raw=in_.read(compressed);st.cursor+=compressed;
      if(h.has(4)&&crc32(raw)!=uint32_t(h.num(4)))bad("page CRC mismatch");
      if(type!=0&&type!=2&&type!=3)bad("unsupported page type");
      std::string data;size_t pos=0;uint64_t count=0;int encoding=0;std::vector<uint32_t> defs;
      if(type==3) {
        const Node &d=h.get(8);count=d.num(1);encoding=d.num(4);int64_t dl=d.num(5),rl=d.num(6);
        if(count>opt_.max_page_bytes/sizeof(Cell))bad("page value count limit");
        if(dl<0||rl!=0||uint64_t(dl)>raw.size()||dl>uncompressed)bad("invalid V2 levels");
        if(sc.repetition==1)defs=hybrid(raw,pos,dl,1,count);else if(dl)bad("unexpected V2 definition levels");
        if(pos!=uint64_t(dl))bad("V2 level length mismatch");
        data=raw.substr(0,dl)+decompress(raw.substr(dl),d.num(7,1)?chunk.codec:0,uncompressed-dl);pos=dl;
      } else {
        data=decompress(raw,chunk.codec,uncompressed);
        const Node &d=h.get(type==2?7:5);count=d.num(1);encoding=d.num(2);
        if(count>opt_.max_page_bytes/sizeof(Cell))bad("page value count limit");
        if(type==0&&sc.repetition==1){if(d.num(3)!=3)bad("unsupported definition encoding");auto bytes=little(data,pos,4);if(bytes>data.size()-pos)bad("invalid levels length");size_t end=pos+bytes;defs=hybrid(data,pos,end,1,count);if(pos!=end)bad("levels length mismatch");}
      }
      if(count>opt_.max_page_bytes/sizeof(Cell))bad("page value count limit");
      if(type==2){
        if(encoding!=0||!st.dictionary.empty()||st.read)bad("invalid dictionary page");
        uint64_t used=reserve(st.dictionary,count);
        for(uint64_t i=0;i<count;++i)append(st.dictionary,plain(sc,data,pos,i),used);
        if(sc.physical==0)pos+=(count+7)/8;
        if(pos!=data.size())bad("dictionary page length mismatch");continue;
      }
      if(count==0||count>chunk.values-st.read)bad("page count exceeds column chunk");
      std::vector<uint32_t> indices;size_t nonnull=0;
      for(uint64_t i=0;i<count;++i)if(defs.empty()||defs[i])++nonnull;
      if(encoding==2||encoding==8){auto bits=little(data,pos,1);indices=hybrid(data,pos,data.size(),bits,nonnull);}
      else if(encoding==3&&sc.physical==0) {
        uint64_t len=little(data,pos,4);if(len!=data.size()-pos)bad("boolean RLE length mismatch");
        indices=hybrid(data,pos,data.size(),1,nonnull);
      }
      else if(encoding!=0)bad("unsupported value encoding "+std::to_string(encoding));
      size_t value=0;uint64_t used=reserve(st.page,count);
      for(uint64_t i=0;i<count;++i) {
        if(!defs.empty()&&!defs[i])append(st.page,Cell{},used);
        else {
          if(encoding==0)append(st.page,plain(sc,data,pos,value),used);
          else if(encoding==3)append(st.page,{indices[value]?"true":"false",Type::Boolean},used);
          else {
            if(indices[value]>=st.dictionary.size())bad("dictionary index outside dictionary");
            const Cell &source=st.dictionary[indices[value]];
            if(source.text.size()>CACHE_LIMIT-used)bad("dictionary expansion exceeds scan memory limit");
            append(st.page,source,used);
          }
          ++value;
        }
      }
      if(encoding==0&&sc.physical==0)pos+=(nonnull+7)/8;
      if(pos!=data.size())bad("data page length mismatch");
      st.read+=count;return;
    }
  }
public:
  Parquet(Input &input,const Options &opt):in_(input),opt_(opt) {
    uint64_t size=input.identity().size;if(size<12)bad("truncated footer");
    input.seek(0);if(input.read(4)!="PAR1")bad("invalid header magic");
    input.seek(size-8);std::string trailer=input.read(8);size_t p=0;uint64_t len=little(trailer,p,4);
    if(trailer.substr(4)!="PAR1"||len>size-12||len>opt.max_metadata_bytes)bad("invalid footer or metadata limit");
    footer_=size-8-len;input.seek(footer_);std::string bytes=input.read(len);Compact c(bytes);Node meta=c.structure();
    if(c.position()!=bytes.size()||meta.num(1)<=0)bad("invalid footer metadata");
    auto &schema=meta.get(2).items;if(schema.size()<2||schema.size()>opt.max_columns+1||schema[0].num(5)!=int64_t(schema.size()-1))bad("expected a flat footer schema");
    for(size_t i=1;i<schema.size();++i) {
      auto &s=schema[i];Schema sc;sc.column.name=s.get(4).bytes;sc.physical=s.num(1,-1);sc.repetition=s.num(3,0);sc.length=s.num(2);
      if(s.num(5)!=0||sc.repetition<0||sc.repetition>1)bad("nested and repeated columns are unsupported");
      sc.column.type=sc.physical==0?Type::Boolean:(sc.physical==1||sc.physical==2?Type::Bigint:(sc.physical==3?Type::Datetime:(sc.physical==4||sc.physical==5?Type::Double:Type::Varchar)));
      int converted=s.num(6,-1);
      if(converted==6){if(sc.physical!=1)bad("DATE must use INT32");sc.column.type=Type::Date;}
      else if(converted==9||converted==10){if(sc.physical!=2)bad("TIMESTAMP must use INT64");sc.column.type=Type::Datetime;sc.time_scale=converted==9?1000:1;}
      else if(converted!=-1&&converted!=0&&!(converted>=15&&converted<=18))bad("unsupported converted type");
      if(s.has(10)) {
        auto &logical=s.get(10);
        if(logical.has(6)){if(sc.physical!=1)bad("DATE must use INT32");sc.column.type=Type::Date;}
        else if(logical.has(8)) {
          if(sc.physical!=2)bad("TIMESTAMP must use INT64");sc.column.type=Type::Datetime;auto &unit=logical.get(8).get(2);
          sc.time_scale=unit.has(1)?1000:(unit.has(2)?1:(unit.has(3)?-1000:0));if(!sc.time_scale)bad("unknown timestamp unit");
        }else if(logical.has(10)){if(!logical.get(10).num(2))bad("unsigned integer is unsupported");}
        else if(logical.has(11))sc.column.type=Type::Null;
        else if(!logical.has(1)&&!logical.has(11))bad("unsupported logical type");
      }
      if(sc.physical<0||sc.physical>7||(sc.physical==7&&(sc.length<=0||uint64_t(sc.length)>opt.max_record_bytes)))bad("invalid physical type");
      sc.column.nullable=sc.repetition==1;
      sc.column.width=sc.column.type==Type::Varchar?opt.max_record_bytes:32;schema_.push_back(sc);columns_.push_back(sc.column);
    }
    uint64_t rows=0;
    for(auto &r:meta.get(4).items) {
      Group g;if(r.num(3)<0)bad("negative row count");g.rows=r.num(3);auto &chunks=r.get(1).items;
      if(chunks.size()!=schema_.size())bad("row group column count mismatch");
      for(size_t i=0;i<chunks.size();++i) {
        if(chunks[i].has(1))bad("external column chunks are unsupported");auto &m=chunks[i].get(3);Chunk ch;
        auto &path=m.get(3).items;if(path.size()!=1||path[0].bytes!=schema_[i].column.name||m.num(1)!=schema_[i].physical)bad("column chunk schema mismatch");
        int64_t off=m.has(11)?m.num(11):m.num(9),extent=m.num(7),values=m.num(5);
        if(off<4||extent<0||values<0||uint64_t(values)!=g.rows||uint64_t(off)>footer_||uint64_t(extent)>footer_-off)bad("invalid column chunk extent");
        ch.offset=off;ch.end=off+extent;ch.values=values;ch.codec=m.num(4);g.chunks.push_back(ch);
      }
      if(g.rows>UINT64_MAX-rows)bad("row count overflow");rows+=g.rows;groups_.push_back(std::move(g));
    }
    if(meta.num(3)<0||rows!=uint64_t(meta.num(3)))bad("footer row count mismatch");
    projected_.assign(schema_.size(),true);rescan();
  }
  bool next(Row &out) override {
    in_.check();
    while(group_<groups_.size()&&row_==groups_[group_].rows){++group_;row_=0;states_.clear();}
    if(group_==groups_.size())return false;
    if(states_.empty()){states_.resize(schema_.size());for(size_t i=0;i<schema_.size();++i)states_[i].cursor=groups_[group_].chunks[i].offset;}
    out.assign(schema_.size(),Cell{});uint64_t row_bytes=0;
    for(size_t i=0;i<schema_.size();++i)if(projected_[i]){
      State &s=states_[i];if(s.at==s.page.size())page(i);
      row_bytes+=s.page[s.at].text.size();if(row_bytes>opt_.max_record_bytes)bad("row size limit exceeded");out[i]=s.page[s.at++];
    }
    ++row_;in_.check();return true;
  }
  void rescan()override{in_.check();group_=0;row_=0;states_.clear();}
  void project(const std::vector<size_t> &columns)override{
    if(row_||group_)bad("projection must be set before reading");projected_.assign(schema_.size(),false);
    for(size_t i:columns){if(i>=schema_.size())bad("invalid projected column");projected_[i]=true;}
  }
  const std::vector<Column> &columns()const override{return columns_;}
};
} // namespace
std::unique_ptr<IReader> make_parquet_reader(Input &in,const Options &opt){return std::make_unique<Parquet>(in,opt);}
}}}
