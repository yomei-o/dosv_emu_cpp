// The guest's file world. DOS hands programs small integer handles; this maps them
// onto host files under a single root directory that plays the part of drive C:.
// DOS paths (drive letters, backslashes, case-insensitive 8.3 names) are translated
// to host paths. Handles 0/1/2 are the console.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace dosemu {

class DosFiles {
public:
    explicit DosFiles(std::string root) : root_(std::move(root)) {}

    // Returns a handle (>=0) or a negative DOS error code.
    int  open(const std::string& dos_path, int access);   // access: 0=r 1=w 2=rw
    int  create(const std::string& dos_path, int attr);
    int  close(int handle);
    int  read(int handle, uint8_t* dst, int len);         // bytes read, or negative error
    int  write(int handle, const uint8_t* src, int len);  // bytes written, or negative error
    // A write of zero bytes truncates the file at the current position — DOS has no other
    // way to shorten a file, so this is the idiom every program uses, and it is how Watcom's
    // wcc finishes rewriting an object file it opened read/write. Without it the tail of
    // whatever was there before survives: a 32-bit object left in place by an earlier
    // wpp386 run made wlink report `E3146: HELLO.obj is an invalid object file` about an
    // object wcc had just written correctly.
    int  truncate_here(int handle);                       // 0, or a negative error
    long seek(int handle, long offset, int whence);        // new position, or negative error
    int  remove(const std::string& dos_path);
    bool exists(const std::string& dos_path);
    int  dup(int handle);
    int  dup2(int from, int to);

    // Console handles report themselves so the DOS layer can route to output().
    bool is_console(int handle, int& con_fd) const;

    void set_cwd(const std::string& dos_dir) { cwd_ = dos_dir.empty() ? "\\" : dos_dir; }
    std::string cwd() const { return cwd_; }
    // Change directory (relative/absolute, with . and ..). Returns true on success.
    bool chdir(const std::string& dos_dir);
    // Normalise a DOS path against the current directory into "\A\B" form.
    std::string normalize(const std::string& dos_path) const;
    std::string host_path(const std::string& dos_path);   // exposed for the loader/EXEC

private:
    struct Entry { std::FILE* fp = nullptr; bool console = false; int con_fd = 0; bool used = false; };
    std::string root_;
    std::string cwd_ = "\\";
    std::map<int, Entry> h_ = {
        {0, {nullptr, true, 0, true}}, {1, {nullptr, true, 1, true}}, {2, {nullptr, true, 2, true}},
    };
    int alloc();
};

}  // namespace dosemu
