/*******************************************************************************
 * Title       : qdda - quick & dirty dedup analyzer
 * Description : Checks files or block devices for duplicate blocks
 * Author      : Bart Sjerps <bart@outrun.nl>
 * License     : GPLv3+
 * Disclaimer  : See https://www.gnu.org/licenses/gpl-3.0.txt
 * More info   : http://outrun.nl/wiki/qdda
 * -----------------------------------------------------------------------------
 * Build notes: Requires lz4 >= 1.7.1
 ******************************************************************************/

#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>

#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>    // srand, rand
#include <signal.h>
#include <sys/stat.h>  // stat

#include "error.h"
#include "lz4/lz4.h"
#include "zlib/zlib.h"
#include "tools.h"
#include "database.h"
#include "qdda.h"

extern "C" {
#include "md5/md5.h"
}

using namespace std;

/*******************************************************************************
 * global parameters - modify at own discretion
 ******************************************************************************/

#if defined(VERSION)
const char* PROGVERSION = TOSTRING(VERSION) RELEASE;
#else
const char* PROGVERSION = "0.0.1";
#endif

const char* kdefault_array = "x2";
const int kdefault_bandwidth = 200;
const int kmax_reader_threads = 8;

/*******************************************************************************
 * Initialization - globals
 ******************************************************************************/

bool g_debug = false; // global debug flag
bool g_query = false; // global query flag
bool g_quiet = false; // global quiet flag
extern sig_atomic_t g_abort; // global abort flag

ulong starttime = epoch(); // start time of program

ofstream c_debug; // Debug stream

FileData::FileData(const string& file) {
  ratio=0; limit_mb=0;
  stringstream ss(file);
  string strlimit,strrepeat;

  getline(ss,filename,':');
  getline(ss,strlimit,',');
  getline(ss,strrepeat);
  
  if(filename=="compress") ratio=1;
  if(filename=="compress") { filename = "/dev/urandom"; limit_mb=1024; }
  if(filename=="random")   { filename = "/dev/urandom"; limit_mb=1024; }
  if(filename=="zero")     { filename = "/dev/zero";    limit_mb=1024; }
  
  if(!strlimit.empty()) limit_mb = atoll(strlimit.c_str());
  repeat   = atoi(strrepeat.c_str());

  if(access(filename.c_str(), F_OK | R_OK)) {
    switch (errno) {
      case EACCES: throw ERROR("Access denied: ") << file << ", try 'sudo setfacl -m u:" << getenv ("USER") << ":r " << filename << "'";
      case ENOENT: throw ERROR("File does not exist: ") << file;
    }
    throw ERROR("File error: ") << file;
  }

  ifs = new ifstream;
  ifs->exceptions ( std::ifstream::failbit );

  c_debug << "Opening: " << file << endl;
  try {
    ifs->open(filename);
    ifs->exceptions ( std::ifstream::goodbit );
  }
  catch (std::exception& e) {
    throw ERROR("File open error in ") << file;
  }
}

/*******************************************************************************
 * Usage (from external files)
 ******************************************************************************/

const char * version_info  = 
  "Copyright (C) 2018 Bart Sjerps <bart@outrun.nl>\n"
  "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n"
  "This is free software: you are free to change and redistribute it.\n"
  "There is NO WARRANTY, to the extent permitted by law.\n\n"
  "build date: " __DATE__  "\nbuild time: " __TIME__ "\n";


const char * title_info = " - The Quick & Dirty Dedupe Analyzer\n"
  "Use for educational purposes only - actual array reduction results may vary\n";

extern const char* manpage_head;
extern const char* manpage_body;

void showtitle()   { if(!g_quiet) cout << "qdda " << PROGVERSION << title_info ; }
void showversion() { showtitle(); std::cout << version_info << std::endl; }

/*******************************************************************************
 * Various
 ******************************************************************************/

/*******************************************************************************
 * Notes on hash algorithm
 * SQLite integer (also used for primary key on kv table) has max 8 bytes and is
 * signed integer. MAX value is 9 223 372 036 854 775 808. The max value of
 * unsigned long (64 bit) is 18446744073709551615UL which result in negative
 * hash values in the kv table. MD5 sum is 128 bit which would not fit in SQLite
 * integers and would be converted to another datatype (TXT or blob).
 * I found hash collisions with CRC32 on large datasets so needed another hash
 * function - see https://en.wikipedia.org/wiki/Birthday_problem#Probability_table
 * which shows 50% chance of collisions with 77000 rows on 32bit hash which equals
 * 630MiB using 8K blocksize. Problem gets worse when increasing the dataset.
 * 7 bytes (56 bits) is a tradeoff between DB space consumed, performance and
 * accuracy and has a 50% chance of collision with 316M rows (4832GB@16K)
 * which is fine for datasets up to many terabytes.
 * A 64-bit hash would get roughly 1 collision every 77TB@16K.
 ******************************************************************************/

// returns the least significant 7 bytes of the md5 hash (16 bytes) as unsigned long
uint64_t hash_md5(const char * src, char* zerobuf, const int size) {
  unsigned char digest[16];
  memset(zerobuf,0,size);      // initialize buf with zeroes
  if(memcmp (src,zerobuf,size)==0) return 0;         // return 0 for zero block
  MD5_CTX ctx;  
  MD5_Init(&ctx);
  MD5_Update(&ctx, src, size);
  MD5_Final(digest, &ctx);
  return                                // ignore chars 0-8
    ((uint64_t)(digest[8]&0X0F)  << 56) +  // pick 4 bits from byte 7
    ((uint64_t)digest[9]  << 48) +         // all bits from byte 6 to 0
    ((uint64_t)digest[10] << 40) +         // convert char* to ulong but keeping
    ((uint64_t)digest[11] << 32) +         // the right order, only use lower 6 bytes (char 10-15)
    ((uint64_t)digest[12] << 24) +         // SQLite integer is 8 byte signed so we need to stay within
    ((uint64_t)digest[13] << 16) +         // 8 bytes and unsigned. 6 bytes is best compromise
    ((uint64_t)digest[14] << 8 ) +
    ((uint64_t)digest[15]);
}

// dummy compress function
u_int compress_none(const char * src, char* buf, const int size) { return size ; }
  
// Get compressed bytes for a compressed block - lz4
u_int compress_lz4(const char * src, char* buf, const int size) {
  int result = LZ4_compress_default(src, buf, size, size); // call LZ4 compression lib, only use bytecount & ignore data
  if(result>size) return size;                             // don't compress if size is larger than blocksize
  if(result==0) return size;
  return result;
}

u_int compress_deflate(const char* src, char* buf, const int size) {
  int ret;
  u_int compressed;
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree  = Z_NULL;
  strm.opaque = Z_NULL;
  ret = deflateInit(&strm, 6); // level
  strm.avail_in  = size;
  strm.avail_out = size;
  strm.next_in   = (unsigned char *)src;
  strm.next_out  = (unsigned char *)buf;
  ret = deflate(&strm, Z_FINISH);
  compressed = strm.total_out;
  (void)deflateEnd(&strm);
  return compressed;
}

/*******************************************************************************
 * Formatting & printing
 ******************************************************************************/

// Show scan speed info
void showprogress(const std::string& str) {
  if(g_quiet) return;
  static unsigned int l = 0;
  l = str.length() > l ? str.length() : l;     // track largest string length between calls
  if(str.length() == 0) {                      // clear line if string empty
    for(u_int i=0;i<l;i++) cout << ' ';   // overwrite old line with spaces
    for(u_int i=0;i<l;i++) cout << '\b';  // carriage return (no newline), \b = backspace
  } else {
    cout << str;
    for(u_int i=0;i<str.length();i++) cout << '\b'; // returns the cursor to original offset
    cout << std::flush;                             // print string ; carriage return
  }
}

// Show progress information, updated every N blocks. 
void progress(ulong blocks,ulong blocksize, ulong bytes, const char * msg) {
  static Stopwatch stopwatch;     // time of start processing file
  static Stopwatch prev;          // time of previous call
  stopwatch.lap();                // get current time interval
  static int   filenum   = 0;     // previous file num (tells us when new file process starts)
  static ulong prevbytes = 0;     // keep track of previous byte count
  if(filenum==0) {                // reset stats before processing new file
    stopwatch.reset();
    prev=stopwatch;
    prevbytes=bytes;
    filenum++;
  }
  auto avgsvctm = stopwatch;                                   // service time of xx bytes since start of file
  auto cursvctm = stopwatch - prev;                            // service time of xx bytes since previous call
  auto avgbw = safeDiv_ulong(bytes,avgsvctm);                  // bytes per second since start of file
  auto curbw = safeDiv_ulong((bytes-prevbytes),cursvctm);      // bytes per second since previous call
  stringstream ss;                                             // generate a string with the progress message
  ss << blocks        << " "                                   // blocks scanned
     << blocksize     << "k blocks ("                          // blocksize
     << bytes/1048576 << " MiB) processed, "                   // processed megabytes
     << setw(6) << curbw << "/" << avgbw << " MB/s (cur/avg)"; // current/average bandwidth
  if(msg) ss << msg;                                           // add message if specified
  ss << "                 " ;                                  // blank rest of line
  showprogress(ss.str());
  prev=stopwatch;  // save stopwatch time for next call
  prevbytes=bytes; // save byte count for next call
}

/*******************************************************************************
 * Functions
 ******************************************************************************/


void import(QddaDB& db, const string& filename) {
  if(!Database::isValid(filename.c_str())) return;
  QddaDB idb(filename);
  ulong blocksize = db.blocksize();
  if(blocksize != idb.blocksize()) throw ERROR("Incompatible blocksize on") << filename;

  cout << "Adding " << idb.getrows() << " blocks from " << filename << " to " << db.getrows() << " existing blocks" << endl;
  db.import(filename);
}

// Merge staging data into kv table, track & display time to merge
void merge(QddaDB& db, Parameters& parameters) {
  if(!Database::isValid(parameters.stagingname.c_str())) return;
  
  StagingDB sdb(parameters.stagingname);

  //stringstream ss2;
  ulong blocksize    = db.blocksize();
  ulong dbrows       = db.getrows();
  ulong tmprows      = sdb.getrows();
  ulong mib_staging  = tmprows*blocksize/1024;
  ulong mib_database = dbrows*blocksize/1024;
  ulong fsize1       = db.filesize();
  ulong fsize2       = sdb.filesize();

  if(blocksize != sdb.blocksize()) throw ERROR("Incompatible blocksize on stagingdb");
  
  sdb.close();

  Stopwatch stopwatch;
  if(tmprows) { // do nothing if merge db has no rows
    if(!g_quiet) cout 
      << "Merging " << tmprows << " blocks (" 
      << mib_staging << " MiB) with " 
      << dbrows << " blocks (" 
      << mib_database << " MiB)" << flush;

    stopwatch.reset();
    ulong index_rps = tmprows*1000000/stopwatch;
    ulong index_mbps = mib_staging*1000000/stopwatch;
    
    stopwatch.reset();
    db.merge(parameters.stagingname);
    stopwatch.lap();
    
    auto time_merge = stopwatch;
    ulong merge_rps = (tmprows+dbrows)*1000000/time_merge;
    ulong merge_mbps = (mib_staging+mib_database)*1000000/time_merge;
    if(!g_quiet) cout << " in "
      << stopwatch.seconds() << " sec (" 
      << merge_rps << " blocks/s, " 
      << merge_mbps << " MiB/s)" << endl;
  }
  Database::deletedb(parameters.stagingname);
}

// test hashing, compression and insert performance
void cputest(QddaDB& db, Parameters& p) {
 
  StagingDB::createdb(p.stagingname,db.blocksize());
  StagingDB stagingdb(p.stagingname);
  
  const ulong mib       = 1024; // size of test set
  const ulong blocksize = db.blocksize();

  const ulong rows      = mib * 1024 / blocksize;
  const ulong bufsize   = mib * 1024 * 1024;
  char*       testdata  = new char[bufsize];
  ulong*      hashes    = new ulong[rows];
  ulong*      bytes     = new ulong[rows];
  ulong time_hash, time_compress, time_insert;
  char buf[blocksize*1024];
  
  Stopwatch   stopwatch;

  cout << fixed << setprecision(2) << "*** Synthetic performance test, 1 thread ***" << endl;

  cout << "Initializing:" << flush; 
  srand(1);
  memset(testdata,0,bufsize);
  for(ulong i=0;i<bufsize;i++) testdata[i] = (char)rand() % 8; // fill test buffer with random(ish) but compressible data
  cout << setw(15) << rows << " blocks, " << blocksize << "k (" << bufsize/1048576 << " MiB)" << endl;

  cout << "Hashing:     " << flush;
  stopwatch.reset();
  for(ulong i=0;i<rows;i++) hashes[i] = hash_md5(testdata + i*blocksize*1024,buf,blocksize*1024);
  time_hash = stopwatch.lap();
  
  cout << setw(15) << time_hash     << " usec, " 
       << setw(10) << (float)bufsize/time_hash << " MB/s, " 
       << setw(11) << float(rows)*1000000/time_hash << " rows/s"
       << endl;

  cout << "Compress LZ4: " << flush;
  stopwatch.reset();
  
  for(ulong i=0;i<rows;i++) bytes[i] = compress_deflate(testdata + i*blocksize*1024,buf,blocksize*1024);
  time_compress = stopwatch.lap();
  cout << setw(15) << time_compress << " usec, " 
       << setw(10) << (float)bufsize/time_compress << " MB/s, " 
       << setw(11) << float(rows)*1000000/time_compress << " rows/s"
       << endl;

  cout << "Compress DEFLATE: " << flush;
  stopwatch.reset();

  for(ulong i=0;i<rows;i++) bytes[i] = compress_lz4(testdata + i*blocksize*1024,buf,blocksize*1024);
  time_compress = stopwatch.lap();
  cout << setw(15) << time_compress << " usec, " 
       << setw(10) << (float)bufsize/time_compress << " MB/s, " 
       << setw(11) << float(rows)*1000000/time_compress << " rows/s"
       << endl;


  // test sqlite insert performance
  cout << "DB insert:   " << flush;
  stopwatch.reset();

  stagingdb.begin();
  for(ulong i=0;i<rows;i++) stagingdb.insertdata(hashes[i],bytes[i]);
  stagingdb.end();
  time_insert = stopwatch.lap();
  cout << setw(15) << time_insert << " usec, "
       << setw(10) << (float)bufsize/time_insert   << " MB/s, "
       << setw(11) << float(rows)*1000000/time_insert << " rows/s"
       << endl;

  delete[] testdata;
  delete[] hashes;
  delete[] bytes;
  Database::deletedb(p.stagingname);
}


void manpage() {
  string cmd = "(";
  cmd += whoAmI();
  cmd += " --mandump > /tmp/qdda.1 ; man /tmp/qdda.1 ; rm /tmp/qdda.1 )";
  if(!system(cmd.c_str())) { };
}

const string& defaultDbName() {
  static string dbname;
  dbname = homeDir() + "/qdda.db";
  return dbname;
}

void showhelp(LongOptions& lo) {
  std::cout << "\nUsage: qdda <options> [FILE]...\nOptions:" << "\n";
  lo.printhelp(cout);
  std::cout << "\nMore info: qdda --man \nor the project homepage: http://outrun.nl/wiki/qdda\n\n";
}

void showlist() {
  showtitle();
  std::cout << "\narray options:\n\n"
    << "  --array x1    - XtremIO X1\n"
    << "  --array x2    - XtremIO X2\n"
    << "  --array vmax1 - VMAX All Flash (experimental)\n"
    << "  --array name=<name>,bs=<blocksize>,buckets=<bucketlist>\n\n"
    << "  blocksize in kb between 1 and 128, buckets in kb separated by +\n"
    << "  example: --array name=foo,bs=32,buckets=8+16+24+32\n"
    ;
}

void mandump(LongOptions& lo) {
  cout << manpage_head;
  lo.printman(cout);
  cout << manpage_body;
}

void rundemo() {
  string cmd = "";
  cmd += whoAmI();
  cmd += " -d /tmp/demo compress:128,4 compress:256,2 compress:512 zero:512";
  if(!system(cmd.c_str())) { };
}

void findhash(Parameters& parameters) {
  StagingDB db(parameters.stagingname);
  Query findhash(db,"select * from offsets where hash=?");
  findhash.bind(parameters.searchhash);
  findhash.report(cout,"20,20,10");
}

void tophash(QddaDB& db, int amount = 10) {
  Query tophash(db,"select hash,blocks from kv where hash!=0 and blocks>1 order by blocks desc limit ?");

  tophash.bind(amount);
  tophash.report(cout,"20,10");
}

void update(QddaDB& db) {
  db.update();
}

// safety guards against overwriting existing files or devices by SQLite
void ParseFileName(string& name) {
  char buf[160];
  if(!getcwd(buf, 160)) throw ERROR("Get current directory failed");
  string cwd = buf;
  if(name.empty()) name = homeDir() + "/qdda.db";
  if(name[0] != '/') name = cwd + "/" + name;
  while (name.find("//") < string::npos) {
    auto i=name.find("//");
    name.replace(i,2,"/");
  }
  if (!name.compare(0,4,"/dev")  )   throw ERROR("/dev not allowed in filename: " ) << name;
  if (!name.compare(0,5,"/proc") )   throw ERROR("/proc not allowed in filename: ") << name;
  if (!name.compare(0,4,"/sys")  )   throw ERROR("/sys not allowed in filename: " ) << name;
  if (!name.find_last_of("/\\"))     throw ERROR("root dir not allowed: "         ) << name;
  if(name.find(".db")>name.length()) name += ".db";
}

string genStagingName(string& name) {
  string tmpname;
  tmpname = name.substr(0,name.find(".db"));
  tmpname += "-staging.db";
  return tmpname;
}

void errorTest() {
  throw ERROR("Error test ") << "Extra info: user=" << getenv("USER"); // << std::endl;
}

const char* Compression::namelist[8] = { "none", "lz4", "deflate"};

void Compression::setMethod(const std::string& in, int interval) {
  setInterval(interval);
  setMethod(in);
}
  
void Compression::setMethod(const std::string& in) {
  for(int i=0; namelist[i]!=0;i++) {
    if(in==namelist[i]) {
      method = CompressMethod(i);
      return;
    }
  }
  throw ERROR("Invalid compression method: ") << in;
}

void Compression::setInterval(int p1) {
  if(p1<1 || p1>100) throw ERROR("Invalid compression interval: ") << p1;
  interval = p1;
}

/*******************************************************************************
 * Main section - process options etc
 ******************************************************************************/

int main(int argc, char** argv) {
  Parameters parameters = {};
  Options    opts = {};

  // set default values
  parameters.workers   = cpuCount();
  parameters.readers   = kmax_reader_threads;
  parameters.bandwidth = kdefault_bandwidth;
  parameters.array     = kdefault_array;

  Parameters& p = parameters; // shorthand alias
  Options& o = opts;
  try {
    LongOptions opts;
    opts.add("version"  ,'V', ""           , showversion,  "show version and copyright info");
    opts.add("help"     ,'h', ""           , o.do_help,    "show usage");
    opts.add("man"      ,'m', ""           , manpage,      "show detailed manpage");
    opts.add("db"       ,'d', "<file>"     , o.dbname,     "database file path (default $HOME/qdda.db)");
    opts.add("append"   ,'a', ""           , o.append,     "Append data instead of deleting database");
    opts.add("delete"   , 0 , ""           , o.do_delete,  "Delete database");
    opts.add("quiet"    ,'q', ""           , g_quiet,      "Don't show progress indicator or intermediate results");
    opts.add("bandwidth",'b', "<mb/s>"     , p.bandwidth,  "Throttle bandwidth in MB/s (default 200, 0=disable)");
    opts.add("array"    , 0 , "<id|def>"   , p.array,      "set array type or custom definition <x1|x2|vmax1|definition>");
    opts.add("compress" , 0 , "<method>"   , o.compress,   "set compression method (lz4|deflate, default=lz4)");
    opts.add("interval" , 0 , "<n>"        , o.interval,   "set compression sample interval (sample 1 per n blocks)");
    opts.add("buckets"  , 0 , "<list>"     , p.buckets,    "set list of compress buckets");
    opts.add("list"     ,'l', ""           , showlist,     "list supported array types and custom definition options");
    opts.add("detail"   ,'x', ""           , p.detail,     "Detailed report (file info and dedupe/compression histograms)");
    opts.add("dryrun"   ,'n', ""           , p.dryrun,     "skip staging db updates during scan");
    opts.add("purge"    , 0 , ""           , o.do_purge,   "Reclaim unused space in database (sqlite vacuum)");
    opts.add("import"   , 0 , "<file>"     , o.import,     "import another database (must have compatible metadata)");
    opts.add("cputest"  , 0 , ""           , o.do_cputest, "Single thread CPU performance test");
    opts.add("nomerge"  , 0 , ""           , p.skip,       "Skip staging data merge and reporting, keep staging database");
    opts.add("debug"    , 0 , ""           , g_debug,      "Enable debug output");
    opts.add("queries"  , 0 , ""           , g_query,      "Show SQLite queries and results"); // --show?
    opts.add("tmpdir"   , 0 , "<dir>"      , p.tmpdir,     "Set $SQLITE_TMPDIR for temporary files");
    opts.add("workers"  , 0 , "<wthreads>" , p.workers,    "number of worker threads");
    opts.add("readers"  , 0 , "<rthreads>" , p.readers,    "(max) number of reader threads");
    opts.add("findhash" , 0 , "<hash>"     , p.searchhash, "find blocks with hash=<hash> in staging db");
    opts.add("tophash"  , 0 , "<num>"      , p.tophash,    "show top <num> hashes by refcount");
    opts.add("squash"   , 0 , ""           , p.squash,     "set all refcounts to 1");
    opts.add("mandump"  , 0 , ""           , o.do_mandump, "dump raw manpage to stdout");
    opts.add("demo"     , 0 , ""           , rundemo,      "show quick demo");
#ifdef __DEBUG
    opts.add("update"   , 0 , ""           , o.do_update,  "update temp tables (debug only!)");
    opts.add("buffers"  , 0 , "<buffers>"  , p.buffers,    "number of buffers (debug only!)");
    opts.add("extest"   , 0 , ""           , errorTest,    "Test error handling");
#endif

    int rc=opts.parse(argc,argv);
    if(rc) return 0; // opts.parse executed a message function
  
    if(o.do_help)         { showhelp(opts); return 0; }
    else if(o.do_mandump) { mandump(opts); return 0; }
    
    if(!p.tmpdir.empty()) setenv("SQLITE_TMPDIR",p.tmpdir.c_str(),1);
  
    showtitle();
    ParseFileName(o.dbname);
    p.stagingname = genStagingName(o.dbname);
    
    if(o.do_delete)  {
      if(!g_quiet) cout << "Deleting database " << o.dbname << endl;
      Database::deletedb(o.dbname); return 0;
    }

    if     (p.array == "x1")    { p.blocksize = 8;   p.compression.setMethod("lz4",1);   p.buckets="2+4+8" ; }
    else if(p.array == "x2")    { p.blocksize = 16;  p.compression.setMethod("lz4",1);   p.buckets="1,2,3,4,5,6,7,8,9,10,11,12,13,15,16"; }
    else if(p.array == "vmax1") { p.blocksize = 128; p.compression.setMethod("deflate",20); p.buckets="8,16,24,32.40,48,56,64,72,80,88,96,104,112,120,128"; }
    
    if(!o.compress.empty()) p.compression.setMethod(o.compress);
    if(o.interval) p.compression.setInterval(o.interval);

  }
  catch (Fatal& e) { e.print(); return 10; }

  v_FileData filelist;

  try {
    // Build filelist
    if(optind<argc || !isatty(fileno(stdin)) ) {
      if (!isatty(fileno(stdin)))
        filelist.push_back(FileData("/dev/stdin"));
      for (int i = optind; i < argc; ++i)
        filelist.push_back(FileData(argv[i]));
      if(!o.append) { // not appending -> delete old database
        if(!g_quiet) cout << "Creating new database " << o.dbname << endl;
        Database::deletedb(o.dbname);
        QddaDB::createdb(o.dbname);
      }
    }
    if(o.do_cputest && !o.append) {
      if(!g_quiet) cout << "Creating new database " << o.dbname << endl;
      Database::deletedb(o.dbname);
      QddaDB::createdb(o.dbname);
    }
    if(!Database::exists(o.dbname)) QddaDB::createdb(o.dbname);
    QddaDB db(o.dbname);
    
    db.setmetadata(p.blocksize,p.compression,p.array.c_str(),p.buckets);

    if(filelist.size()>0) 
      analyze(filelist, db, parameters);

    if(g_abort) return 1;

    if(o.do_purge)             { db.vacuum();           }
    else if(!o.import.empty()) { import(db,o.import);   }
    else if(o.do_cputest)      { cputest(db,p) ;        }
    else if(o.do_update)       { update(db) ;           }
    else if(p.searchhash!=0)   { findhash(p);           }
    else if(p.tophash!=0)      { tophash(db,p.tophash); }
    else if(p.squash!=0)       { db.squash();           }
    else {
      if(!parameters.skip)     { merge(db,parameters); }
      if(parameters.detail)    { reportDetail(db); }
      else if (!p.skip)        { report(db); }
    }
  }
  catch (std::bad_alloc& e) { ERROR("Out of memory").print(); return -1; }
  catch (Fatal& e) { e.print(); return -1; }

  return 0;  
}

