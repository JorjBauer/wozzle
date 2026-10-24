// wozfuse: mount a DOS 3.3, ProDOS or Pascal disk image as a read-only
// filesystem.
//
//   wozfuse [-d | -p | -P] [-c] [-v] <image> <mountpoint>
//
// Files read back as their contents (the same thing wozit's cpout copies
// out), and Apple II file types ride along as extended attributes:
//   user.apple2.filetype   ProDOS file type, two hex digits (DOS 3.3 types
//                          are mapped onto ProDOS ones; Pascal gives its
//                          file kind)
//   user.apple2.auxtype    aux type, four hex digits
//   user.apple2.access     ProDOS access bits, two hex digits
// With -c, each directory also gets a generated _CATALOG.TXT listing them.
//
// It runs in the foreground; Ctrl-C (or umount) unmounts it. It's built on
// FUSE 3: libfuse on Linux, macFUSE on a Mac, and WinFsp's FUSE layer on
// Windows (which I haven't been able to try).
//
// The whole catalog is read once, when the image is mounted. Nothing can
// change it underneath us (it's read-only, and the library is
// single-threaded, so FUSE runs single-threaded too), and on DOS 3.3 the
// only way to know a file's length is to read the file.

// Stock libfuse signatures on macOS too, so one source serves all three
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "wozlib.h"

using namespace std;

#ifdef _WIN32
// WinFsp's FUSE layer has its own names for the POSIX types
typedef struct fuse_stat hostStat;
typedef fuse_off_t hostOff;
typedef struct fuse_statvfs hostStatvfs;
#else
typedef struct stat hostStat;
typedef off_t hostOff;
typedef struct statvfs hostStatvfs;
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#define CATALOG_NAME "_CATALOG.TXT"

struct Node {
  wozEntry entry;
  string applePath;          // path inside the image
  bool isDir;
  bool isCatalog;            // a generated _CATALOG.TXT
  string catalogText;
  vector<string> children;   // host names, for a directory
};

static wozImage *image = NULL;
static wozVolumeInfo volume;
static time_t imageTime = 0;   // for entries with no date of their own
static uid_t owner = 0;
static gid_t group = 0;
static bool verbose = false;
static bool makeCatalogs = false;
static map<string, Node> nodes; // by host path ("/", "/SUB/FILE")

// The bytes a host filename can't hold, or can't hold on one of the three
// platforms, become %XX. Only DOS 3.3 names have any of these, but they
// can have nearly anything.
static string hostName(const char *appleName)
{
  string out;
  size_t len = strlen(appleName);
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)appleName[i];
    bool trailingDot = (c == '.' && i == len - 1);
    if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|%", c) || trailingDot) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    } else {
      out += (char)c;
    }
  }
  if (out == "." || out == "..") out = "%2E" + out.substr(1);
  return out;
}

static const char *typeName(uint8_t fileType)
{
  static char buf[8];
  if (volume.fsType == WOZ_FS_PASCAL) {
    static const char *kinds[] = { "UNTYPED", "BAD", "CODE", "TEXT", "INFO",
                                   "DATA", "GRAF", "FOTO", "SECDIR" };
    if (fileType < sizeof(kinds)/sizeof(kinds[0])) return kinds[fileType];
  } else if (volume.fsType == WOZ_FS_DOS33) {
    switch (fileType) {
    case 0x04: return "T";
    case 0x06: return "B";
    case 0xFA: return "I";
    case 0xFC: return "A";
    case 0xFE: return "R";
    case 0xFF: return "S";
    }
  } else {
    switch (fileType) {
    case 0x04: return "TXT";
    case 0x06: return "BIN";
    case 0x0F: return "DIR";
    case 0x19: return "ADB";
    case 0x1A: return "AWP";
    case 0x1B: return "ASP";
    case 0xEF: return "PAS";
    case 0xF0: return "CMD";
    case 0xFA: return "INT";
    case 0xFB: return "IVR";
    case 0xFC: return "BAS";
    case 0xFD: return "VAR";
    case 0xFE: return "REL";
    case 0xFF: return "SYS";
    }
  }
  snprintf(buf, sizeof(buf), "$%02X", fileType);
  return buf;
}

static void formatDate(int64_t when, char *buf, size_t len)
{
  if (when < 0) {
    snprintf(buf, len, "%s", "<no date>");
    return;
  }
  time_t t = (time_t)when;
  struct tm *lt = localtime(&t);
  if (!lt || !strftime(buf, len, "%Y-%m-%d %H:%M", lt))
    snprintf(buf, len, "%s", "?");
}

static string catalogFor(const vector<wozEntry> &ents)
{
  string s;
  char line[160];
  bool dates = (volume.fsType == WOZ_FS_PRODOS);
  snprintf(line, sizeof(line), "%-30s %-7s %-5s %8s %6s%s\n", "NAME", "TYPE",
           "AUX", "LENGTH", volume.unitSize == 256 ? "SECTS" : "BLOCKS",
           dates ? " ACCESS MODIFIED         CREATED" : "");
  s += line;
  for (size_t i = 0; i < ents.size(); i++) {
    const wozEntry &e = ents[i];
    char aux[8];
    snprintf(aux, sizeof(aux), "$%04X", e.auxType);
    snprintf(line, sizeof(line), "%-30s %-7s %-5s %8u %6u", e.name,
             typeName(e.fileType), aux, (unsigned)e.length,
             (unsigned)e.unitsUsed);
    s += line;
    if (dates) {
      char m[24], c[24];
      formatDate(e.modified, m, sizeof(m));
      formatDate(e.created, c, sizeof(c));
      snprintf(line, sizeof(line), "  $%02X   %-16s %s", e.access, m, c);
      s += line;
    }
    s += '\n';
  }
  return s;
}

// Read one directory of the image into `nodes`, and everything under it
static bool loadDirectory(const string &hostPath, const string &applePath)
{
  int n = wozImageList(image, applePath.c_str(), NULL, 0);
  if (n < 0) {
    fprintf(stderr, "Can't read directory '%s': %s\n",
            applePath.empty() ? "/" : applePath.c_str(), wozLastError());
    return false;
  }
  vector<wozEntry> ents(n);
  if (n) wozImageList(image, applePath.c_str(), &ents[0], n);

  for (int i = 0; i < n; i++) {
    string name = hostName(ents[i].name);
    string childHost = (hostPath == "/" ? "/" : hostPath + "/") + name;
    if (nodes.count(childHost)) {
      // Escaping can't produce a duplicate, but a damaged catalog can hold
      // the same name twice
      fprintf(stderr, "WARNING: '%s' appears twice; showing the first one\n",
              ents[i].name);
      continue;
    }
    Node child;
    child.entry = ents[i];
    child.applePath = applePath.empty() ? string(ents[i].name)
                                        : applePath + "/" + ents[i].name;
    child.isDir = ents[i].isDirectory;
    child.isCatalog = false;
    nodes[childHost] = child;
    nodes[hostPath].children.push_back(name);
    if (child.isDir && !loadDirectory(childHost, child.applePath))
      return false;
  }

  if (makeCatalogs) {
    string catHost = (hostPath == "/" ? "/" : hostPath + "/") + CATALOG_NAME;
    if (nodes.count(catHost)) {
      fprintf(stderr, "WARNING: '%s' already has a file named %s; not "
              "generating a catalog there\n", hostPath.c_str(), CATALOG_NAME);
    } else {
      Node cat;
      memset(&cat.entry, 0, sizeof(cat.entry));
      cat.entry.created = cat.entry.modified = -1;
      cat.isDir = false;
      cat.isCatalog = true;
      cat.catalogText = catalogFor(ents);
      cat.entry.length = (uint32_t)cat.catalogText.size();
      nodes[catHost] = cat;
      nodes[hostPath].children.push_back(CATALOG_NAME);
    }
  }
  return true;
}

static Node *findNode(const char *path)
{
  map<string, Node>::iterator it = nodes.find(path);
  return it == nodes.end() ? NULL : &it->second;
}

// ---- FUSE operations ----

static int wf_getattr(const char *path, hostStat *st, struct fuse_file_info *fi)
{
  (void)fi;
  Node *n = findNode(path);
  if (!n) return -ENOENT;
  memset(st, 0, sizeof(*st));
  if (n->isDir) {
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
  } else {
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = n->entry.length;
    st->st_blocks = ((uint64_t)n->entry.unitsUsed * volume.unitSize + 511) / 512;
  }
  // Whoever mounted it owns everything, as with any user mount
  st->st_uid = owner;
  st->st_gid = group;
  time_t m = n->entry.modified >= 0 ? (time_t)n->entry.modified : imageTime;
  st->st_mtime = st->st_ctime = st->st_atime = m;
  return 0;
}

static int wf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      hostOff offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
  (void)offset; (void)fi; (void)flags;
  Node *n = findNode(path);
  if (!n) return -ENOENT;
  if (!n->isDir) return -ENOTDIR;
  filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
  filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
  for (size_t i = 0; i < n->children.size(); i++)
    filler(buf, n->children[i].c_str(), NULL, 0, (enum fuse_fill_dir_flags)0);
  return 0;
}

struct OpenFile {
  uint8_t *data;
  uint32_t len;
};

static int wf_open(const char *path, struct fuse_file_info *fi)
{
  Node *n = findNode(path);
  if (!n) return -ENOENT;
  if (n->isDir) return -EISDIR;
  if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;

  OpenFile *of = new OpenFile;
  of->data = NULL;
  of->len = 0;
  if (n->isCatalog) {
    of->len = (uint32_t)n->catalogText.size();
    of->data = (uint8_t *)wozMalloc(of->len ? of->len : 1);
    if (of->data) memcpy(of->data, n->catalogText.data(), of->len);
  } else if (!wozImageReadFile(image, n->applePath.c_str(), false, false,
                               &of->data, &of->len)) {
    fprintf(stderr, "Can't read '%s': %s\n", n->applePath.c_str(),
            wozLastError());
    delete of;
    return -EIO;
  }
  fi->fh = (uint64_t)(uintptr_t)of;
  return 0;
}

static int wf_read(const char *path, char *buf, size_t size, hostOff offset,
                   struct fuse_file_info *fi)
{
  (void)path;
  OpenFile *of = (OpenFile *)(uintptr_t)fi->fh;
  if (!of) return -EBADF;
  if (offset < 0) return -EINVAL;
  if ((uint64_t)offset >= of->len) return 0;
  size_t avail = of->len - (size_t)offset;
  if (size > avail) size = avail;
  memcpy(buf, of->data + offset, size);
  return (int)size;
}

static int wf_release(const char *path, struct fuse_file_info *fi)
{
  (void)path;
  OpenFile *of = (OpenFile *)(uintptr_t)fi->fh;
  if (of) {
    wozFree(of->data);
    delete of;
  }
  fi->fh = 0;
  return 0;
}

static int wf_statfs(const char *path, hostStatvfs *sv)
{
  (void)path;
  memset(sv, 0, sizeof(*sv));
  sv->f_bsize = sv->f_frsize = volume.unitSize;
  sv->f_blocks = volume.totalUnits;
  sv->f_bfree = sv->f_bavail = volume.freeUnits;
  sv->f_namemax = 30;
  return 0;
}

// The attributes a node has, as name/value pairs
static vector<pair<string, string> > attributesOf(Node *n)
{
  vector<pair<string, string> > a;
  if (n->isCatalog || n->applePath.empty()) return a;
  char v[8];
  snprintf(v, sizeof(v), "%02X", n->entry.fileType);
  a.push_back(make_pair(string("user.apple2.filetype"), string(v)));
  snprintf(v, sizeof(v), "%04X", n->entry.auxType);
  a.push_back(make_pair(string("user.apple2.auxtype"), string(v)));
  if (volume.fsType == WOZ_FS_PRODOS) {
    snprintf(v, sizeof(v), "%02X", n->entry.access);
    a.push_back(make_pair(string("user.apple2.access"), string(v)));
  }
  return a;
}

static int wf_getxattr(const char *path, const char *name, char *value,
                       size_t size)
{
  Node *n = findNode(path);
  if (!n) return -ENOENT;
  vector<pair<string, string> > a = attributesOf(n);
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].first != name) continue;
    const string &v = a[i].second;
    // A zero size asks how big the value is
    if (size == 0) return (int)v.size();
    if (size < v.size()) return -ERANGE;
    memcpy(value, v.data(), v.size());
    return (int)v.size();
  }
  return -ENOATTR;
}

static int wf_listxattr(const char *path, char *list, size_t size)
{
  Node *n = findNode(path);
  if (!n) return -ENOENT;
  vector<pair<string, string> > a = attributesOf(n);
  // NUL-separated names; a zero size asks how much room they need
  size_t need = 0;
  for (size_t i = 0; i < a.size(); i++) need += a[i].first.size() + 1;
  if (size == 0) return (int)need;
  if (size < need) return -ERANGE;
  char *p = list;
  for (size_t i = 0; i < a.size(); i++) {
    memcpy(p, a[i].first.c_str(), a[i].first.size() + 1);
    p += a[i].first.size() + 1;
  }
  return (int)need;
}

static void *wf_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
  (void)conn;
  // Our inode numbers mean nothing; let FUSE pick, and let the kernel
  // cache freely, since nothing ever changes
  cfg->use_ino = 0;
  cfg->entry_timeout = cfg->attr_timeout = 3600;
  return NULL;
}

static void quietSink(wozMsgLevel level, const char *msg, void *ctx)
{
  (void)ctx;
  if (level >= WOZ_MSG_WARNING || verbose)
    fprintf(stderr, "%s\n", msg);
}

static void usage(const char *name)
{
  fprintf(stderr, "Usage: %s [-d | -p | -P] [-c] [-v] <image> <mountpoint>\n\n", name);
  fprintf(stderr, "  -d    the image is DOS 3.3\n");
  fprintf(stderr, "  -p    the image is ProDOS\n");
  fprintf(stderr, "  -P    the image is UCSD p-System / Apple Pascal\n");
  fprintf(stderr, "        (with none of these, it's worked out from the image)\n");
  fprintf(stderr, "  -c    put a generated %s in each directory\n", CATALOG_NAME);
  fprintf(stderr, "  -v    verbose\n\n");
  fprintf(stderr, "  The mount is read-only and runs in the foreground; Ctrl-C\n");
  fprintf(stderr, "  or umount unmounts it.\n");
}

int main(int argc, char *argv[])
{
  wozFsType type = WOZ_FS_UNKNOWN;
  int c;
  while ((c = getopt(argc, argv, "dpPcvh?")) != -1) {
    switch (c) {
    case 'd': type = WOZ_FS_DOS33; break;
    case 'p': type = WOZ_FS_PRODOS; break;
    case 'P': type = WOZ_FS_PASCAL; break;
    case 'c': makeCatalogs = true; break;
    case 'v': verbose = true; break;
    default:
      usage(argv[0]);
      return 1;
    }
  }
  if (argc - optind != 2) {
    usage(argv[0]);
    return 1;
  }
  const char *imagePath = argv[optind];
  const char *mountPoint = argv[optind + 1];

  wozInit();
  wozSetMsgSink(quietSink, NULL);

  image = wozImageOpen(imagePath, type);
  if (!image) {
    fprintf(stderr, "Can't open '%s': %s\n", imagePath, wozLastError());
    return 1;
  }
  if (type != WOZ_FS_UNKNOWN && !wozImageProbe(image)) {
    fprintf(stderr, "WARNING: '%s' doesn't look like a %s volume; mounting "
            "it anyway\n", imagePath,
            type == WOZ_FS_DOS33 ? "DOS 3.3" : type == WOZ_FS_PRODOS
            ? "ProDOS" : "Pascal");
  }
  if (!wozImageGetVolumeInfo(image, &volume)) {
    fprintf(stderr, "Can't read the volume on '%s': %s\n", imagePath,
            wozLastError());
    return 1;
  }
  struct stat ist;
  if (stat(imagePath, &ist) == 0) imageTime = ist.st_mtime;
  owner = getuid();
  group = getgid();

  Node root;
  memset(&root.entry, 0, sizeof(root.entry));
  root.entry.created = root.entry.modified = -1;
  root.isDir = true;
  root.isCatalog = false;
  nodes["/"] = root;
  if (!loadDirectory("/", ""))
    return 1;

  // The volume's own name, where it has one, names the mount
  string volName = volume.volumeName[0] ? string(volume.volumeName)
                                        : string("DOS 3.3 volume ") +
                                          to_string(volume.dosVolume);
  string opts = "ro,fsname=wozfuse,subtype=wozfuse";
#ifdef __APPLE__
  // No ._ AppleDouble files or .DS_Store, which a read-only image can't
  // hold anyway
  opts += ",noappledouble,volname=" + volName;
#endif
  (void)volName;

  struct fuse_operations ops;
  memset(&ops, 0, sizeof(ops));
  ops.init = wf_init;
  ops.getattr = wf_getattr;
  ops.readdir = wf_readdir;
  ops.open = wf_open;
  ops.read = wf_read;
  ops.release = wf_release;
  ops.statfs = wf_statfs;
  ops.getxattr = wf_getxattr;
  ops.listxattr = wf_listxattr;

  // Foreground and single-threaded: the library has no locking
  vector<string> fargs;
  fargs.push_back(argv[0]);
  fargs.push_back("-f");
  fargs.push_back("-s");
  fargs.push_back("-o");
  fargs.push_back(opts);
  fargs.push_back(mountPoint);
  vector<char *> fargv;
  for (size_t i = 0; i < fargs.size(); i++)
    fargv.push_back((char *)fargs[i].c_str());
  fargv.push_back(NULL);

  int ret = fuse_main((int)fargs.size(), &fargv[0], &ops, NULL);
  wozImageClose(image);
  return ret;
}
