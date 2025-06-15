#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"
static bool walk_directory(
    File_Paths* dirs,
    File_Paths* c_sources,
    const char* path
) {
    DIR *dir = opendir(path);
    if(!dir) {
        nob_log(NOB_ERROR, "Could not open directory %s: %s", path, strerror(errno));
        return false;
    }
    errno = 0;
    struct dirent *ent;
    while((ent = readdir(dir))) {
        if(strcmp(ent->d_name, "..") == 0 || strcmp(ent->d_name, ".") == 0) continue;
        const char* fext = nob_get_ext(ent->d_name);
        size_t temp = nob_temp_save();
        const char* p = nob_temp_sprintf("%s/%s", path, ent->d_name); 
        Nob_File_Type type = nob_get_file_type(p);
        if(type == NOB_FILE_DIRECTORY) {
            da_append(dirs, p);
            if(!walk_directory(dirs, c_sources, p)) {
                closedir(dir);
                return false;
            }
            continue;
        }
        if(strcmp(fext, "c") == 0) {
            nob_da_append(c_sources, p);
            continue;
        }
        nob_temp_rewind(temp);
    }
    closedir(dir);
    return true;
}
int main(int argc, char** argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);
    char* cc = getenv("CC");
    // TODO: automatic checks for the compiler 
    // available on the system. Maybe default to clang on bimbows
    if(!cc) cc = "cc";
    setenv("CC", cc, 0);
    char* bindir = getenv("BINDIR");
    if(!bindir) bindir = "bin";
    setenv("BINDIR", bindir, 0);
    if(!mkdir_if_not_exists(bindir)) return 1;

    // Building Raylib
    Cmd cmd = { 0 };
    if(!mkdir_if_not_exists(temp_sprintf("%s/jesse", bindir))) return 1;


    File_Paths dirs = { 0 }, c_sources = { 0 };
    const char* src_dir = "src";
    size_t src_prefix_len = strlen(src_dir)+1;
    if(!walk_directory(&dirs, &c_sources, src_dir)) return 1;
    for(size_t i = 0; i < dirs.count; ++i) {
        if(!mkdir_if_not_exists(temp_sprintf("%s/jesse/%s", bindir, dirs.items[i]))) return 1;
    }
    File_Paths objs = { 0 };
    String_Builder stb = { 0 };
    File_Paths pathb = { 0 };
    for(size_t i = 0; i < c_sources.count; ++i) {
        const char* src = c_sources.items[i];
        const char* out = temp_sprintf("%s/jesse/%.*s.o", bindir, (int)(strlen(src + src_prefix_len)-2), src + src_prefix_len);
        da_append(&objs, out);
        if(!nob_c_needs_rebuild1(&stb, &pathb, out, src)) continue;
        // C compiler
        cmd_append(&cmd, cc);
        // Warnings
        cmd_append(&cmd,
            "-Wall",
            "-Wextra",
            "-Wno-unused-function",
        #if 1
            "-Werror",
        #endif
        );
        // Include directories 
        cmd_append(&cmd,
            "-I", "include",
        );
        // Actual compilation
        cmd_append(&cmd,
            "-MD", "-O1", "-g", "-c",
            src,
            "-o", out,
        );
        if(!cmd_run_sync_and_reset(&cmd)) return 1;
    }
    const char* exe = "jesse";
    if(needs_rebuild(exe, objs.items, objs.count)) {
        cmd_append(&cmd, cc, "-o", exe);
        da_append_many(&cmd, objs.items, objs.count);
        // Vendor libraries we link with
        cmd_append(&cmd, "-lm");
        if(!cmd_run_sync_and_reset(&cmd)) return 1;
    }
}
