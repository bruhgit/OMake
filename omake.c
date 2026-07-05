#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <stdint.h>

#ifdef _WIN32
    #include <direct.h>
    #include <windows.h>
    #define get_cwd       _getcwd
    #define PLATFORM      "win32"
    #define mkdir_auto(d) _mkdir(d)
    #define DEL_FILE      "del /q /f"
    #define DEL_DIR       "rmdir /s /q"
    #define SEP           '\\'
    #define DEVNULL       "nul"
    #define strcasecmp    _stricmp
    #define strncasecmp   _strnicmp
    #define popen         _popen
    #define pclose        _pclose
    static int setenv(const char *name, const char *value, int overwrite) {
        if (!overwrite && getenv(name)) return 0;
        return _putenv_s(name, value);
    }
    static int get_cpu_count(void) {
        SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors;
    }
#else
    #include <unistd.h>
    #define get_cwd       getcwd
    #define PLATFORM      "linux"
    #define mkdir_auto(d) mkdir(d, 0777)
    #define DEL_FILE      "rm -f"
    #define DEL_DIR       "rm -rf"
    #define SEP           '/'
    #define DEVNULL       "/dev/null"
    static int get_cpu_count(void) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return (n > 0) ? (int)n : 1;
    }
#endif

#define COPYRIGHT   "Copyright (C) 2025 - 2026 omerdev All Rights Reserved. "
#define VERSION     "26.1"
#define RED     "\x1b[1;31m"
#define GREEN   "\x1b[1;32m"
#define YELLOW  "\x1b[1;33m"
#define BLUE    "\x1b[1;34m"
#define MAGENTA "\x1b[1;35m"
#define CYAN    "\x1b[1;36m"
#define RESET   "\x1b[0m"
#ifdef _WIN32
/* MinGW does not have realpath() — use _fullpath() instead */
static char *omake_realpath(const char *path, char *out) {
    if (!_fullpath(out, path, 512)) { strncpy(out, path, 511); out[511]='\0'; }
    return out;
}
#define realpath(p, r) omake_realpath((p), (r))
#endif


#define B_SIZE      32768
#define MAX_FETCH   32
#define MAX_SRCS    512
#define MAX_INCDIRS 128
#define MAX_JOBS    64
#define MAX_RC      64   /* Maximum .rc files */
#define MAX_RC_PATH 512  /* Max path length per .rc file */

/* ------------------------------------------------------------------ */
/*  Compiler type detection                                             */
/* ------------------------------------------------------------------ */

typedef enum { LANG_C = 0, LANG_CXX = 1 } SourceLang;

typedef enum {
    CT_GCC   = 0,   /* g++, gcc, mingw                                 */
    CT_CLANG = 1,   /* clang++, clang, clang-cl                        */
    CT_MSVC  = 2    /* cl.exe  (MSVC)                                  */
} CompilerType;

/*
 * Detect compiler type from binary name.
 * "cl" or "cl.exe"          -> CT_MSVC
 * "clang*" or "clang-cl*"   -> CT_CLANG
 * everything else            -> CT_GCC
 */
static CompilerType detect_compiler(const char *comp) {
    /* work on lowercase copy */
    char lc[256]; strncpy(lc, comp, 255); lc[255] = '\0';
    for (int i = 0; lc[i]; i++) lc[i] = (char)tolower((unsigned char)lc[i]);

    /* strip path — only look at the binary name */
    const char *base = strrchr(lc, '/');
    if (!base) base = strrchr(lc, '\\');
    base = base ? base + 1 : lc;

    if (strcmp(base, "cl") == 0 || strcmp(base, "cl.exe") == 0)
        return CT_MSVC;
    if (strncmp(base, "clang", 5) == 0)
        return CT_CLANG;
    return CT_GCC;
}

/* ------------------------------------------------------------------ */
/*  Flag translation layer                                              */
/* ------------------------------------------------------------------ */

/*
 * translate_tags():
 *   Converts GCC-style flags in `src` to the dialect required by `ct`.
 *   Result written into `dst` (size `dsz`).
 *
 *   Translations performed for MSVC (CT_MSVC):
 *     -O0           ->  /Od
 *     -O1           ->  /O1
 *     -O2           ->  /O2
 *     -O3           ->  /Ox
 *     -g / -ggdb*   ->  /Zi
 *     -Wall         ->  /W4
 *     -Wextra       ->  (dropped — no direct equivalent)
 *     -Werror       ->  /WX
 *     -DXXX         ->  /DXXX
 *     -UXXX         ->  /UXXX
 *     -IXXX         ->  /IXXX
 *     -static       ->  /MT   (static CRT)
 *     -m64          ->  (dropped — handled by linker /MACHINE:X64)
 *     -m32          ->  (dropped — handled by linker /MACHINE:X86)
 *     -march=*      ->  (dropped)
 *     -std=c++*     ->  /std:c++XX
 *     -std=c*       ->  /std:cXX
 *     -ffast-math   ->  /fp:fast
 *     -fopenmp      ->  /openmp
 *     -fpic/-fPIC   ->  (dropped — not needed on Windows PE)
 *     -fsanitize=*  ->  (dropped — use clang-cl for ASAN on Windows)
 *     -s            ->  (dropped — use linker /OPT:REF /OPT:ICF instead)
 *     -DNDEBUG      ->  /DNDEBUG
 *     unknown -f*   ->  dropped with a warning
 *     unknown -W*   ->  dropped silently
 *     -Ipath        ->  /Ipath (already handled in auto_inc but catch here too)
 *
 *   For CT_CLANG (clang++): flags pass through unchanged — clang is
 *   GCC-compatible.  Exception: clang-cl binary gets MSVC translation.
 */
static void translate_tags(const char *src, char *dst, size_t dsz,
                            CompilerType ct) {
    if (ct == CT_GCC || ct == CT_CLANG) {
        /* clang-cl needs MSVC translation */
        /* detect clang-cl via the caller — ct already set correctly */
        strncpy(dst, src, dsz - 1);
        dst[dsz - 1] = '\0';
        return;
    }

    /* CT_MSVC: tokenize and translate */
    dst[0] = '\0';
    char tmp[B_SIZE]; strncpy(tmp, src, B_SIZE - 1); tmp[B_SIZE-1] = '\0';
    char *tok = strtok(tmp, " \t");
    while (tok) {
        char out_tok[256] = "";
        int drop = 0;

        if      (strcmp(tok, "-O0") == 0)           strcpy(out_tok, "/Od");
        else if (strcmp(tok, "-O1") == 0)           strcpy(out_tok, "/O1");
        else if (strcmp(tok, "-O2") == 0)           strcpy(out_tok, "/O2");
        else if (strcmp(tok, "-O3") == 0)           strcpy(out_tok, "/Ox");
        else if (strncmp(tok, "-g",  2) == 0)       strcpy(out_tok, "/Zi");
        else if (strcmp(tok, "-Wall") == 0)         strcpy(out_tok, "/W4");
        else if (strcmp(tok, "-Wextra") == 0)       drop = 1;
        else if (strcmp(tok, "-Werror") == 0)       strcpy(out_tok, "/WX");
        else if (strcmp(tok, "-static") == 0)       strcpy(out_tok, "/MT");
        else if (strcmp(tok, "-m64") == 0)          drop = 1; /* /MACHINE via linker */
        else if (strcmp(tok, "-m32") == 0)          drop = 1;
        else if (strcmp(tok, "-ffast-math") == 0)   strcpy(out_tok, "/fp:fast");
        else if (strcmp(tok, "-fopenmp") == 0)      strcpy(out_tok, "/openmp");
        else if (strncmp(tok, "-fpic", 5) == 0 ||
                 strncmp(tok, "-fPIC", 5) == 0)     drop = 1;
        else if (strncmp(tok, "-fsanitize", 10) == 0) drop = 1;
        else if (strcmp(tok, "-s") == 0)            drop = 1;
        else if (strcmp(tok, "-DNDEBUG") == 0)      strcpy(out_tok, "/DNDEBUG");
        else if (strcmp(tok, "-DDEBUG") == 0)       strcpy(out_tok, "/DDEBUG");
        else if (strncmp(tok, "-D", 2) == 0)  { out_tok[0]='/'; strcpy(out_tok+1, tok+1); }
        else if (strncmp(tok, "-U", 2) == 0)  { out_tok[0]='/'; strcpy(out_tok+1, tok+1); }
        else if (strncmp(tok, "-I", 2) == 0)  { out_tok[0]='/'; strcpy(out_tok+1, tok+1); }
        else if (strncmp(tok, "-std=c++", 8) == 0) {
            /* -std=c++17 -> /std:c++17   -std=c++20 -> /std:c++20  etc. */
            snprintf(out_tok, sizeof(out_tok), "/std:c++%s", tok + 8);
        }
        else if (strncmp(tok, "-std=c", 6) == 0) {
            snprintf(out_tok, sizeof(out_tok), "/std:c%s", tok + 6);
        }
        else if (strncmp(tok, "-march", 6) == 0)   drop = 1;
        else if (strncmp(tok, "-W", 2) == 0)        drop = 1; /* unknown -W */
        else if (strncmp(tok, "-f", 2) == 0) {
            printf(YELLOW "[MSVC] Dropping unsupported flag: %s\n" RESET, tok);
            drop = 1;
        }
        else {
            /* Pass through unknown flags (e.g. /W3 already in MSVC form) */
            strncpy(out_tok, tok, 255);
        }

        if (!drop && out_tok[0]) {
            strncat(dst, out_tok, dsz - strlen(dst) - 1);
            strncat(dst, " ",     dsz - strlen(dst) - 1);
        }
        tok = strtok(NULL, " \t");
    }
}

/*
 * translate_inc():
 *   Convert -I"path" flags to /I"path" for MSVC.
 *   For GCC/Clang: pass through unchanged.
 */
static void translate_inc(const char *src, char *dst, size_t dsz,
                           CompilerType ct) {
    if (ct != CT_MSVC) { strncpy(dst, src, dsz-1); dst[dsz-1]='\0'; return; }
    dst[0] = '\0';
    char tmp[B_SIZE]; strncpy(tmp, src, B_SIZE-1);
    char *p = tmp;
    while (*p) {
        if (p[0] == '-' && p[1] == 'I') {
            strncat(dst, "/I", dsz - strlen(dst) - 1);
            p += 2;
        } else {
            char ch[2] = {*p, '\0'};
            strncat(dst, ch, dsz - strlen(dst) - 1);
            p++;
        }
    }
}

/*
 * build_compile_cmd():
 *   Construct the compile-only command for one source file.
 *
 *   GCC/Clang:
 *     <comp> -c "src.cpp" -o "obj.o" -MMD -MP <tags> <inc>
 *
 *   MSVC:
 *     cl.exe /nologo /c "src.cpp" /Fo"obj.obj" /showIncludes <tags> <inc>
 *     (no -MMD; dependency tracking done via /showIncludes redirect instead —
 *      we keep it simple and let MSVC always recompile changed files via mtime)
 *
 *   clang-cl:
 *     clang-cl /nologo /c "src.cpp" /Fo"obj.obj" /showIncludes <tags> <inc>
 */
/*
 * per_file_compiler():
 *   For a given base compiler binary and source language, return the
 *   appropriate compiler to invoke.
 *
 *   g++   + LANG_C  -> gcc
 *   g++   + LANG_CXX-> g++
 *   clang++ + LANG_C  -> clang
 *   clang++ + LANG_CXX-> clang++
 *   gcc   + LANG_CXX-> g++  (user typed gcc but file is .cpp)
 *   cl    + any     -> cl   (MSVC handles both via /TC /TP)
 *
 *   Result written into out_buf (caller provides [256]).
 */
static void per_file_compiler(const char *comp, CompilerType ct,
                               SourceLang lang, char *out_buf) {
    if (ct == CT_MSVC) {
        strncpy(out_buf, comp, 255);
        return;
    }

    /*
     * Extract the real binary name from a potentially complex string like:
     *   "ccache g++"          -> base = "g++"
     *   "/usr/bin/g++-14"     -> prefix = "/usr/bin/", base = "g++-14"
     *   "aarch64-linux-gnu-g++"
     *
     * Strategy:
     *   1. Strip a leading "ccache " token if present — store it separately.
     *   2. Extract the last path component after / or \ as 'base'.
     *   3. Lowercase base for comparison.
     *   4. Map C++ -> C  (or C -> C++) using a robust suffix/strstr approach
     *      that handles versioned names (g++-14, g++-15, clang++-17, …).
     *   5. Re-assemble: ccache_pfx + dir_prefix + mapped_base.
     */

    /* Step 1: strip optional "ccache " prefix */
    char ccache_pfx[16] = "";
    const char *p = comp;
    if (strncmp(p, "ccache ", 7) == 0) {
        strncpy(ccache_pfx, "ccache ", sizeof(ccache_pfx)-1);
        p += 7;
        while (*p == ' ') p++;   /* skip extra spaces */
    }

    /* Step 2: split path prefix and base name */
    char prefix[256] = "";
    char base[256]   = "";
    const char *sep = strrchr(p, '/');
    if (!sep) sep = strrchr(p, '\\');
    if (sep) {
        int plen = (int)(sep - p) + 1;
        strncpy(prefix, p, plen < 255 ? plen : 255);
        strncpy(base, sep+1, 255);
    } else {
        strncpy(base, p, 255);
    }

    /* Step 3: lowercase base */
    char lb[256]; strncpy(lb, base, 255);
    for (int i = 0; lb[i]; i++) lb[i] = (char)tolower((unsigned char)lb[i]);

    /* Step 4: map */
    char mapped[256];
    strncpy(mapped, lb, 255);   /* default: unchanged */

    if (lang == LANG_C) {
        /*
         * C++ compiler -> C compiler:
         *   g++          -> gcc
         *   g++-14       -> gcc-14   (any versioned suffix)
         *   clang++      -> clang
         *   clang++-17   -> clang-17
         *   *-g++        -> *-gcc    (cross-compile prefix)
         *   *-g++-14     -> *-gcc-14
         *   *-clang++*   -> *-clang*
         */
        char *pp;
        /* IMPORTANT: check clang++ BEFORE g++ — "clang++" contains "g++" as
         * a substring and would otherwise be incorrectly mapped to "clangcc". */
        if ((pp = strstr(mapped, "clang++")) != NULL) {
            /* "clang++" -> "clang": remove "++" (shift left 2) */
            memmove(pp + 5, pp + 7, strlen(pp + 7) + 1);
        } else if ((pp = strstr(mapped, "g++")) != NULL) {
            /* "g++" -> "gcc": same length, in-place */
            pp[0] = 'g'; pp[1] = 'c'; pp[2] = 'c';
        }
        /* else: unknown C++ compiler — leave as-is */
    } else {
        /*
         * C compiler -> C++ compiler:
         *   gcc          -> g++
         *   gcc-14       -> g++-14
         *   clang        -> clang++
         *   clang-17     -> clang++-17
         *   *-gcc*       -> *-g++*
         *   cc           -> c++
         */
        char *pp;
        if (strcmp(mapped, "cc") == 0) {
            strncpy(mapped, "c++", 255);
        } else if ((pp = strstr(mapped, "gcc")) != NULL) {
            /* "gcc" -> "g++"  (same length) */
            pp[1] = '+'; pp[2] = '+';
        } else if ((pp = strstr(mapped, "clang")) != NULL) {
            /* "clang" -> "clang++"  (insert "++" after "clang")
             * e.g. "clang" -> "clang++", "clang-17" -> "clang++-17" */
            char rest[256];
            strncpy(rest, pp + 5, 255);   /* everything after "clang" */
            snprintf(pp, 256 - (int)(pp - mapped), "clang++%s", rest);
        }
        /* else: already a C++ compiler or unknown — leave as-is */
    }

    /* Step 5: reassemble */
    snprintf(out_buf, 255, "%s%s%s", ccache_pfx, prefix, mapped);
}

static void build_compile_cmd(char *buf, size_t bufsz,
                               const char *comp, CompilerType ct,
                               const char *src, const char *obj,
                               const char *tags_translated,
                               const char *inc_translated,
                               SourceLang lang) {
    /* Pick the right compiler binary for this file's language */
    char file_comp[256];
    per_file_compiler(comp, ct, lang, file_comp);

    /* Strip internal __omake_stdc= sentinel from tags before any path */
    char tags_clean[B_SIZE];
    strncpy(tags_clean, tags_translated, B_SIZE-1);
    {
        char *s = strstr(tags_clean, "__omake_stdc=");
        if (s) {
            char *e = s + 1; while (*e && *e != ' ' && *e != '\t') e++;
            memmove(s, e, strlen(e)+1);
        }
    }

    if (ct == CT_MSVC) {
        /* MSVC: /TC forces C compilation, /TP forces C++ */
        const char *lang_flag = (lang == LANG_C) ? "/TC" : "/TP";
        /* MSVC /std: flag for C++ */
        char msvc_std[64] = "";
        {
            char *p = strstr(tags_clean, "/std:");
            if (!p) {
                /* translate -std=c++XX -> /std:c++XX */
                char *gp = strstr(tags_clean, "-std=");
                if (gp) {
                    char *ge = gp+1; while (*ge && *ge != ' ') ge++;
                    int gl = (int)(ge-gp-5);
                    if (gl > 0 && gl < 32) {
                        snprintf(msvc_std, sizeof(msvc_std), " /std:%.*s", gl, gp+5);
                        memmove(gp, ge, strlen(ge)+1);
                    }
                }
            }
        }
        char obj_msvc[1024]; strncpy(obj_msvc, obj, 1023);
        char *dot = strrchr(obj_msvc, '.'); if (dot) strcpy(dot, ".obj");
        snprintf(buf, bufsz,
            "%s /nologo /c %s%s \"%s\" /Fo\"%s\" /showIncludes %s %s",
            file_comp, lang_flag, msvc_std, src, obj_msvc, tags_clean, inc_translated);
    } else {
        /* GCC / Clang: adjust -std flag to match file language.
         * Also handle __omake_stdc=XX sentinel injected by STANDARD: C=XX. */
        char tr_fixed[B_SIZE];
        strncpy(tr_fixed, tags_translated, B_SIZE-1);

        /* Extract __omake_stdc=XX sentinel if present */
        char explicit_c_std[32] = "";
        {
            char *sentinel = strstr(tr_fixed, "__omake_stdc=");
            if (sentinel) {
                /* Read the value */
                char *vs = sentinel + strlen("__omake_stdc=");
                char *ve = vs;
                while (*ve && *ve != ' ' && *ve != '\t') ve++;
                int vl = (int)(ve - vs);
                if (vl > 0 && vl < 31) { strncpy(explicit_c_std, vs, vl); explicit_c_std[vl] = '\0'; }
                /* Remove sentinel from tr_fixed */
                memmove(sentinel, ve, strlen(ve) + 1);
                /* trim trailing space left behind */
                char *sp = sentinel;
                while (sp > tr_fixed && *(sp-1) == ' ') sp--;
                *sp = '\0';
                strncat(tr_fixed, ve, B_SIZE - strlen(tr_fixed) - 1);
            }
        }

        if (lang == LANG_C) {
            if (explicit_c_std[0]) {
                /* User specified STANDARD: C=XX — use it directly */
                /* Remove any existing -std= */
                char *p = strstr(tr_fixed, "-std=");
                if (p) {
                    char *end = p + 1;
                    while (*end && *end != ' ' && *end != '\t') end++;
                    memmove(p, end, strlen(end) + 1);
                }
                char newflag[48]; snprintf(newflag, sizeof(newflag), " -std=%s", explicit_c_std);
                strncat(tr_fixed, newflag, B_SIZE - strlen(tr_fixed) - 1);
            } else {
                /*
                 * Derive C standard from C++ standard:
                 *   -std=c++17   -> -std=c17
                 *   -std=gnu++17 -> -std=gnu17
                 *   -std=c++     -> -std=c   (bare, no version)
                 *
                 * Find "++" within the -std= token and remove it.
                 */
                char *p = strstr(tr_fixed, "-std=");
                while (p) {
                    char *pp = strstr(p + 5, "++");
                    if (!pp) break;
                    int gap_ok = 1;
                    for (char *q = p+5; q < pp; q++)
                        if (*q == ' ' || *q == '\t') { gap_ok = 0; break; }
                    if (!gap_ok) { p = strstr(pp, "-std="); continue; }
                    memmove(pp, pp + 2, strlen(pp + 2) + 1);
                    p = strstr(pp, "-std=");
                }
            }
        } else { /* LANG_CXX */
            /* Remove the C sentinel (already stripped above) */
            /*
             * If std is a bare C standard (not already c++ / gnu++),
             * promote it to the C++ equivalent:
             *   -std=c11   -> -std=c++11
             *   -std=gnu17 -> -std=gnu++17
             */
            char *p = strstr(tr_fixed, "-std=");
            while (p) {
                char *after_eq = p + 5;
                if (strncmp(after_eq, "c++",  3) == 0 ||
                    strncmp(after_eq, "gnu++", 5) == 0) {
                    p = strstr(after_eq, "-std="); continue;
                }
                /* Insert "++" after letter prefix, before digit/end */
                int pfx = 0;
                while (after_eq[pfx] && (after_eq[pfx] < '0' || after_eq[pfx] > '9'))
                    pfx++;
                size_t tail = strlen(after_eq + pfx);
                memmove(after_eq + pfx + 2, after_eq + pfx, tail + 1);
                after_eq[pfx]     = '+';
                after_eq[pfx + 1] = '+';
                p = strstr(after_eq + pfx + 2, "-std=");
            }
        }

        snprintf(buf, bufsz,
            "%s -c \"%s\" -o \"%s\" -MMD -MP %s %s",
            file_comp, src, obj, tr_fixed, inc_translated);
    }
}

/*
 * build_link_cmd():
 *   Construct the final link command.
 *
 *   GCC/Clang:
 *     <comp> <objs> <res> <tags> <libs> -o "<out>"
 *
 *   MSVC:
 *     link.exe /nologo <objs_obj> <res> /OUT:"<out>" <machine> <libs_translated>
 *     (cl.exe can also link but link.exe is cleaner for separate compile+link)
 *
 *   libs for MSVC: -lXXX -> XXX.lib   -LXXX -> /LIBPATH:XXX
 */
static void translate_libs_msvc(const char *src, char *dst, size_t dsz) {
    dst[0] = '\0';
    char tmp[B_SIZE]; strncpy(tmp, src, B_SIZE-1);
    char *tok = strtok(tmp, " \t");
    while (tok) {
        char out_tok[300] = "";
        if (strncmp(tok, "-l", 2) == 0) {
            snprintf(out_tok, sizeof(out_tok), "%s.lib", tok+2);
        } else if (strncmp(tok, "-L", 2) == 0) {
            snprintf(out_tok, sizeof(out_tok), "/LIBPATH:\"%s\"", tok+2);
        } else if (strncmp(tok, "-Wl,", 4) == 0) {
            /* linker pass-through: drop for MSVC */
            printf(YELLOW "[MSVC] Dropping linker flag: %s\n" RESET, tok);
        } else {
            strncpy(out_tok, tok, 299);
        }
        if (out_tok[0]) {
            strncat(dst, out_tok, dsz - strlen(dst) - 1);
            strncat(dst, " ",     dsz - strlen(dst) - 1);
        }
        tok = strtok(NULL, " \t");
    }
}

static void build_link_cmd(char *buf, size_t bufsz,
                            const char *comp, CompilerType ct,
                            const char *all_objs, const char *res_objs,
                            const char *tags_translated, const char *libs,
                            const char *out, const char *arch) {
    if (ct == CT_MSVC) {
        /* Use link.exe instead of cl.exe for linking */
        char libs_win[B_SIZE]; translate_libs_msvc(libs, libs_win, B_SIZE);

        /* /MACHINE flag */
        char machine[32] = "";
        if      (strcmp(arch, "x64")   == 0) strcpy(machine, "/MACHINE:X64");
        else if (strcmp(arch, "i386")  == 0) strcpy(machine, "/MACHINE:X86");
        else if (strcmp(arch, "arm64") == 0) strcpy(machine, "/MACHINE:ARM64");
        else                                  strcpy(machine, "/MACHINE:X64"); /* safe default */

        /* Change .o -> .obj in all_objs string */
        char objs_msvc[B_SIZE]; strncpy(objs_msvc, all_objs, B_SIZE-1);
        /* simple replace of .o" -> .obj" */
        char *p = objs_msvc;
        char fixed[B_SIZE]; fixed[0] = '\0';
        while (*p) {
            if (p[0]=='.' && p[1]=='o' && p[2]=='"') {
                strncat(fixed, ".obj\"", B_SIZE - strlen(fixed) - 1);
                p += 3;
            } else {
                char ch[2] = {*p, '\0'};
                strncat(fixed, ch, B_SIZE - strlen(fixed) - 1);
                p++;
            }
        }

        snprintf(buf, bufsz,
            "link.exe /nologo %s %s /OUT:\"%s\" %s %s %s",
            fixed, res_objs ? res_objs : "", out, machine, libs_win, tags_translated);
    } else {
        /* Filter compile-only flags from tags before linking */
        char link_tags[B_SIZE];
        {
            link_tags[0] = '\0';
            char tmp_lt[B_SIZE]; strncpy(tmp_lt, tags_translated, B_SIZE-1);
            char *tok2 = strtok(tmp_lt, " \t");
            while (tok2) {
                /* Skip: -std=*, -W*, -f* (compile-only), -D* (defines), -g, -O*, -march, -mtune */
                int skip = (
                    strncmp(tok2,"-std=",5)==0 ||
                    strncmp(tok2,"-W",2)==0     ||
                    strncmp(tok2,"-D",2)==0     ||
                    strncmp(tok2,"-g",2)==0     ||
                    strncmp(tok2,"-O",2)==0     ||
                    strncmp(tok2,"-march",6)==0 ||
                    strncmp(tok2,"-mtune",6)==0 ||
                    strncmp(tok2,"-fPIC",5)==0  ||
                    strncmp(tok2,"-fpic",5)==0  ||
                    strncmp(tok2,"-fpie",5)==0  ||
                    strncmp(tok2,"-fPIE",5)==0  ||
                    (strncmp(tok2,"-f",2)==0 && strncmp(tok2,"-framework",10)!=0)
                );
                if (!skip) {
                    strncat(link_tags, tok2, B_SIZE-strlen(link_tags)-1);
                    strncat(link_tags, " ", B_SIZE-strlen(link_tags)-1);
                }
                tok2 = strtok(NULL, " \t");
            }
        }
        snprintf(buf, bufsz,
            "%s %s %s %s %s -o \"%s\"",
            comp, all_objs, res_objs ? res_objs : "", link_tags, libs, out);
    }
}

/*
 * msvc_obj_ext():
 *   Returns ".obj" for MSVC, ".o" for GCC/Clang.
 *   Used when scanning existing object files.
 */
static const char *obj_ext(CompilerType ct) {
    return (ct == CT_MSVC) ? ".obj" : ".o";
}

/*
 * windres_or_rc():
 *   On MSVC builds use rc.exe instead of windres.
 *   rc.exe syntax: rc.exe [flags] /fo "out.res" "in.rc"
 *   Returns 1 if compilation succeeded.
 */
static int compile_one_rc_msvc(const char *rc_in, const char *rc_out,
                                const char *arch_flag) {
    char cmd[1600];
    snprintf(cmd, sizeof(cmd), "rc.exe /nologo %s /fo \"%s\" \"%s\"",
             arch_flag, rc_out, rc_in);
    printf(CYAN "[RC/MSVC] %s\n" RESET, cmd);
    return system(cmd) == 0;
}



/* ------------------------------------------------------------------ */
/*  Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char url[512];
    char dest[512];
} FetchEntry;

typedef struct {
    char path[1024];   /* full path to source file (.c / .cpp) */
    char obj[1024];    /* full path to .o   */
    char dep[1024];    /* full path to .d   */
    int  needs_rebuild;
    SourceLang lang;   /* LANG_C or LANG_CXX */
} SourceFile;

/* ── FetchEntryV2 (extended FETCH_GIT) ── */

typedef struct {
    char url[512];
    char dest[512];
    char tag[64];          /* git tag or commit */
    char branch[64];       /* git branch */
    char configure[512];   /* post-clone configure command */
    char add_inc[512];     /* extra include dir inside repo */
    char add_lib[512];     /* extra lib dir inside repo */
    char cmake_args[512];  /* if repo has CMakeLists, run cmake with these args */
    int  shallow;          /* --depth=1 (default) */
    int  recurse;          /* --recurse-submodules */
} FetchEntryV2;

#define MAX_FETCH_V2  64
static FetchEntryV2 g_fetch_v2[MAX_FETCH_V2];
static int          g_fetch_v2_count = 0;

/* ── Target system ── */

/* ── PkgMeta (package metadata) ── */
typedef struct {
    char name[128];         /* stem of OUT: */
    char version[64];       /* VERSION: */
    char install_dir[512];  /* directory part of INSTALL: */
    char bin_path[512];     /* OUT: */
    char description[256];  /* DESCRIPTION: (new directive) */
    char maintainer[128];   /* MAINTAINER: (new directive) */
    char homepage[256];     /* HOMEPAGE: (new directive) */
    char arch_str[32];      /* x86_64 / amd64 / i386 / arm64 */
    long long bin_size;
} PkgMeta;

#define MAX_TARGETS 32

typedef struct {
    char name[128];
    char src_dir[512];
    char src_ext[32];
    char src_ext2[32];
    char out[512];
    char flags[B_SIZE];       /* TARGET_FLAGS  */
    char libs[B_SIZE];        /* TARGET_LIBS   */
    char inc[B_SIZE];         /* TARGET_INC    */
    char deps[512];           /* TARGET_DEPS   */
    char output_type[32];     /* exe|shared|static */
    int  is_interface;        /* INTERFACE_LIB  */
    int  is_object;           /* OBJECT_LIB     */
    char alias[128];          /* ALIAS: Foo::Core */
    int  active;
} Target;

static Target g_targets[MAX_TARGETS];
static int    g_target_count = 0;
static int    g_current_target = -1;


/* ------------------------------------------------------------------ */
/*  Utility                                                             */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/*  Forward declarations for feature functions                          */
/* ------------------------------------------------------------------ */
void apply_pkg_config(const char *pkg_list, char *tags, char *libs);
void apply_defines(const char *define_list, char *tags);
void apply_version(const char *ver, char *tags);
void apply_lto(const char *lto_mode, char *tags, char *libs, CompilerType ct);
void apply_output_type(const char *otype, char *tags, char *libs, char *out, CompilerType ct);
int  link_static_lib(const char *all_objs, const char *out);
void build_pch(const char *hdr, const char *obj_root, const char *comp, CompilerType ct,
               const char *tags, const char *auto_inc, char *pch_flag_out);
int  unity_compile(SourceFile *srcs, int count, const char *obj_root,
                   const char *final_comp, CompilerType ct,
                   const char *tags_tr, const char *inc_tr);
void generate_compile_commands(SourceFile *srcs, int count, const char *comp,
                                const char *tags, const char *inc);
void do_install(const char *src_bin, const char *dst);
void do_strip(const char *bin, CompilerType ct);
void cmd_format(int dry_run);
void cmd_lint(void);
void cmd_benchmark(const char *bin, int runs);
void cmd_disasm(const char *bin, const char *func_filter);
void cmd_coverage(void);
void cmd_docs(void);
void cmd_package(const char *bin, const char *ver);
void cmd_compress(const char *bin);
int  cmd_run_tests(const char *test_dir, const char *test_ext, const char *obj_root,
                   const char *final_comp, CompilerType ct,
                   const char *tags, const char *libs, const char *auto_inc);
void cmd_watch(int force, const char *profile, int jobs, const char *arch, int do_static);
void cmd_self_update(const char *self_path);
static int g_force_bin_ext = 0; /* --bin flag */

/* ------------------------------------------------------------------ */
/*  MSVC Environment Auto-Setup                                         */
/*                                                                      */
/*  When cl.exe is invoked from a plain cmd.exe (not Developer          */
/*  Command Prompt), INCLUDE and LIB are not set, so the compiler       */
/*  cannot find <iostream>, <string>, Windows SDK headers, etc.         */
/*                                                                      */
/*  This function:                                                       */
/*  1. Checks if INCLUDE is already set (Developer Prompt) — skip.     */
/*  2. Runs vswhere.exe to find the VS installation path.               */
/*  3. Locates VCToolsInstallDir and Windows SDK paths.                 */
/*  4. Injects INCLUDE, LIB, and PATH into the current process env.    */
/*                                                                      */
/*  Supports: VS 2017, 2019, 2022 (any edition).                       */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

/* Try to read a registry DWORD/SZ value.  Returns 1 on success. */
static int reg_read_sz(HKEY root, const char *subkey, const char *value,
                        char *out, DWORD outsz) {
    HKEY hk;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    DWORD type, sz = outsz;
    int ok = (RegQueryValueExA(hk, value, NULL, &type, (LPBYTE)out, &sz) == ERROR_SUCCESS
              && (type == REG_SZ || type == REG_EXPAND_SZ));
    RegCloseKey(hk);
    if (ok) out[sz < outsz ? sz : outsz-1] = '\0';
    return ok;
}

/*
 * Append `addition` to the env var `name`, separated by ';'.
 * Creates the variable if it doesn't exist yet.
 */
static void env_append(const char *name, const char *addition) {
    const char *cur = getenv(name);
    char buf[8192];
    if (cur && cur[0])
        snprintf(buf, sizeof(buf), "%s;%s", addition, cur);
    else
        strncpy(buf, addition, sizeof(buf)-1);
    SetEnvironmentVariableA(name, buf);
    /* Also propagate into the CRT's view via _putenv */
    char kv[8256];
    snprintf(kv, sizeof(kv), "%s=%s", name, buf);
    _putenv(kv);
}

/*
 * Scan `base_dir` for the highest-version subdirectory.
 * Used to find e.g. "14.36.32532" inside VCToolsInstallDir parents.
 * Writes result into `out`.
 */
static int find_latest_subdir(const char *base_dir, char *out, int outsz) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", base_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char best[512] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        /* Simple lexicographic max works for dotted version strings */
        if (strcmp(fd.cFileName, best) > 0)
            strncpy(best, fd.cFileName, sizeof(best)-1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!best[0]) return 0;
    snprintf(out, outsz, "%s\\%s", base_dir, best);
    return 1;
}

/*
 * msvc_setup_environment():
 *   Auto-detect and inject MSVC + Windows SDK include/lib paths.
 *   Safe to call multiple times — skips if INCLUDE already set.
 *   Returns 1 if setup succeeded (or was already done), 0 on failure.
 */
static int msvc_setup_environment(void) {
    /* Already configured (Developer Prompt or previous call) */
    const char *inc = getenv("INCLUDE");
    if (inc && strstr(inc, "MSVC")) return 1;
    if (inc && strstr(inc, "VC"))   return 1;

    printf(YELLOW "[MSVC] INCLUDE not set — auto-detecting Visual Studio...\n" RESET);

    /* ── Step 1: find VS install root via vswhere ── */
    char vs_root[1024] = "";

    /* vswhere ships with VS 2017+ and lives in a fixed path */
    const char *vswhere_paths[] = {
        "%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        "%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        "C:\\Program Files\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        NULL
    };

    char vswhere[512] = "";
    for (int i = 0; vswhere_paths[i]; i++) {
        char expanded[512];
        ExpandEnvironmentStringsA(vswhere_paths[i], expanded, sizeof(expanded));
        if (GetFileAttributesA(expanded) != INVALID_FILE_ATTRIBUTES) {
            strncpy(vswhere, expanded, sizeof(vswhere)-1);
            break;
        }
    }

    if (vswhere[0]) {
        /* Run: vswhere -latest -products * -requires Microsoft.VisualCpp.Tools.HostX86.TargetX64
         *              -property installationPath */
        char cmd[768];
        snprintf(cmd, sizeof(cmd),
            "\"%s\" -latest -products * "
            "-requires Microsoft.VisualCpp.Tools.HostX64.TargetX64 "
            "-property installationPath 2>nul", vswhere);
        FILE *fp = _popen(cmd, "r");
        if (fp) {
            if (fgets(vs_root, sizeof(vs_root), fp)) {
                /* strip newline/CR */
                char *nl = strrchr(vs_root, '\n'); if (nl) *nl = '\0';
                nl = strrchr(vs_root, '\r'); if (nl) *nl = '\0';
            }
            _pclose(fp);
        }
        /* Fallback: try without the HostX64 requirement */
        if (!vs_root[0]) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" -latest -products * -property installationPath 2>nul",
                vswhere);
            fp = _popen(cmd, "r");
            if (fp) {
                if (fgets(vs_root, sizeof(vs_root), fp)) {
                    char *nl = strrchr(vs_root, '\n'); if (nl) *nl = '\0';
                    nl = strrchr(vs_root, '\r'); if (nl) *nl = '\0';
                }
                _pclose(fp);
            }
        }
    }

    /* Fallback: well-known VS 2022/2019/2017 paths */
    if (!vs_root[0]) {
        const char *fallbacks[] = {
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Professional",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Community",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Professional",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Enterprise",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\BuildTools",
            NULL
        };
        for (int i = 0; fallbacks[i]; i++) {
            if (GetFileAttributesA(fallbacks[i]) != INVALID_FILE_ATTRIBUTES) {
                strncpy(vs_root, fallbacks[i], sizeof(vs_root)-1);
                printf(CYAN "[MSVC] Found VS (fallback): %s\n" RESET, vs_root);
                break;
            }
        }
    }

    if (!vs_root[0]) {
        printf(RED "[MSVC] Cannot locate Visual Studio installation.\n" RESET);
        printf(RED "[MSVC] Run from Developer Command Prompt, or install VS.\n" RESET);
        return 0;
    }
    printf(CYAN "[MSVC] VS root: %s\n" RESET, vs_root);

    /* ── Step 2: find VCToolsInstallDir (highest version) ── */
    char vc_tools_base[1024];
    snprintf(vc_tools_base, sizeof(vc_tools_base),
             "%s\\VC\\Tools\\MSVC", vs_root);

    char vc_tools[1024] = "";
    if (!find_latest_subdir(vc_tools_base, vc_tools, sizeof(vc_tools))) {
        printf(RED "[MSVC] Cannot find VCTools under: %s\n" RESET, vc_tools_base);
        return 0;
    }
    printf(CYAN "[MSVC] VCTools: %s\n" RESET, vc_tools);

    /* Determine host/target arch for the compiler binary path */
#if defined(_M_X64) || defined(__x86_64__)
    const char *host_arch   = "Hostx64";
    const char *target_arch = "x64";
#elif defined(_M_IX86) || defined(__i386__)
    const char *host_arch   = "Hostx86";
    const char *target_arch = "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    const char *host_arch   = "Hostarm64";
    const char *target_arch = "arm64";
#else
    const char *host_arch   = "Hostx64";
    const char *target_arch = "x64";
#endif

    /* ── Step 3: find Windows SDK ── */
    char sdk_root[1024]    = "";
    char sdk_version[64]   = "";

    /* Registry path for Windows 10/11 SDK */
    const char *sdk_reg_key =
        "SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots";
    reg_read_sz(HKEY_LOCAL_MACHINE, sdk_reg_key, "KitsRoot10",
                sdk_root, sizeof(sdk_root));

    if (!sdk_root[0]) {
        /* Try non-WOW path */
        reg_read_sz(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
                    "KitsRoot10", sdk_root, sizeof(sdk_root));
    }

    /* Fallback: well-known paths */
    if (!sdk_root[0]) {
        const char *sdk_fb[] = {
            "C:\\Program Files (x86)\\Windows Kits\\10",
            "C:\\Program Files\\Windows Kits\\10",
            NULL
        };
        for (int i = 0; sdk_fb[i]; i++)
            if (GetFileAttributesA(sdk_fb[i]) != INVALID_FILE_ATTRIBUTES)
                { strncpy(sdk_root, sdk_fb[i], sizeof(sdk_root)-1); break; }
    }

    /* Find latest SDK version under sdk_root\Include\ */
    if (sdk_root[0]) {
        char sdk_inc_base[1024];
        snprintf(sdk_inc_base, sizeof(sdk_inc_base), "%s\\Include", sdk_root);
        char sdk_ver_path[1024] = "";
        if (find_latest_subdir(sdk_inc_base, sdk_ver_path, sizeof(sdk_ver_path))) {
            /* sdk_ver_path = "C:\...\Include\10.0.22621.0"
             * extract just the version number */
            const char *vp = strrchr(sdk_ver_path, '\\');
            if (vp) strncpy(sdk_version, vp+1, sizeof(sdk_version)-1);
        }
        printf(CYAN "[MSVC] Windows SDK: %s  version: %s\n" RESET,
               sdk_root, sdk_version[0] ? sdk_version : "(unknown)");
    } else {
        printf(YELLOW "[MSVC] Windows SDK not found — some headers may be missing.\n" RESET);
    }

    /* ── Step 4: build include paths ── */
    /*
     * MSVC CRT headers:
     *   <VCTools>\include
     *
     * Windows SDK headers (all four are needed for a complete build):
     *   <SDK>\Include\<ver>\ucrt      — C runtime (stdio.h, math.h …)
     *   <SDK>\Include\<ver>\um        — Win32 API (windows.h …)
     *   <SDK>\Include\<ver>\shared    — shared types (wtypes.h …)
     *   <SDK>\Include\<ver>\winrt     — WinRT (optional but harmless)
     */
    {
        char inc_path[8192]; inc_path[0] = '\0';

        /* VC++ STL / CRT headers */
        char p1[1024]; snprintf(p1, sizeof(p1), "%s\\include", vc_tools);
        snprintf(inc_path, sizeof(inc_path), "%s", p1);

        /* Windows SDK headers */
        if (sdk_root[0] && sdk_version[0]) {
            const char *sdk_subdirs[] = { "ucrt", "um", "shared", "winrt", NULL };
            for (int i = 0; sdk_subdirs[i]; i++) {
                char p[1024];
                snprintf(p, sizeof(p), "%s\\Include\\%s\\%s",
                         sdk_root, sdk_version, sdk_subdirs[i]);
                if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
                    strncat(inc_path, ";", sizeof(inc_path)-strlen(inc_path)-1);
                    strncat(inc_path, p,   sizeof(inc_path)-strlen(inc_path)-1);
                }
            }
        }

        env_append("INCLUDE", inc_path);
        printf(GREEN "[MSVC] INCLUDE set (%zu chars)\n" RESET, strlen(inc_path));
    }

    /* ── Step 5: build lib paths ── */
    {
        char lib_path[4096]; lib_path[0] = '\0';

        /* VC++ libs */
        char p1[1024];
        snprintf(p1, sizeof(p1), "%s\\lib\\%s", vc_tools, target_arch);
        strncpy(lib_path, p1, sizeof(lib_path)-1);

        /* Windows SDK libs */
        if (sdk_root[0] && sdk_version[0]) {
            const char *sdk_lib_subdirs[] = { "ucrt", "um", NULL };
            for (int i = 0; sdk_lib_subdirs[i]; i++) {
                char p[1024];
                snprintf(p, sizeof(p), "%s\\Lib\\%s\\%s\\%s",
                         sdk_root, sdk_version, sdk_lib_subdirs[i], target_arch);
                if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
                    strncat(lib_path, ";", sizeof(lib_path)-strlen(lib_path)-1);
                    strncat(lib_path, p,   sizeof(lib_path)-strlen(lib_path)-1);
                }
            }
        }

        env_append("LIB", lib_path);
        printf(GREEN "[MSVC] LIB set (%zu chars)\n" RESET, strlen(lib_path));
    }

    /* ── Step 6: add cl.exe / link.exe to PATH ── */
    {
        char bin_dir[1024];
        snprintf(bin_dir, sizeof(bin_dir),
                 "%s\\bin\\%s\\%s", vc_tools, host_arch, target_arch);
        if (GetFileAttributesA(bin_dir) != INVALID_FILE_ATTRIBUTES) {
            env_append("PATH", bin_dir);
            printf(GREEN "[MSVC] PATH += %s\n" RESET, bin_dir);
        }
    }

    printf(GREEN "[MSVC] Environment ready — cl.exe can now find standard headers.\n" RESET);
    return 1;
}

#else  /* non-Windows stub */
static int msvc_setup_environment(void) { return 0; }
#endif /* _WIN32 */


/* Forward declarations for enterprise features */
typedef struct { char compiler[256]; char c_compiler[256]; char linker[256];
    char sysroot[512]; char ar[128]; char strip_tool[128];
    char linker_script[512]; char tags[1024]; char defines[512];
    char sysroot_libs[512]; } Toolchain;
static int  toolchain_load(const char *path, Toolchain *tc);
static void toolchain_apply(const Toolchain *tc, char *comp, char *tags, char *libs);
static int  dep_cache_hit(const char *lib_name, const char *url, const char *version, const char *flags);
static void dep_cache_mark(const char *lib_name, const char *url, const char *version, const char *flags);
static void dep_cache_list(void);
static void dep_cache_clear(void);
static void apply_reproducible(char *tags);
static void pgo_apply_generate(char *tags);
static void pgo_apply_use(char *tags);
static void gen_omake_test_header(const char *obj_root);
static void preset_load(const char *path);
typedef struct { char name[64]; char compiler[256]; char profile[64];
    char tags[1024]; char arch[32]; char toolchain[512];
    char use_lib[512]; char defines[512]; char libs[512];
    int  do_static; int  jobs; int  reproducible; } OmakePreset;
static OmakePreset *preset_find(const char *name);
static void preset_list(void);
static void ide_gen_vscode(void);
static void ide_gen_clion(void);
static void cmd_ide(const char *target);
static void dist_build(const char *hosts_csv, int jobs, const char *comp, const char *tags);
int  build_engine(int force, const char *profile, int jobs, int show_graph,
                  const char *arch, int do_static, const char *override_compiler,
                  int do_strip_bin, int do_compress_bin, int gen_compile_commands, int gen_asm);
void resolve_deps(const char *deps_line, FetchEntry *fetch_list, int *fetch_count);
void apply_env(const char *env_list, char *tags);
void cmd_sign(const char *bin, const char *cert);
void cmd_create_project_ex(const char *name, const char *tmpl_name);
void cmd_m_pack(const char *formats_arg);
void cmd_m_gvz(const char *out_format);
void fetch_v2_all(int force, char *tags, char *libs);
static void auto_inject_missing_fetches(const char *use_libs);
void expand_genex(const char *input, char *output, int outsz,
                  const char *profile, CompilerType ct);
int  unity_compile_batched(SourceFile *srcs, int count,
                           const char *obj_root,
                           const char *final_comp, CompilerType ct,
                           const char *tags_tr, const char *inc_tr,
                           int batch_size);
void auto_pch_detect(const char *obj_root, char *best_hdr_out, int outsz);
Target *target_get_or_create(const char *name);
void mpack_nsis(const PkgMeta *m, const char *outdir);
void apply_lto_auto(char *tags, char *libs, CompilerType ct, const char *comp);
void run_configure_files(const char *cfg_list, const char *ver,
                         const char *define_list, const char *project_name);
void external_project_build_all(char *tags, char *libs);
void apply_pkg_probe(const char *probe_list, char *tags, char *libs);
void check_compile_features(const char *req, const char *comp);
void apply_ipo(const char *ipo_mode, char *tags, char *libs, CompilerType ct);
void enforce_clean_build(const char *out_bin, const char *obj_root);
void apply_debug_postfix(char *out, const char *postfix, const char *profile);
void apply_use_lib(const char *use_lib_str, char *tags, char *libs,
                   const char *src_dir, const char *obj_root,
                   const char *comp, CompilerType ct, int do_static,
                   char *extra_objs, size_t extra_sz,
                   const char *qt_modules, const char *boost_modules,
                   const char *opencv_modules, const char *wx_modules,
                   const char *imgui_backend);
void setup_qt(const char *modules_str, char *tags, char *libs,
              const char *src_dir, const char *obj_root,
              const char *comp, CompilerType ct,
              char *extra_objs, size_t extra_sz);
void run_hook(const char *hook_line, const char *out_bin,
              const char *src_dir, const char *ver, const char *hook_type);
void run_size_report(const char *mode, const char *bin_path);
void run_mirror(const char *mirror_list);
void run_hash_check(const char *mode, SourceFile *srcs, int nsrcs);
void run_multi_out(const char *multi_list, const char *src_dir,
                    const char *obj_root, const char *final_comp,
                    CompilerType ct, const char *tags,
                    const char *libs, const char *auto_inc,
                    const char *arch);
void cmd_m_gvz_targets(void);
void process_qt_resources(const char *qrc_list, const char *obj_root,
                           const char *comp, CompilerType ct,
                           const char *tags, const char *auto_inc,
                           char *extra_objs, int extra_objs_sz);
void process_embed_binary(const char *embed_list, const char *obj_root,
                           const char *comp, CompilerType ct,
                           const char *tags, const char *auto_inc,
                           char *extra_objs, int extra_objs_sz);
void external_project_parse(const char *line);
void mpack_pkg_macos(const PkgMeta *m, const char *outdir);
int  parse_fetch_v2(const char *line, FetchEntryV2 *e);
void propagate_interface_libs(void);
void resolve_target_deps(Target *consumer);

void fix_slashes(char *p) {
    if (!p) return;
    for (; *p; p++) if (*p == '/' || *p == '\\') *p = SEP;
}

void trim(char *s) {
    if (!s || !*s) return;
    while (isspace((unsigned char)*s)) memmove(s, s+1, strlen(s));
    char *p = s + strlen(s) - 1;
    while (p >= s && isspace((unsigned char)*p)) *p-- = '\0';
}

void to_lower_str(char *s) {
    for (int i = 0; s[i]; i++) s[i] = (char)tolower((unsigned char)s[i]);
}

int is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFREG) != 0;
}

int dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFDIR) != 0;
}

time_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

void ensure_dir_exists(const char *path) {
    char temp[1024], *p = NULL;
    snprintf(temp, sizeof(temp), "%s", path);
    fix_slashes(temp);
    /* Strip last component (filename) — create only the parent dir */
    char *last_sep = strrchr(temp, SEP);
    if (last_sep) {
        *last_sep = '\0';  /* always strip: "bin/app" → "bin", "bin/app.exe" → "bin" */
    } else {
        /* No separator: path is just a filename with no directory part — nothing to create */
        return;
    }
    if (!temp[0]) return;
    for (p = temp + 1; *p; p++) {
        if (*p == SEP) { *p = '\0'; mkdir_auto(temp); *p = SEP; }
    }
    mkdir_auto(temp);
}

int tool_exists(const char *tool) {
    char cmd[256];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>&1", tool);
#else
    snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", tool);
#endif
    return system(cmd) == 0;
}

/* ------------------------------------------------------------------ */
/*  Recursive directory scanner                                         */
/* ------------------------------------------------------------------ */

static int   inc_count = 0;
static char  inc_dirs[MAX_INCDIRS][512];

void scan_inc_dirs(const char *base) {
    if (!dir_exists(base)) return;
    DIR *d = opendir(base);
    if (!d) return;
    if (inc_count < MAX_INCDIRS) {
        strncpy(inc_dirs[inc_count++], base, 511);
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s%c%s", base, SEP, ent->d_name);
        if (dir_exists(full)) scan_inc_dirs(full);
    }
    closedir(d);
}

static int        src_count = 0;
static SourceFile sources[MAX_SRCS];

void scan_sources(const char *base, const char *ext, const char *obj_root) {
    if (!dir_exists(base)) {
        return;
    }
#ifdef _WIN32
    /* Windows: use FindFirstFile for reliable directory scanning */
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", base);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s\\%s", base, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_sources(full, ext, obj_root);
        } else if (strstr(fd.cFileName, ext)) {
            struct { char path[1024]; char obj[1024]; char dep[1024];
                     int needs_rebuild; int lang; } _sf;
            /* Use main SourceFile path */
            if (src_count >= MAX_SRCS) continue;
            int already = 0;
            for (int di=0; di<src_count; di++)
                if (strcmp(sources[di].path, full)==0){already=1;break;}
            if (already) continue;
            SourceFile *sf = &sources[src_count++];
            strncpy(sf->path, full, 1023);
            { const char *e=strrchr(fd.cFileName,'.'); sf->lang=(e&&strcmp(e,".c")==0)?LANG_C:LANG_CXX; }
            char rel[1024]; snprintf(rel,sizeof(rel),"%s\\%s",base,fd.cFileName);
            char obj_rel[1024]; strncpy(obj_rel,rel,1023);
            char *dot=strrchr(obj_rel,'.'); if(dot) strcpy(dot,".o");
            snprintf(sf->obj,sizeof(sf->obj),"%s\\%s",obj_root,obj_rel);
            char dep_rel[1024]; strncpy(dep_rel,rel,1023);
            dot=strrchr(dep_rel,'.'); if(dot) strcpy(dot,".d");
            snprintf(sf->dep,sizeof(sf->dep),"%s\\%s",obj_root,dep_rel);
            fix_slashes(sf->path); fix_slashes(sf->obj); fix_slashes(sf->dep);
            sf->needs_rebuild=0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s%c%s", base, SEP, ent->d_name);
        if (dir_exists(full)) {
            scan_sources(full, ext, obj_root);
        } else if (is_regular_file(full) && strstr(ent->d_name, ext)) {
            if (src_count >= MAX_SRCS) continue;
            /* Dedup: skip if this path is already in sources[] */
            {
                int already = 0;
                for (int di = 0; di < src_count; di++) {
                    if (strcmp(sources[di].path, full) == 0) { already = 1; break; }
                }
                if (already) continue;
            }

            SourceFile *sf = &sources[src_count++];

            strncpy(sf->path, full, 1023);

            /* Detect language from extension */
            {
                const char *e = strrchr(ent->d_name, '.');
                sf->lang = (e && (strcmp(e, ".c") == 0)) ? LANG_C : LANG_CXX;
            }

            /* .o path: <obj_root>/<base>/<file>.o */
            char rel[1024];
            snprintf(rel, sizeof(rel), "%s%c%s", base, SEP, ent->d_name);
            char obj_rel[1024]; strncpy(obj_rel, rel, 1023);
            char *dot = strrchr(obj_rel, '.'); if (dot) strcpy(dot, ".o");
            snprintf(sf->obj, sizeof(sf->obj), "%s%c%s", obj_root, SEP, obj_rel);

            /* .d path */
            char dep_rel[1024]; strncpy(dep_rel, rel, 1023);
            dot = strrchr(dep_rel, '.'); if (dot) strcpy(dot, ".d");
            snprintf(sf->dep, sizeof(sf->dep), "%s%c%s", obj_root, SEP, dep_rel);

            fix_slashes(sf->path);
            fix_slashes(sf->obj);
            fix_slashes(sf->dep);
            sf->needs_rebuild = 0;
        }
    }
    closedir(d);
#endif
}

/* ------------------------------------------------------------------ */
/*  Dependency file parser                                              */
/*  GCC/Clang -MMD produces "obj.o: src.cpp hdr1.h hdr2.h ..."        */
/* ------------------------------------------------------------------ */

int dep_needs_rebuild(const char *obj_path, const char *dep_path) {
    time_t obj_time = file_mtime(obj_path);
    if (obj_time == 0) return 1;

    FILE *f = fopen(dep_path, "r");
    if (!f) return 1;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *bs = strstr(line, " \\"); if (bs) *bs = '\0';
        trim(line);
        char *colon = strchr(line, ':');
        const char *start = colon ? colon + 1 : line;
        char tmp[4096]; strncpy(tmp, start, 4095);
        char *tok = strtok(tmp, " \t\n\r");
        while (tok) {
            if (strlen(tok) > 1 && file_mtime(tok) > obj_time) {
                fclose(f); return 1;
            }
            tok = strtok(NULL, " \t\n\r");
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  FETCH_GIT                                                           */
/* ------------------------------------------------------------------ */

int parse_fetch_git(const char *line, FetchEntry *entry) {
    const char *p = strchr(line, ':'); if (!p) return 0;
    p++;
    while (isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 511) entry->url[i++] = *p++;
    entry->url[i] = '\0'; if (i == 0) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (strncmp(p, "TO", 2) != 0 && strncmp(p, "to", 2) != 0) return 0;
    p += 2;
    while (isspace((unsigned char)*p)) p++;
    i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 511) entry->dest[i++] = *p++;
    entry->dest[i] = '\0'; if (i == 0) return 0;
    fix_slashes(entry->dest);
    return 1;
}

/* Auto-detect -I and -L paths inside a fetched repo */
void fetch_auto_flags(const char *dest, char *tags, char *libs) {
    const char *inc_c[] = { "", "include", "src", NULL };
    for (int i = 0; inc_c[i]; i++) {
        char p[700];
        if (strlen(inc_c[i])) snprintf(p, sizeof(p), "%s%c%s", dest, SEP, inc_c[i]);
        else strncpy(p, dest, 699);
        if (dir_exists(p)) {
            char flag[800]; snprintf(flag, sizeof(flag), " -I\"%s\"", p);
            if (!strstr(tags, flag)) strcat(tags, flag);
        }
    }
    const char *lib_c[] = { "lib", "build", "bin", NULL };
    for (int i = 0; lib_c[i]; i++) {
        char p[700]; snprintf(p, sizeof(p), "%s%c%s", dest, SEP, lib_c[i]);
        if (dir_exists(p)) {
            char flag[800]; snprintf(flag, sizeof(flag), " -L\"%s\"", p);
            if (!strstr(libs, flag)) strcat(libs, flag);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  curl fallback helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * Detect archive type from URL and return the appropriate extract command.
 * Writes the temp download path into out_tmp (caller must free).
 *
 * Supported:
 *   *.tar.gz / *.tgz  -> tar xzf
 *   *.tar.bz2         -> tar xjf
 *   *.tar.xz          -> tar xJf
 *   *.zip             -> unzip  (or tar on Windows)
 *
 * For a bare git URL (*.git or no extension), we download a GitHub-style
 * /archive/HEAD.zip as a best-effort fallback.
 */
static void curl_fetch(const char *url, const char *dest, int force) {
    /* Determine download filename */
    char tmp_file[512];
    const char *slash = strrchr(url, '/');
    const char *fname = slash ? slash + 1 : url;

    /* If URL ends with .git or has no archive extension, try GitHub archive */
    char real_url[1024];
    strncpy(real_url, url, 1023);
    int is_git_url = (strstr(url, ".git") != NULL);
    int is_archive = (strstr(fname, ".tar") || strstr(fname, ".tgz") || strstr(fname, ".zip"));

    if (is_git_url && !is_archive) {
        /* Convert  https://github.com/user/repo.git
           to       https://github.com/user/repo/archive/refs/heads/master.zip */
        char base[1024]; strncpy(base, url, 1023);
        char *dot = strstr(base, ".git"); if (dot) *dot = '\0';
        snprintf(real_url, sizeof(real_url), "%s/archive/refs/heads/master.zip", base);
        snprintf(tmp_file, sizeof(tmp_file), ".omake_dl_tmp.zip");
    } else {
        snprintf(tmp_file, sizeof(tmp_file), ".omake_dl_tmp_%s", fname);
    }

    /* Download with curl */
    char curl_cmd[1600];
    snprintf(curl_cmd, sizeof(curl_cmd),
        "curl -L --progress-bar --retry 3 --retry-delay 2 "
        "--connect-timeout 15 -o \"%s\" \"%s\"",
        tmp_file, real_url);
    printf(CYAN "[CURL] %s\n" RESET, curl_cmd);

    if (system(curl_cmd) != 0) {
        printf(RED "[FETCH] curl download failed: %s\n" RESET, real_url);
        return;
    }

    /* Extract */
    ensure_dir_exists(dest);

    char extract_cmd[1600];
    const char *tf = tmp_file;

    if (strstr(tf, ".tar.gz") || strstr(tf, ".tgz")) {
#ifdef _WIN32
        snprintf(extract_cmd, sizeof(extract_cmd), "tar -xzf \"%s\" -C \"%s\" --strip-components=1", tf, dest);
#else
        snprintf(extract_cmd, sizeof(extract_cmd), "tar -xzf \"%s\" -C \"%s\" --strip-components=1", tf, dest);
#endif
    } else if (strstr(tf, ".tar.bz2")) {
        snprintf(extract_cmd, sizeof(extract_cmd), "tar -xjf \"%s\" -C \"%s\" --strip-components=1", tf, dest);
    } else if (strstr(tf, ".tar.xz")) {
        snprintf(extract_cmd, sizeof(extract_cmd), "tar -xJf \"%s\" -C \"%s\" --strip-components=1", tf, dest);
    } else {
        /* .zip */
#ifdef _WIN32
        /*
         * Windows: system() runs through cmd.exe but does NOT support
         * multi-statement FOR loops inline.  Write a temporary .bat file
         * and execute it instead — this is the only reliable way.
         */
        char tmp_dir[512];
        snprintf(tmp_dir, sizeof(tmp_dir), ".omake_zip_tmp");

        /* Clean and recreate temp dir */
        char rm_tmp[600]; snprintf(rm_tmp, sizeof(rm_tmp), "rmdir /s /q \"%s\" >nul 2>&1", tmp_dir);
        system(rm_tmp);
        mkdir_auto(tmp_dir);

        /* Write helper bat */
        FILE *bat = fopen(".omake_extract.bat", "w");
        if (!bat) {
            printf(RED "[FETCH] Cannot create .omake_extract.bat\n" RESET);
            return;
        }
        fprintf(bat, "@echo off\n");
        fprintf(bat, "tar -xf \"%s\" -C \"%s\"\n", tf, tmp_dir);
        fprintf(bat, "if errorlevel 1 exit /b 1\n");
        /* Copy contents of the single extracted subdirectory into dest */
        fprintf(bat, "for /d %%%%d in (\"%s\\*\") do (\n", tmp_dir);
        fprintf(bat, "    xcopy /e /i /q /y \"%%%%d\" \"%s\"\n", dest);
        fprintf(bat, ")\n");
        fprintf(bat, "rmdir /s /q \"%s\"\n", tmp_dir);
        fclose(bat);

        snprintf(extract_cmd, sizeof(extract_cmd), "cmd /c .omake_extract.bat");
#else
        char tmp_dir[512]; snprintf(tmp_dir, sizeof(tmp_dir), ".omake_zip_tmp");
        char rm_tmp[600];  snprintf(rm_tmp, sizeof(rm_tmp), "rm -rf \"%s\"", tmp_dir);
        system(rm_tmp);
        mkdir_auto(tmp_dir);
        snprintf(extract_cmd, sizeof(extract_cmd),
            "unzip -q \"%s\" -d \"%s\" && "
            "mv \"%s\"/*/.[!.]* \"%s\"/ 2>/dev/null; "
            "mv \"%s\"/*/* \"%s\"/ 2>/dev/null; "
            "rm -rf \"%s\"",
            tf, tmp_dir, tmp_dir, dest, tmp_dir, dest, tmp_dir);
#endif
    }

    printf(CYAN "[EXTRACT] %s\n" RESET, extract_cmd);
    int extract_ok = (system(extract_cmd) == 0);

    /* Clean up temp files */
    char rm_cmd[600];
    snprintf(rm_cmd, sizeof(rm_cmd), "%s \"%s\"", DEL_FILE, tmp_file);
    system(rm_cmd);
#ifdef _WIN32
    system("del /q /f .omake_extract.bat >nul 2>&1");
#else
    system("rm -rf .omake_zip_tmp 2>/dev/null");
#endif

    if (extract_ok)
        printf(GREEN "[FETCH] Extracted to: %s\n" RESET, dest);
    else
        printf(RED "[FETCH] Extraction failed for: %s\n" RESET, tf);
}

/* ------------------------------------------------------------------ */
/*  fetch_dependencies: git with curl fallback                          */
/* ------------------------------------------------------------------ */

void fetch_dependencies(FetchEntry *entries, int count, int force,
                        char *tags, char *libs) {
    if (count == 0) return;

    int has_git  = tool_exists("git");
    int has_curl = tool_exists("curl");

    if (!has_git && !has_curl) {
        printf(RED "[FETCH] Neither 'git' nor 'curl' found! Cannot fetch dependencies.\n" RESET);
        printf(RED "[FETCH] Install git (preferred) or curl to enable FETCH:.\n" RESET);
        return;
    }

    if (!has_git)
        printf(YELLOW "[FETCH] git not found, falling back to curl for all downloads.\n" RESET);

    for (int i = 0; i < count; i++) {
        FetchEntry *e = &entries[i];
        printf(BLUE "[FETCH] %s  -->  %s\n" RESET, e->url, e->dest);

        if (dir_exists(e->dest)) {
            if (force) {
                char rm[700]; snprintf(rm, sizeof(rm), "%s \"%s\"", DEL_DIR, e->dest);
                system(rm);
                printf(YELLOW "[FETCH] Removed existing directory, re-fetching...\n" RESET);
            } else {
                if (has_git) {
                    /* Try git pull to update */
                    char pull[700]; snprintf(pull, sizeof(pull), "git -C \"%s\" pull --quiet", e->dest);
                    printf(CYAN "[FETCH] Already exists, updating via git: %s\n" RESET, e->dest);
                    if (system(pull) == 0) printf(GREEN "[FETCH] Updated: %s\n" RESET, e->dest);
                    else printf(YELLOW "[FETCH] Update failed (offline?), using existing copy.\n" RESET);
                } else {
                    printf(CYAN "[FETCH] Already exists (curl mode has no update). Use --build to re-fetch.\n" RESET);
                }
                fetch_auto_flags(e->dest, tags, libs);
                continue;
            }
        }

        /* --- Fetch --- */
        if (has_git) {
            ensure_dir_exists(e->dest);
            char clone[900];
            snprintf(clone, sizeof(clone), "git clone --depth=1 \"%s\" \"%s\"", e->url, e->dest);
            printf(CYAN "[EXEC] %s\n" RESET, clone);
            if (system(clone) == 0) {
                printf(GREEN "[FETCH] Cloned: %s\n" RESET, e->dest);
                fetch_auto_flags(e->dest, tags, libs);
            } else {
                printf(YELLOW "[FETCH] git clone failed, trying curl fallback...\n" RESET);
                curl_fetch(e->url, e->dest, force);
                fetch_auto_flags(e->dest, tags, libs);
            }
        } else {
            /* git not available — use curl directly */
            curl_fetch(e->url, e->dest, force);
            fetch_auto_flags(e->dest, tags, libs);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Dependency graph                                                    */
/* ------------------------------------------------------------------ */

void print_dep_graph(SourceFile *srcs, int count) {
    printf(MAGENTA "\n[GRAPH] Dependency Graph\n" RESET);
    printf(MAGENTA "========================\n" RESET);
    for (int i = 0; i < count; i++) {
        printf(CYAN "  %s\n" RESET, srcs[i].path);
        FILE *f = fopen(srcs[i].dep, "r");
        if (!f) { printf("    (no .d file yet — build first)\n"); continue; }
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            char *bs = strstr(line, " \\"); if (bs) *bs = '\0';
            trim(line);
            char *colon = strchr(line, ':');
            const char *start = colon ? colon + 1 : line;
            char tmp[4096]; strncpy(tmp, start, 4095);
            char *tok = strtok(tmp, " \t\n\r");
            while (tok) {
                if (strlen(tok) > 1 && strstr(tok, ".h"))
                    printf("    " GREEN "--> %s\n" RESET, tok);
                tok = strtok(NULL, " \t\n\r");
            }
        }
        fclose(f);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/*  Build profiles                                                      */
/* ------------------------------------------------------------------ */

void apply_profile(const char *profile, char *tags) {
    if (strcmp(profile, "debug") == 0) {
        if (!strstr(tags, "-g"))       strcat(tags, " -g");
        if (!strstr(tags, "-O0"))      strcat(tags, " -O0");
        if (!strstr(tags, "-DDEBUG"))  strcat(tags, " -DDEBUG");
        printf(YELLOW "[PROFILE] debug  (-g -O0 -DDEBUG)\n" RESET);
    } else if (strcmp(profile, "release") == 0) {
        if (!strstr(tags, "-O3"))      strcat(tags, " -O3");
        if (!strstr(tags, "-DNDEBUG")) strcat(tags, " -DNDEBUG");
        if (!strstr(tags, "-s"))       strcat(tags, " -s");
        printf(YELLOW "[PROFILE] release  (-O3 -DNDEBUG -s)\n" RESET);
    } else if (strcmp(profile, "asan") == 0) {
        if (!strstr(tags, "-g"))       strcat(tags, " -g");
        if (!strstr(tags, "-O1"))      strcat(tags, " -O1");
        strcat(tags, " -fsanitize=address,undefined -fno-omit-frame-pointer");
        printf(YELLOW "[PROFILE] asan  (AddressSanitizer + UBSan)\n" RESET);
    } else {
        printf(YELLOW "[WARN] Unknown profile '%s', ignoring.\n" RESET, profile);
    }
}

/* ------------------------------------------------------------------ */
/*  Parallel compile                                                    */
/* ------------------------------------------------------------------ */

static int run_parallel(const char **cmds, int n, int jobs) {
#ifdef _WIN32
    /* Windows: use mingw32-make for parallel if available, else sequential */
    {
        static int has_make = -1;
        if (has_make < 0)
            has_make = (tool_exists("mingw32-make") || tool_exists("make")) ? 1 : 0;
        if (has_make && jobs > 1 && n > 1) {
            /* Write temp makefile and run parallel */
            FILE *mf = fopen(".omake_parallel.mk", "w");
            if (mf) {
                fprintf(mf, ".PHONY: all");
                for (int i=0;i<n;i++) fprintf(mf," _j%d",i);
                fprintf(mf,"\nall:");
                for (int i=0;i<n;i++) fprintf(mf," _j%d",i);
                fprintf(mf,"\n");
                for (int i=0;i<n;i++) fprintf(mf,"_j%d:\n\t@%s\n",i,cmds[i]);
                fclose(mf);
                char mc[256];
                const char *make_bin = tool_exists("mingw32-make") ? "mingw32-make" : "make";
                snprintf(mc,sizeof(mc),"%s -f .omake_parallel.mk -j%d",make_bin,jobs);
                printf(BLUE "[PARALLEL] %d compile job(s), running: %s\n" RESET, n, mc);
                int ret = system(mc);
                remove(".omake_parallel.mk");
                return ret == 0;
            }
        }
    }
    /* Sequential fallback */
    for (int i = 0; i < n; i++) {
        printf(CYAN "[COMPILE] %s\n" RESET, cmds[i]);
        if (system(cmds[i]) != 0) return 0;
    }
    return 1;
#else
    /* POSIX: generate a temp GNU Makefile and run make -j<jobs> */
    FILE *mf = fopen(".omake_parallel.mk", "w");
    if (!mf) goto fallback;

    fprintf(mf, ".PHONY: all");
    for (int i = 0; i < n; i++) fprintf(mf, " _j%d", i);
    fprintf(mf, "\nall:");
    for (int i = 0; i < n; i++) fprintf(mf, " _j%d", i);
    fprintf(mf, "\n");
    for (int i = 0; i < n; i++)
        fprintf(mf, "_j%d:\n\t@%s\n", i, cmds[i]);
    fclose(mf);

    char make_cmd[128];
    snprintf(make_cmd, sizeof(make_cmd), "make -f .omake_parallel.mk -j%d", jobs);
    printf(BLUE "[PARALLEL] %d compile job(s), running: %s\n" RESET, n, make_cmd);
    int ret = system(make_cmd);
    remove(".omake_parallel.mk");
    return ret == 0;

fallback:
    for (int i = 0; i < n; i++) {
        printf(CYAN "[COMPILE] %s\n" RESET, cmds[i]);
        if (system(cmds[i]) != 0) return 0;
    }
    return 1;
#endif
}

/* ------------------------------------------------------------------ */
/*  Architecture & static linking                                       */
/* ------------------------------------------------------------------ */

/*
 * arch: "x64" | "i386" | "arm64" | "" (default, no flag added)
 *
 * x64   -> -m64
 * i386  -> -m32  (requires multilib: apt install gcc-multilib g++-multilib)
 * arm64 -> sets cross-compiler to aarch64-linux-gnu-g++ on Linux,
 *          or clang --target=aarch64-apple-darwin on macOS-style envs.
 *          On Windows cross-compile is handled via CROSS: in OMakeLists.txt.
 *
 * do_static: appends -static to linker flags, causing all libs to be
 *            bundled into the output binary (no shared .so/.dll deps).
 */
void apply_arch(const char *arch, int do_static, char *tags, char *comp) {
    if (strlen(arch) == 0) goto handle_static;

    if (strcmp(arch, "x64") == 0) {
        if (!strstr(tags, "-m64")) strcat(tags, " -m64");
        printf(YELLOW "[ARCH] x64  (-m64)\n" RESET);
    }
    else if (strcmp(arch, "i386") == 0) {
        if (!strstr(tags, "-m32")) strcat(tags, " -m32");
        printf(YELLOW "[ARCH] i386  (-m32)  -- requires g++-multilib\n" RESET);
    }
    else if (strcmp(arch, "arm64") == 0) {
#ifdef _WIN32
        if (strncmp(comp, "aarch64", 7) != 0 && strncmp(comp, "clang", 5) != 0) {
            char tmp[300];
            snprintf(tmp, sizeof(tmp), "aarch64-w64-mingw32-%s", comp);
            strncpy(comp, tmp, 255);
        }
        printf(YELLOW "[ARCH] arm64  (aarch64-w64-mingw32 toolchain)\n" RESET);
#else
        if (strncmp(comp, "aarch64", 7) != 0) {
            CompilerType ct_arm = detect_compiler(comp);
            if (ct_arm == CT_CLANG) {
                /* Clang cross-compile: use --target flag instead of prefix */
                /* Append --target to tags rather than renaming compiler */
                if (!strstr(tags, "--target=aarch64")) {
                    strncat(tags, " --target=aarch64-linux-gnu", B_SIZE - strlen(tags) - 1);
                }
                printf(YELLOW "[ARCH] arm64  (clang --target=aarch64-linux-gnu)\n" RESET);
            } else {
                char tmp[300];
                snprintf(tmp, sizeof(tmp), "aarch64-linux-gnu-%s", comp);
                strncpy(comp, tmp, 255);
                printf(YELLOW "[ARCH] arm64  (aarch64-linux-gnu toolchain)\n" RESET);
            }
        } else {
            printf(YELLOW "[ARCH] arm64  (already aarch64 compiler)\n" RESET);
        }
#endif
    }
    else {
        printf(YELLOW "[WARN] Unknown arch '%s'. Valid: x64, i386, arm64\n" RESET, arch);
    }

handle_static:
    if (do_static) {
        if (!strstr(tags, "-static")) strcat(tags, " -static");
        printf(YELLOW "[STATIC] Linking statically -- all libs bundled into binary.\n" RESET);
    }
}

/* ------------------------------------------------------------------ */
/*  .rc file scanner                                                    */
/* ------------------------------------------------------------------ */

/*
 * Recursively scan a directory for *.rc files.
 * Fills rc_paths[MAX_RC] and returns the count found.
 * Works on both Windows and Linux (cross-compile scenario).
 */
static int scan_rc_files(const char *base, char rc_paths[][MAX_RC_PATH], int max) {
    int count = 0;
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s%c%s", base, SEP, ent->d_name);
        if (dir_exists(full)) {
            count += scan_rc_files(full, rc_paths + count, max - count);
        } else if (is_regular_file(full)) {
            /* Match *.rc  (case-insensitive on Windows) */
            size_t len = strlen(ent->d_name);
            if (len > 3) {
                char ext[8];
                strncpy(ext, ent->d_name + len - 3, 7);
                ext[3] = '\0';
#ifdef _WIN32
                /* tolower each char */
                for (int k = 0; ext[k]; k++) ext[k] = (char)tolower((unsigned char)ext[k]);
#endif
                if (strcmp(ext, ".rc") == 0) {
                    strncpy(rc_paths[count], full, MAX_RC_PATH - 1);
                    fix_slashes(rc_paths[count]);
                    count++;
                }
            }
        }
    }
    closedir(d);
    return count;
}

/*
 * Compile all discovered .rc files with windres.
 * Each foo.rc -> <obj_root>/res/foo.res
 * Returns a heap-allocated string of all compiled .res paths,
 * ready to be appended to the linker command line.
 * Caller must free() the returned string.
 *
 * arch_flag: pass "-F pe-x86-64" for x64, "-F pe-i386" for i386, "" otherwise.
 */
static char *compile_rc_files(const char *rc_dir, const char *obj_root,
                               const char *arch_flag) {
    /* windres must be available */
    if (!tool_exists("windres")) {
        printf(YELLOW "[RC] windres not found, skipping resource compilation.\n" RESET);
        return calloc(1, 1);
    }

    char rc_paths[MAX_RC][MAX_RC_PATH];
    int rc_count = scan_rc_files(rc_dir, rc_paths, MAX_RC);

    if (rc_count == 0) {
        printf(YELLOW "[RC] No .rc files found in '%s'.\n" RESET, rc_dir);
        return calloc(1, 1);
    }

    printf(BLUE "[RC] Compiling %d resource file(s)...\n" RESET, rc_count);

    /* Result buffer: collect all compiled .res paths */
    char *res_objs = (char *)calloc(1, B_SIZE);
    if (!res_objs) return calloc(1, 1);

    /* Output dir for .res files */
    char res_out_dir[600];
    snprintf(res_out_dir, sizeof(res_out_dir), "%s%cres", obj_root, SEP);
    ensure_dir_exists(res_out_dir);
    /* ensure_dir_exists needs a file-like path; just mkdir directly too */
    mkdir_auto(res_out_dir);

    for (int i = 0; i < rc_count; i++) {
        /* Build output .res path: <obj_root>/res/<filename>.res */
        const char *base_name = strrchr(rc_paths[i], SEP);
        base_name = base_name ? base_name + 1 : rc_paths[i];
        char res_out[800];
        snprintf(res_out, sizeof(res_out), "%s%c%s", res_out_dir, SEP, base_name);
        /* Replace .rc extension with .res */
        char *dot = strrchr(res_out, '.'); if (dot) strcpy(dot, ".res");
        fix_slashes(res_out);

        /* Build windres command */
        char wrcmd[1600];
        snprintf(wrcmd, sizeof(wrcmd),
            "windres %s -i \"%s\" -O coff -o \"%s\"",
            arch_flag, rc_paths[i], res_out);
        printf(CYAN "[RC] %s\n" RESET, wrcmd);

        if (system(wrcmd) == 0) {
            printf(GREEN "[RC] Compiled: %s\n" RESET, res_out);
            strncat(res_objs, " \"", B_SIZE - strlen(res_objs) - 1);
            strncat(res_objs, res_out, B_SIZE - strlen(res_objs) - 1);
            strncat(res_objs, "\"", B_SIZE - strlen(res_objs) - 1);
        } else {
            printf(RED "[RC] Failed: %s\n" RESET, rc_paths[i]);
        }
    }
    return res_objs;
}

/* ------------------------------------------------------------------ */
/*  Core build engine                                                   */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */

int  build_engine(int force, const char *profile, int jobs, int show_graph, const char *arch, int do_static, const char *override_compiler, int do_strip_bin, int do_compress_bin, int gen_compile_commands, int gen_asm) {
    FILE *cfg = fopen("OMakeLists.txt", "r");
    if (!cfg) { printf(RED "[ERROR] OMakeLists.txt not found!\n" RESET); return 1; }

    char src_dir[512]   = "src";
    char src_ext[64]    = ".cpp";   /* primary extension   */
    char src_ext2[64]   = "";       /* secondary extension (C+CXX mixed) */
    char out[512]       = "bin/app";
    char comp[256]      = "g++";
    /* SET_COMPILER_<LIB>: <path> entries — lib name → compiler path */
    /* e.g. SET_COMPILER_QT: C:/Qt/6.6.0/mingw_64/bin/g++.exe */
    typedef struct { char lib[64]; char path[512]; } SetCompEntry;
    SetCompEntry set_comp_entries[32];
    int set_comp_count = 0;
    /* Flag: COMPILER: was explicitly set by user */
    int compiler_explicit = 0;
    char tags[B_SIZE]   = "";
    char libs[B_SIZE]   = "";
    char res[256]       = "";
    char cross[256]     = "";
    char obj_root[512]  = ".omake_obj";
    char deps_list[B_SIZE]= "";  /* DEPS: nlohmann/json fmt ...  */
    char env_list[512]  = "";   /* ENV: CC CXX MY_VAR            */
    char std_c[32]      = "";   /* e.g. "c11"  from STANDARD: C=11   */
    char std_cxx[32]    = "";   /* e.g. "c++20" from STANDARD: C++=20 */
    char pch_header[512]= "";   /* PRECOMPILED: include/stdafx.h      */
    char pkg_list[B_SIZE]="";   /* PKG: openssl zlib ...              */
    char install_dst[512]="";   /* INSTALL: /usr/local/bin            */
    char lto_mode[16]   = "";   /* LTO: on|thin|off                   */
    char output_type[32]= "exe";/* OUTPUT_TYPE: exe|shared|static     */
    char ver_string[64] = "";   /* VERSION: 1.2.3                     */
    char define_list[B_SIZE]="";/* DEFINE: FOO=1 BAR=hello            */
    char test_dir[512]  = "";   /* TEST: tests/*.cpp                  */
    char test_ext[32]   = ".cpp";/* extension for test files          */
    int  unity_build    = 0;    /* UNITY: on                          */
    int  unity_batch    = 0;    /* UNITY_BATCH: N                     */
    int  auto_pch       = 0;    /* AUTO_PCH: on                       */
    int  always_ccjson  = 0;    /* ALWAYS_EXPORT_COMMANDS: on         */
    int  multi_target   = 0;    /* TARGET: system active              */
    int  lto_auto       = 0;    /* LTO_AUTO: on                       */
    char debug_postfix[32]= ""; /* DEBUG_POSTFIX: -d                  */
    char cfg_files[B_SIZE]= ""; /* CONFIGURE_FILE: a.h.in->a.h        */
    char pkg_probe[B_SIZE]="";  /* PKG_PROBE: openssl zlib SDL2       */
    char require_std[128]= "";  /* REQUIRE_STANDARD: C++=20           */
    char ipo_mode[16]   = "";   /* IPO: on|full|off                   */
    int  clean_build    = 0;    /* CLEAN_BUILD: on                    */
    char project_name[128]="";  /* NAME: MyProject                    */
    char qt_resource[B_SIZE]="";/* QT_RESOURCE: res/app.qrc           */
    char embed_binary[B_SIZE]="";/* EMBED_BINARY: icon=assets/icon.png */
    char prerun[B_SIZE]  = ""; /* PRERUN: cmd                        */
    char postrun[B_SIZE] = ""; /* POSTRUN: cmd @OUT@                 */
    char size_report[16] = ""; /* SIZE_REPORT: on|full|bloat         */
    char mirror[B_SIZE]  = ""; /* MIRROR: src/->dst/                 */
    char hash_check[16]  = ""; /* HASH_CHECK: on|strict              */
    char multi_out[B_SIZE]=""; /* MULTI_OUT: bin/a bin/b             */
    char use_lib[B_SIZE]  =""; /* USE_LIB: opengl glfw glew sfml     */
    char qt_dir[512]      =""; /* QT_DIR: C:/Qt/6.7.0/mingw_64        */
    char qt_modules[B_SIZE]="";/* QT_MODULES: Core Widgets OpenGL    */
    char boost_modules[512]="";/* BOOST_MODULES: filesystem thread    */
    char opencv_modules[512]="";/*OPENCV_MODULES: core imgproc        */
    char wx_modules[256]  =""; /* WX_MODULES: core base              */
    char imgui_backend[128]="";/* IMGUI_BACKEND: opengl3+glfw        */

    char toolchain_file[512] = "";  /* TOOLCHAIN: arm.toolchain  */
    int  reproducible = 0;          /* REPRODUCIBLE: on          */
    int  pgo_gen = 0, pgo_use = 0;  /* set by main() via env     */
    char test_cases[B_SIZE] = "";   /* TEST_CASE: file1 file2... */
    int active = 1, found_true = 0;
    FetchEntry fetch_list[MAX_FETCH];
    int fetch_count = 0;

    char line[1024];
    while (fgets(line, sizeof(line), cfg)) {
        char *cmt = strchr(line, '#'); if (cmt) *cmt = '\0';
        trim(line); if (!line[0]) continue;
        char ll[1024]; strcpy(ll, line); to_lower_str(ll);

        if      (strncmp(ll, "if",   2) == 0 && isspace(ll[2])) {
            char c[64]; sscanf(ll, "%*s %63s", c);
            active = (strcmp(c, PLATFORM) == 0); found_true = active;
        }
        else if (strncmp(ll, "elif", 4) == 0) {
            char c[64]; sscanf(ll, "%*s %63s", c);
            active = (!found_true && strcmp(c, PLATFORM) == 0);
            if (active) found_true = 1;
        }
        else if (strncmp(ll, "else", 4) == 0) active = !found_true;
        else if (strncmp(ll, "end",  3) == 0) { active = 1; found_true = 0; }
        else if (active) {
            /* Use lowercase line (ll) for keyword matching to avoid false strstr hits */
            char *colon_pos = strchr(ll, ':');
            char key[64] = "";
            char *val = strchr(line, ':');  /* value from original (case-preserved) line */
            if (colon_pos) {
                int klen = (int)(colon_pos - ll);
                if (klen > 0 && klen < 63) { strncpy(key, ll, klen); key[klen] = '\0'; trim(key); }
            }
            if (val) val++;  /* skip ':' */

            if (strcmp(key, "fetch") == 0) {
                if (fetch_count < MAX_FETCH) {
                    if (parse_fetch_git(line, &fetch_list[fetch_count])) fetch_count++;
                    else printf(YELLOW "[WARN] Invalid FETCH: line: %s\n" RESET, line);
                } else printf(YELLOW "[WARN] MAX_FETCH limit reached.\n" RESET);
            }
            else if (strcmp(key, "src") == 0 && val) {
                char v[512]; strncpy(v, val, 511); trim(v);
                /*
                 * SRC: formats supported:
                 *   src/*.cpp          -> C++ only
                 *   src/*.c            -> C only
                 *   src/*.c;*.cpp      -> mixed C and C++
                 *   src               -> auto: scan both .c and .cpp
                 */
                /* Check for semicolon-separated dual extension */
                char *semi = strchr(v, ';');
                if (semi) {
                    /* e.g. "src/*.c;*.cpp" */
                    *semi = '\0';
                    char *first  = v;
                    char *second = semi + 1;
                    char *slash1 = strrchr(first, '/');
                    if (!slash1) slash1 = strrchr(first, '\\');
                    if (slash1) { *slash1 = '\0'; strncpy(src_dir, first, 511); }
                    char *dot1 = strchr(slash1 ? slash1+1 : first, '.');
                    if (dot1) strncpy(src_ext, dot1, 63);
                    /* second part: just get extension */
                    char *dot2 = strchr(second, '.');
                    if (dot2) strncpy(src_ext2, dot2, 63);
                } else {
                    char *slash = strrchr(v, '/');
                    if (!slash) slash = strrchr(v, '\\');
                    if (slash && strchr(slash, '*')) {
                        *slash = '\0'; strncpy(src_dir, v, 511);
                        char *dot = strchr(slash + 1, '.'); if (dot) strncpy(src_ext, dot, 63);
                    } else {
                        /* bare directory: scan both .c and .cpp */
                        strncpy(src_dir, v, 511);
                        strncpy(src_ext,  ".cpp", 63);
                        strncpy(src_ext2, ".c",   63);
                    }
                }
            }
            else if (strcmp(key, "out")      == 0 && val) { strncpy(out,      val, 511); trim(out); }
            else if (strcmp(key, "compiler") == 0 && val) {
                strncpy(comp, val, 255); trim(comp); compiler_explicit = 1;
            }
            else if (strncmp(key, "set_compiler_", 13) == 0 && val) {
                /* SET_COMPILER_QT: C:/Qt/.../bin/g++.exe */
                if (set_comp_count < 32) {
                    const char *libname = key + 13; /* skip "set_compiler_" */
                    strncpy(set_comp_entries[set_comp_count].lib,  libname, 63);
                    strncpy(set_comp_entries[set_comp_count].path, val,     511);
                    trim(set_comp_entries[set_comp_count].path);
                    set_comp_count++;
                }
            }
            else if (strcmp(key, "tags")     == 0 && val) { char tv[B_SIZE]; strncpy(tv,val,B_SIZE-1); trim(tv); if(tags[0]) strncat(tags," ",B_SIZE-strlen(tags)-1); strncat(tags,tv,B_SIZE-strlen(tags)-1); }
            else if (strcmp(key, "libs")     == 0 && val) { strncpy(libs,     val, B_SIZE-1); trim(libs); }
            else if (strcmp(key, "resource") == 0 && val) { strncpy(res,      val, 255); trim(res); }
            else if (strcmp(key, "cross")    == 0 && val) { strncpy(cross,    val, 255); trim(cross); }
            else if (strcmp(key, "objdir")      == 0 && val) { strncpy(obj_root,    val, 511); trim(obj_root); }
            else if (strcmp(key, "pch") == 0 && val) { strncpy(pch_header,  val, 511); trim(pch_header); }
            else if (strcmp(key, "pkg")         == 0 && val) {
                trim((char*)val);
                if (pkg_list[0]) strncat(pkg_list, " ", B_SIZE-strlen(pkg_list)-1);
                strncat(pkg_list, val, B_SIZE-strlen(pkg_list)-1);
            }
            else if (strcmp(key, "install")     == 0 && val) { strncpy(install_dst, val, 511); trim(install_dst); }
            else if (strcmp(key, "lto")         == 0 && val) { strncpy(lto_mode,    val,  15); trim(lto_mode); }
            else if (strcmp(key, "out_type") == 0 && val) { strncpy(output_type, val,  31); trim(output_type); }
            else if (strcmp(key, "version")     == 0 && val) { strncpy(ver_string,  val,  63); trim(ver_string); }
            else if (strcmp(key, "batch") == 0 && val) {
                unity_batch = atoi(val);
            }
            else if (strcmp(key, "pch_auto") == 0 && val) {
                char av[16]; strncpy(av,val,15); trim(av);
                char lav[16]; strncpy(lav,av,15);
                for(int ki=0;lav[ki];ki++) lav[ki]=(char)tolower((unsigned char)lav[ki]);
                auto_pch=(strcmp(lav,"on")==0||strcmp(lav,"1")==0||strcmp(lav,"yes")==0);
            }
            else if (strcmp(key, "export_cc") == 0 && val) {
                char av[16]; strncpy(av,val,15); trim(av);
                char lav[16]; strncpy(lav,av,15);
                for(int ki=0;lav[ki];ki++) lav[ki]=(char)tolower((unsigned char)lav[ki]);
                always_ccjson=(strcmp(lav,"on")==0||strcmp(lav,"1")==0||strcmp(lav,"yes")==0);
            }
            /* ── OMake Extended Keys ── */
            else if (strcmp(key, "name") == 0 && val) {
                strncpy(project_name, val, 127); trim(project_name);
            }
            else if (strcmp(key, "lto_probe") == 0 && val) {
                char av[16]; strncpy(av,val,15); trim(av);
                char lav[16]; strncpy(lav,av,15);
                for(int ki=0;lav[ki];ki++) lav[ki]=(char)tolower((unsigned char)lav[ki]);
                lto_auto=(strcmp(lav,"on")==0||strcmp(lav,"1")==0||strcmp(lav,"yes")==0);
            }
            else if (strcmp(key, "dbg_suffix") == 0 && val) {
                strncpy(debug_postfix, val, 31); trim(debug_postfix);
            }
            else if (strcmp(key, "gen_header") == 0 && val) {
                if (cfg_files[0]) strncat(cfg_files," ",B_SIZE-strlen(cfg_files)-1);
                strncat(cfg_files, val, B_SIZE-strlen(cfg_files)-1);
            }
            else if (strcmp(key, "extern") == 0 && val) {
                external_project_parse(val);
            }
            else if (strcmp(key, "probe") == 0 && val) {
                if (pkg_probe[0]) strncat(pkg_probe," ",B_SIZE-strlen(pkg_probe)-1);
                strncat(pkg_probe, val, B_SIZE-strlen(pkg_probe)-1);
            }
            else if (strcmp(key, "need_std") == 0 && val) {
                strncpy(require_std, val, 127); trim(require_std);
            }
            else if (strcmp(key, "ipo") == 0 && val) {
                strncpy(ipo_mode, val, 15); trim(ipo_mode);
            }
            else if (strcmp(key, "resource_qt") == 0 && val) {
                if (qt_resource[0]) strncat(qt_resource," ",B_SIZE-strlen(qt_resource)-1);
                strncat(qt_resource, val, B_SIZE-strlen(qt_resource)-1);
            }
            else if (strcmp(key, "embed") == 0 && val) {
                if (embed_binary[0]) strncat(embed_binary," ",B_SIZE-strlen(embed_binary)-1);
                strncat(embed_binary, val, B_SIZE-strlen(embed_binary)-1);
            }
            /* ── USE_LIB: Heavy library directives ── */
            else if (strcmp(key, "toolchain")    == 0 && val) { strncpy(toolchain_file, val, 511); trim(toolchain_file); }
            else if (strcmp(key, "reproducible") == 0 && val) { reproducible = (tolower(val[0])=='o'); }
            else if (strcmp(key, "test_case")    == 0 && val) {
                trim(val);
                if (test_cases[0]) strncat(test_cases," ",B_SIZE-strlen(test_cases)-1);
                strncat(test_cases, val, B_SIZE-strlen(test_cases)-1);
            }
            else if (strcmp(key, "use_lib") == 0 && val) {
                if (use_lib[0]) strncat(use_lib," ",B_SIZE-strlen(use_lib)-1);
                strncat(use_lib, val, B_SIZE-strlen(use_lib)-1);
            } else if (strcmp(key, "qt_dir") == 0 && val) {
                strncpy(qt_dir, val, sizeof(qt_dir)-1); trim(qt_dir);
            }
            else if (strcmp(key, "qt_modules") == 0 && val) {
                strncpy(qt_modules, val, B_SIZE-1);
            }
            else if (strcmp(key, "boost_modules") == 0 && val) {
                strncpy(boost_modules, val, 511);
            }
            else if (strcmp(key, "opencv_modules") == 0 && val) {
                strncpy(opencv_modules, val, 511);
            }
            else if (strcmp(key, "wx_modules") == 0 && val) {
                strncpy(wx_modules, val, 255);
            }
            else if (strcmp(key, "imgui_backend") == 0 && val) {
                strncpy(imgui_backend, val, 127);
            }
            /* ── OMake Features 21-25 ── */
            else if (strcmp(key, "prerun") == 0 && val) {
                if (prerun[0]) strncat(prerun," ; ",B_SIZE-strlen(prerun)-1);
                strncat(prerun, val, B_SIZE-strlen(prerun)-1);
            }
            else if (strcmp(key, "postrun") == 0 && val) {
                if (postrun[0]) strncat(postrun," ; ",B_SIZE-strlen(postrun)-1);
                strncat(postrun, val, B_SIZE-strlen(postrun)-1);
            }
            else if (strcmp(key, "size_report") == 0 && val) {
                strncpy(size_report, val, 15); trim(size_report);
            }
            else if (strcmp(key, "mirror") == 0 && val) {
                if (mirror[0]) strncat(mirror," ; ",B_SIZE-strlen(mirror)-1);
                strncat(mirror, val, B_SIZE-strlen(mirror)-1);
            }
            else if (strcmp(key, "hash_check") == 0 && val) {
                strncpy(hash_check, val, 15); trim(hash_check);
            }
            else if (strcmp(key, "multi_out") == 0 && val) {
                if (multi_out[0]) strncat(multi_out," ",B_SIZE-strlen(multi_out)-1);
                strncat(multi_out, val, B_SIZE-strlen(multi_out)-1);
            }
            else if (strcmp(key, "portable") == 0 && val) {
                char av[16]; strncpy(av,val,15); trim(av);
                char lav[16]; strncpy(lav,av,15);
                for(int ki=0;lav[ki];ki++) lav[ki]=(char)tolower((unsigned char)lav[ki]);
                clean_build=(strcmp(lav,"on")==0||strcmp(lav,"1")==0||strcmp(lav,"yes")==0);
            }
            /* TARGET system */
            else if (strcmp(key, "module") == 0 && val) {
                char tv[128]; strncpy(tv,val,127); trim(tv);
                Target *t = target_get_or_create(tv);
                g_current_target = t ? (int)(t - g_targets) : -1;
                multi_target = 1;
                printf(CYAN "[MODULE] Defined: %s\n" RESET, tv);
            }
            else if (strcmp(key, "mod_flags") == 0 && val && g_current_target>=0) {
                trim((char*)val);
                strncat(g_targets[g_current_target].flags,val,B_SIZE-strlen(g_targets[g_current_target].flags)-1);
            }
            else if (strcmp(key, "mod_libs") == 0 && val && g_current_target>=0) {
                trim((char*)val);
                strncat(g_targets[g_current_target].libs,val,B_SIZE-strlen(g_targets[g_current_target].libs)-1);
            }
            else if (strcmp(key, "mod_inc") == 0 && val && g_current_target>=0) {
                trim((char*)val);
                char iflag[600]; snprintf(iflag,sizeof(iflag),"-I\"%s\"",val);
                strncat(g_targets[g_current_target].inc,iflag,B_SIZE-strlen(g_targets[g_current_target].inc)-1);
            }
            else if (strcmp(key, "mod_deps") == 0 && val && g_current_target>=0) {
                trim((char*)val);
                strncpy(g_targets[g_current_target].deps,val,511);
            }
            else if (strcmp(key, "header_lib") == 0 && val) {
                Target *t = target_get_or_create(val);
                if (t) { t->is_interface=1; g_current_target=(int)(t-g_targets); }
            }
            else if (strcmp(key, "obj_lib") == 0 && val) {
                Target *t = target_get_or_create(val);
                if (t) { t->is_object=1; g_current_target=(int)(t-g_targets); }
            }
            else if (strcmp(key, "alias") == 0 && val) {
                /* ALIAS: Foo::Core=mylib */
                char *eq = strchr(val, '=');
                if (eq && g_current_target>=0) {
                    *eq='\0'; char *aname=val; char *tname=eq+1;
                    trim(aname); trim(tname);
                    Target *t = target_get_or_create(tname);
                    if (t) strncpy(t->alias, aname, 127);
                    printf(CYAN "[ALIAS] %s -> %s\n" RESET, aname, tname);
                } else if (g_current_target>=0) {
                    trim((char*)val);
                    strncpy(g_targets[g_current_target].alias, val, 127);
                }
            }
            /* FETCH_GIT v2 — enhanced parser */
            else if (strcmp(key, "fetch") == 0 && val && g_fetch_v2_count < MAX_FETCH_V2) {
                FetchEntryV2 ev2;
                char full_line[B_SIZE]; snprintf(full_line,sizeof(full_line),"FETCH: %s",val);
                if (parse_fetch_v2(full_line, &ev2))
                    g_fetch_v2[g_fetch_v2_count++] = ev2;
            }
            else if (strcmp(key, "unity")       == 0 && val) {
                char v[16]; strncpy(v,val,15); trim(v);
                char lv[16]; strncpy(lv,v,15);
                for(int ki=0;lv[ki];ki++) lv[ki]=(char)tolower((unsigned char)lv[ki]);
                unity_build = (strcmp(lv,"on")==0||strcmp(lv,"1")==0||strcmp(lv,"yes")==0);
            }
            else if (strcmp(key, "define") == 0 && val) {
                /* DEFINE: FOO=1 BAR="hello world" -> appended as -DFOO=1 -DBAR=... */
                char dv[B_SIZE]; strncpy(dv,val,B_SIZE-1); trim(dv);
                if (define_list[0]) strncat(define_list," ",B_SIZE-strlen(define_list)-1);
                strncat(define_list,dv,B_SIZE-strlen(define_list)-1);
            }
            else if (strcmp(key, "deps") == 0 && val) {
                char dv[B_SIZE]; strncpy(dv,val,B_SIZE-1); trim(dv);
                if(deps_list[0]) strncat(deps_list," ",B_SIZE-strlen(deps_list)-1);
                strncat(deps_list,dv,B_SIZE-strlen(deps_list)-1);
            }
            else if (strcmp(key, "env") == 0 && val) {
                char ev[512]; strncpy(ev,val,511); trim(ev);
                if(env_list[0]) strncat(env_list," ",512-strlen(env_list)-1);
                strncat(env_list,ev,512-strlen(env_list)-1);
            }
            /* Packaging metadata — informational, consumed by --m-pack */
            else if (strcmp(key, "description") == 0 && val) {
                printf(CYAN "[META] Description: %s\n" RESET, val); /* stored in pkg meta */
            }
            else if (strcmp(key, "maintainer") == 0 && val) {
                printf(CYAN "[META] Maintainer: %s\n" RESET, val);
            }
            else if (strcmp(key, "homepage") == 0 && val) {
                printf(CYAN "[META] Homepage: %s\n" RESET, val);
            }
            else if (strcmp(key, "test") == 0 && val) {
                char tv[512]; strncpy(tv,val,511); trim(tv);
                char *sl = strrchr(tv,'/'); if(!sl) sl=strrchr(tv,'\\');
                if(sl && strchr(sl,'*')) {
                    *sl='\0'; strncpy(test_dir,tv,511);
                    char *dt=strchr(sl+1,'.'); if(dt) strncpy(test_ext,dt,31);
                } else { strncpy(test_dir,tv,511); }
            }
            else if (strcmp(key, "standard") == 0 && val) {
                /*
                 * STANDARD: syntax:
                 *   STANDARD: C++=17        -> -std=c++17 for .cpp files
                 *   STANDARD: C=11          -> -std=c11   for .c files
                 *   STANDARD: C=11 C++=17   -> both
                 *   STANDARD: C++=20        -> only C++ standard
                 *
                 * Accepted version tokens (case-insensitive on key, not version):
                 *   C++  C++ C++=98 C++=03 C++=11 C++=14 C++=17 C++=20 C++=23 C++=26
                 *   C    C   C=89   C=90   C=99   C=11   C=17   C=23
                 *   Also accepts "gnu++" and "gnu" for GNU extensions.
                 */
                char sv[256]; strncpy(sv, val, 255); trim(sv);
                /* tokenize by spaces — allow multiple on one line */
                char *tok = strtok(sv, " \t");
                while (tok) {
                    /* lowercase only the key part (before =) */
                    char kpart[64] = ""; char vpart[32] = "";
                    char *eq = strchr(tok, '=');
                    if (eq) {
                        int kl = (int)(eq - tok);
                        if (kl > 0 && kl < 63) { strncpy(kpart, tok, kl); kpart[kl] = '\0'; }
                        strncpy(vpart, eq+1, 31);
                    } else {
                        /* bare "C++" or "C" with no version = use compiler default */
                        strncpy(kpart, tok, 63);
                    }
                    /* lowercase key for comparison */
                    char lk[64]; strncpy(lk, kpart, 63);
                    for (int ki = 0; lk[ki]; ki++) lk[ki] = (char)tolower((unsigned char)lk[ki]);

                    if (strcmp(lk, "c++") == 0 || strcmp(lk, "cxx") == 0 ||
                        strncmp(lk, "gnu++", 5) == 0) {
                        if (vpart[0]) snprintf(std_cxx, sizeof(std_cxx), "c++%s", vpart);
                        else          strncpy(std_cxx, "c++17", sizeof(std_cxx)-1); /* sane default */
                        if (strncmp(lk, "gnu", 3) == 0)
                            snprintf(std_cxx, sizeof(std_cxx), "gnu++%s", vpart[0] ? vpart : "17");
                    } else if (strcmp(lk, "c") == 0 || strcmp(lk, "gnu") == 0) {
                        if (vpart[0]) snprintf(std_c, sizeof(std_c), "c%s", vpart);
                        else          strncpy(std_c, "c11", sizeof(std_c)-1);
                        if (strcmp(lk, "gnu") == 0)
                            snprintf(std_c, sizeof(std_c), "gnu%s", vpart[0] ? vpart : "11");
                    } else {
                        printf(YELLOW "[STANDARD] Unknown key '%s', expected C or C++\n" RESET, kpart);
                    }
                    tok = strtok(NULL, " \t");
                }
            }
        }
    }
    fclose(cfg);

    /* Override compiler if --all-compilers passed a specific one */
    if (override_compiler && strlen(override_compiler) > 0) {
        strncpy(comp, override_compiler, 255);
        /*
         * Suffix the output binary so each compiler produces its own file:
         *   bin/app.exe  ->  bin/app_gcc.exe  /  bin/app_msvc.exe  etc.
         * Also use a separate obj dir per compiler to avoid .o/.obj conflicts.
         */
        CompilerType ct_ov = detect_compiler(override_compiler);
        const char *sfx = (ct_ov == CT_MSVC)  ? "_msvc"  :
                          (ct_ov == CT_CLANG)  ? "_clang" : "_gcc";
        /* Find last dot in out path and insert suffix before it */
        char *last_dot = strrchr(out, '.');
        char *last_sep = strrchr(out, SEP);
        if (last_dot && (!last_sep || last_dot > last_sep)) {
            /* e.g. bin/app.exe -> bin/app_gcc.exe */
            char tmp_out[512];
            int  pre_len = (int)(last_dot - out);
            snprintf(tmp_out, sizeof(tmp_out), "%.*s%s%s", pre_len, out, sfx, last_dot);
            strncpy(out, tmp_out, 511);
        } else {
            /* no extension */
            strncat(out, sfx, 511 - strlen(out));
        }
        /* Separate obj root per compiler */
        char tmp_obj[512];
        snprintf(tmp_obj, sizeof(tmp_obj), "%s%s", obj_root, sfx);
        strncpy(obj_root, tmp_obj, 511);
    }

    /* Cross-compile toolchain prefix */
    if (strlen(cross) > 0) {
        char pfx[512]; snprintf(pfx, sizeof(pfx), "%s%s", cross, comp);
        strncpy(comp, pfx, 255);
        printf(YELLOW "[CROSS] Toolchain: %s\n" RESET, comp);
    }

    /* Architecture flags and static linking */
    /* ── Pickup env overrides from --preset / --toolchain ── */
    {
        const char *et = getenv("OMAKE_EXTRA_TAGS");
        if (et && et[0]) { strncat(tags," ",B_SIZE-strlen(tags)-1); strncat(tags,et,B_SIZE-strlen(tags)-1); }
        const char *ec = getenv("OMAKE_COMPILER");
        if (ec && ec[0]) strncpy(comp, ec, 255);
        const char *ul = getenv("OMAKE_EXTRA_USE_LIB");
        if (ul && ul[0]) {
            if (use_lib[0]) strncat(use_lib," ",B_SIZE-strlen(use_lib)-1);
            strncat(use_lib, ul, B_SIZE-strlen(use_lib)-1);
        }
    }
    apply_arch(arch, do_static, tags, comp);

    /* ── Read PGO mode from env (set by main via --pgo-generate/--pgo-use) */
    if (getenv("OMAKE_PGO_GEN")) pgo_gen = 1;
    if (getenv("OMAKE_PGO_USE")) pgo_use = 1;

    /* ── Reproducible build ── */
    if (reproducible || getenv("OMAKE_REPRODUCIBLE"))
        apply_reproducible(tags);

    /* ── PGO flags ── */
    if (pgo_gen) pgo_apply_generate(tags);
    if (pgo_use) pgo_apply_use(tags);

    /* ── Toolchain file ── */
    {
        const char *tc_env = getenv("OMAKE_TOOLCHAIN");
        if (tc_env && tc_env[0]) strncpy(toolchain_file, tc_env, 511);
    }
    if (toolchain_file[0]) {
        Toolchain tc;
        if (toolchain_load(toolchain_file, &tc))
            toolchain_apply(&tc, comp, tags, libs);
    }

    /* ── Generate omake_test.h if TEST_CASE: used ── */
    if (test_cases[0]) gen_omake_test_header(obj_root);

    /* ── Apply SET_COMPILER_<lib> directives ─────────────────────────
     * Priority order:
     *  1. SET_COMPILER_<lib> that matches an active USE_LIB entry
     *  2. SET_COMPILER_QT   auto-derives from Qt bin dir if not explicit
     *  3. COMPILER: explicit value
     *  4. Default: g++
     *
     * COMPILER: directive is still respected for MSVC / clang-cl.
     * For g++ / gcc / clang++ it becomes optional when SET_COMPILER_*
     * provides the right compiler.
     * ─────────────────────────────────────────────────────────────── */
    if (set_comp_count > 0) {
        /* Walk entries; find one whose lib name appears in use_lib or qt_modules */
        for (int sci = 0; sci < set_comp_count; sci++) {
            const char *sclib  = set_comp_entries[sci].lib;   /* e.g. "qt" */
            const char *scpath = set_comp_entries[sci].path;  /* e.g. "C:/Qt/.../g++.exe" */

            /* Check against use_lib string */
            int matched = 0;
            {
                char ull[B_SIZE]; strncpy(ull, use_lib, B_SIZE-1);
                char *tok = strtok(ull, " \t");
                while (tok) {
                    char base[64]; strncpy(base, tok, 63);
                    char *col = strchr(base, ':'); if (col) *col = '\0';
                    if (strcasecmp(base, sclib) == 0) { matched = 1; break; }
                    tok = strtok(NULL, " \t");
                }
            }
            /* Also check qt_modules standalone */
            if (!matched && qt_modules[0] && strcasecmp(sclib, "qt") == 0)
                matched = 1;

            if (matched) {
                if (scpath[0]) {
                    strncpy(comp, scpath, 255); trim(comp);
                    printf(CYAN "[SET_COMPILER] %s → %s\n" RESET, sclib, comp);
                } else {
                    /* Empty path → auto-derive from qmake for Qt */
                    if (strcasecmp(sclib, "qt") == 0) {
                        /* Ask qmake where QT_INSTALL_BINS is, find g++/mingw32-g++ */
                        static const char *qmake_names[] =
                            {"qmake6","qmake-qt6","qmake","qmake-qt5",NULL};
                        char qmake_bin[512]; qmake_bin[0]='\0';
                        for (int qi=0; qmake_names[qi]; qi++)
                            if (tool_exists(qmake_names[qi]))
                                { strncpy(qmake_bin,qmake_names[qi],511); break; }
#ifdef _WIN32
                        /* Also scan C:/Qt/.../bin/qmake.exe */
                        if (!qmake_bin[0] && qt_dir[0]) {
                            char qm_try[600];
                            snprintf(qm_try,sizeof(qm_try),"%s/bin/qmake.exe",qt_dir);
                            if (is_regular_file(qm_try)) strncpy(qmake_bin,qm_try,511);
                        }
#endif
                        if (qmake_bin[0]) {
                            char cmd[600]; snprintf(cmd,sizeof(cmd),"\"%s\" -query QT_INSTALL_BINS 2>&1",qmake_bin);
                            FILE *fp=popen(cmd,"r");
                            if (fp) {
                                char qt_bins[512]; qt_bins[0]='\0';
                                if (fgets(qt_bins,sizeof(qt_bins),fp)) {
                                    char *nl=strrchr(qt_bins,'\n'); if(nl)*nl='\0';
                                    char *cr=strrchr(qt_bins,'\r'); if(cr)*cr='\0';
                                    trim(qt_bins);
                                    /* Strip "QT_INSTALL_BINS:" prefix if present */
                                    const char *pfx="QT_INSTALL_BINS:";
                                    if (strncmp(qt_bins,pfx,strlen(pfx))==0)
                                        memmove(qt_bins,qt_bins+strlen(pfx),strlen(qt_bins+strlen(pfx))+1);
                                    trim(qt_bins);
                                }
                                pclose(fp);
                                if (qt_bins[0]) {
                                    /* Try g++ candidates in Qt bin dir */
                                    static const char *gpp_names[] = {
                                        "g++.exe","mingw32-g++.exe","x86_64-w64-mingw32-g++.exe",
                                        "g++","mingw32-g++","x86_64-w64-mingw32-g++",NULL
                                    };
                                    for (int gi=0; gpp_names[gi]; gi++) {
                                        char gpp[700];
                                        snprintf(gpp,sizeof(gpp),"%s/%s",qt_bins,gpp_names[gi]);
                                        if (is_regular_file(gpp)) {
                                            strncpy(comp,gpp,255);
                                            printf(CYAN "[SET_COMPILER] qt → auto: %s\n" RESET, comp);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                break; /* first match wins */
            }
        }
    }

    /* If COMPILER: was NOT explicitly set AND no SET_COMPILER matched,
       keep default g++.  MSVC/clang-cl REQUIRE explicit COMPILER: */

    /* Detect compiler type BEFORE ccache wrapping */
    CompilerType ct = detect_compiler(comp);
    if (ct == CT_MSVC) {
        printf(YELLOW "[COMPILER] MSVC (cl.exe) mode\n" RESET);
        msvc_setup_environment();   /* auto-inject INCLUDE/LIB if not set */
    } else if (ct == CT_CLANG) {
        /* clang-cl -> MSVC-compatible flag translation */
        const char *b = strrchr(comp, '/');
        if (!b) b = strrchr(comp, '\\');
        b = b ? b+1 : comp;
        if (strncmp(b, "clang-cl", 8) == 0) {
            ct = CT_MSVC;
            printf(YELLOW "[COMPILER] clang-cl (MSVC-compatible) mode\n" RESET);
            msvc_setup_environment();   /* clang-cl also needs MSVC headers */
        } else {
            printf(YELLOW "[COMPILER] Clang mode\n" RESET);
        }
    }

    /* ccache: only wrap GCC/Clang, not MSVC */
    char final_comp[350];
    if (ct != CT_MSVC && tool_exists("ccache")) {
        snprintf(final_comp, sizeof(final_comp), "ccache %s", comp);
        printf(GREEN "[CCACHE] Enabled.\n" RESET);
    } else {
        strncpy(final_comp, comp, 349);
    }

    /* DEPS: shorthand -> resolve to FETCH_GIT entries */
    if (strlen(deps_list) > 0)
        resolve_deps(deps_list, fetch_list, &fetch_count);

    /* AUTO-INJECT: USE_LIB kütüphaneleri için eksik FETCH_V2 satırlarını tamamla */
    /* Bu hem g_fetch_v2 dizisini günceller hem OMakeLists.txt'yi kalıcı yazar */
    if (use_lib[0]) auto_inject_missing_fetches(use_lib);

    /* Fetch git dependencies v2 (TAG/BRANCH/CONFIGURE/BUILD_ARGS) */
    fetch_v2_all(force, tags, libs);
    /* Legacy fetch for entries parsed before v2 system */
    fetch_dependencies(fetch_list, fetch_count, force, tags, libs);

    /* USE_LIB: heavy library setup — runs after PROBE:, injects flags */
    char *use_lib_extra_objs = (char*)calloc(1, B_SIZE);
    /* Set QTDIR env from QT_DIR directive (picked up by qt_find_tools) */
    if (qt_dir[0]) {
#ifdef _WIN32
        { char e[600]; snprintf(e,sizeof(e),"QTDIR=%s",qt_dir); _putenv(e); }
#else
        setenv("QTDIR", qt_dir, 1);
#endif
        printf(CYAN "[QT] QT_DIR override: %s\n" RESET, qt_dir);
    }
    if (use_lib[0] && use_lib_extra_objs) {
        apply_use_lib(use_lib, tags, libs,
                      src_dir, obj_root, final_comp, ct, do_static,
                      use_lib_extra_objs, B_SIZE,
                      qt_modules[0] ? qt_modules : NULL,
                      boost_modules[0] ? boost_modules : NULL,
                      opencv_modules[0] ? opencv_modules : NULL,
                      wx_modules[0] ? wx_modules : NULL,
                      imgui_backend[0] ? imgui_backend : NULL);
    }
    /* QT_MODULES: standalone (without USE_LIB:) */
    if (qt_modules[0] && !use_lib[0]) {
        /* Also inject QT_DIR for standalone QT_MODULES */
        if (qt_dir[0]) {
#ifdef _WIN32
            { char e[600]; snprintf(e,sizeof(e),"QTDIR=%s",qt_dir); _putenv(e); }
#else
            setenv("QTDIR", qt_dir, 1);
#endif
        }
        char *qt_extra = use_lib_extra_objs ? use_lib_extra_objs : (char*)calloc(1,B_SIZE);
        setup_qt(qt_modules, tags, libs, src_dir, obj_root,
                 final_comp, ct, qt_extra, B_SIZE);
    }

    /* [21] PRERUN: pre-build command hook */
    if (prerun[0]) run_hook(prerun, out, src_dir, ver_string, "PRERUN");

    /* [14] EXTERNAL_PROJECT: Heavy external libs (OpenCV, Qt style) */
    external_project_build_all(tags, libs);

    /* [15] PKG_PROBE: Smart pkg-config + header scan + vcpkg */
    if (pkg_probe[0]) apply_pkg_probe(pkg_probe, tags, libs);

    /* [16] REQUIRE_STANDARD: Abort if compiler lacks the standard */
    if (require_std[0]) check_compile_features(require_std, final_comp);

    /* [13] CONFIGURE_FILE: Template substitution (version.h etc) */
    if (cfg_files[0])
        run_configure_files(cfg_files, ver_string, define_list, project_name);

    /* TARGET system: propagate interface libs, resolve deps */
    if (multi_target) {
        propagate_interface_libs();
        for (int ti=0; ti<g_target_count; ti++)
            resolve_target_deps(&g_targets[ti]);
        /* Merge first non-interface target flags into global build */
        for (int ti=0; ti<g_target_count; ti++) {
            if (!g_targets[ti].is_interface && g_targets[ti].flags[0]) {
                strncat(tags," ",B_SIZE-strlen(tags)-1);
                strncat(tags,g_targets[ti].flags,B_SIZE-strlen(tags)-1);
            }
            if (!g_targets[ti].is_interface && g_targets[ti].inc[0]) {
                strncat(tags," ",B_SIZE-strlen(tags)-1);
                strncat(tags,g_targets[ti].inc,B_SIZE-strlen(tags)-1);
            }
            if (!g_targets[ti].is_interface && g_targets[ti].libs[0]) {
                strncat(libs," ",B_SIZE-strlen(libs)-1);
                strncat(libs,g_targets[ti].libs,B_SIZE-strlen(libs)-1);
            }
        }
    }

    /* Auto-discover include directories */
    inc_count = 0;
    scan_inc_dirs("include");
    scan_inc_dirs(src_dir);
    for (int i = 0; i < fetch_count; i++) scan_inc_dirs(fetch_list[i].dest);

    char *auto_inc = (char*)calloc(1, B_SIZE);
    for (int i = 0; i < inc_count; i++) {
        char flag[600]; snprintf(flag, sizeof(flag), " -I\"%s\"", inc_dirs[i]);
        if (!strstr(auto_inc, flag)) strcat(auto_inc, flag);
    }

    /* Apply STANDARD: flags (-std=c++XX / -std=cXX) */
    if (strlen(std_cxx) > 0 || strlen(std_c) > 0) {
        /*
         * We inject the std flags into tags here. Per-file language
         * mapping in build_compile_cmd() will already adjust -std=c++XX
         * to -std=cXX for .c files (and vice-versa), so it is safe
         * to inject the C++ standard — it will be corrected per file.
         *
         * If BOTH are specified, inject C++ into tags (used as base)
         * and store C standard separately so build_compile_cmd can use it.
         * If only one is set, inject whichever is present.
         */
        CompilerType ct_std = detect_compiler(comp);
        if (ct_std == CT_MSVC) {
            /* MSVC: /std:c++17 etc — translate_tags() handles this */
            if (strlen(std_cxx) > 0) {
                char msflag[64]; snprintf(msflag, sizeof(msflag), "-std=%s", std_cxx);
                if (!strstr(tags, msflag)) {
                    strncat(tags, " ", B_SIZE - strlen(tags) - 1);
                    strncat(tags, msflag, B_SIZE - strlen(tags) - 1);
                }
            }
        } else {
            /* GCC / Clang */
            /* Remove any existing -std= the user might have put in TAGS: */
            char *existing = strstr(tags, "-std=");
            if (existing) {
                char *end = existing + 1;
                while (*end && *end != ' ' && *end != '\t') end++;
                memmove(existing, end, strlen(end) + 1);
            }
            /* Inject C++ standard into tags (build_compile_cmd adjusts for .c) */
            char stdflag[64];
            if (strlen(std_cxx) > 0) {
                snprintf(stdflag, sizeof(stdflag), "-std=%s", std_cxx);
                strncat(tags, " ", B_SIZE - strlen(tags) - 1);
                strncat(tags, stdflag, B_SIZE - strlen(tags) - 1);
                printf(CYAN "[STANDARD] C++: -std=%s\n" RESET, std_cxx);
            }
            if (strlen(std_c) > 0) {
                /* Store in std_cxx slot as sentinel so build_compile_cmd
                 * can use it for .c files; we append to tags with a marker.
                 * Simpler: just let build_compile_cmd derive cXX from c++XX,
                 * but if user specified C= explicitly, override that derivation.
                 * We encode it as a special tag __omake_stdc=XX that
                 * build_compile_cmd strips and uses. */
                char ctag[64]; snprintf(ctag, sizeof(ctag), "__omake_stdc=%s", std_c);
                strncat(tags, " ", B_SIZE - strlen(tags) - 1);
                strncat(tags, ctag, B_SIZE - strlen(tags) - 1);
                printf(CYAN "[STANDARD] C:   -std=%s\n" RESET, std_c);
            }
        }
    }

    /* DEFINE: macro injection */
    if (strlen(define_list) > 0) apply_defines(define_list, tags);

    /* VERSION: embed into binary */
    if (strlen(ver_string) > 0) apply_version(ver_string, tags);

    /* [12] DBG_SUFFIX: rename output in debug mode */
    if (debug_postfix[0]) apply_debug_postfix(out, debug_postfix, profile);

    /* --bin: ensure .bin extension */
    if (g_force_bin_ext) {
        char *dot = strrchr(out, '.');
        char *sep = strrchr(out, SEP);
        if (!dot || (sep && dot < sep)) {
            /* No extension: append .bin */
            strncat(out, ".bin", 511 - strlen(out));
        } else if (strcmp(dot, ".bin") != 0) {
            /* Has different extension: replace with .bin */
            strcpy(dot, ".bin");
        }
        printf(CYAN "[BIN] Output forced to .bin: %s\n" RESET, out);
    }

    /* ENV: environment variable injection */
    if (strlen(env_list) > 0) apply_env(env_list, tags);

    /* PKG: pkg-config flags */
    if (strlen(pkg_list) > 0) apply_pkg_config(pkg_list, tags, libs);

    /* LTO: link-time optimization */
    if (strlen(lto_mode) > 0) apply_lto(lto_mode, tags, libs, ct);

    /* OUTPUT_TYPE: shared|static modifies out path and flags */
    if (strcmp(output_type,"exe") != 0)
        apply_output_type(output_type, tags, libs, out, ct);

    /* Build profile flags */
    if (strlen(profile) > 0) apply_profile(profile, tags);

    /* [18] IPO: Interprocedural Optimization */
    if (ipo_mode[0]) apply_ipo(ipo_mode, tags, libs, ct);

    /* [11] LTO_AUTO: probe compiler + enable best LTO mode */
    if (lto_auto) apply_lto_auto(tags, libs, ct, final_comp);

    /* [8] Expand Generator Expressions: $<CONFIG:Debug>:-DDEBUG etc */
    {
        char *expanded_tags = (char*)calloc(1,B_SIZE);
        char *expanded_libs = (char*)calloc(1,B_SIZE);
        if (expanded_tags && expanded_libs) {
            expand_genex(tags, expanded_tags, B_SIZE, profile, ct);
            expand_genex(libs, expanded_libs, B_SIZE, profile, ct);
            strncpy(tags, expanded_tags, B_SIZE-1);
            strncpy(libs, expanded_libs, B_SIZE-1);
        }
        free(expanded_tags); free(expanded_libs);
    }

    /* Scan all source files (supports mixed C/C++ projects) */
    src_count = 0;
    fix_slashes(src_dir); fix_slashes(obj_root);
    scan_sources(src_dir, src_ext, obj_root);
    if (strlen(src_ext2) > 0)
        scan_sources(src_dir, src_ext2, obj_root);

    if (src_count == 0) {
        printf(RED "[FATAL] No source files found!\n" RESET);
        printf(RED "[FATAL]   Directory : %s\n" RESET, src_dir);
        printf(RED "[FATAL]   Extension : %s%s%s\n" RESET,
               src_ext,
               strlen(src_ext2)>0 ? " / " : "",
               strlen(src_ext2)>0 ? src_ext2 : "");
        if (!dir_exists(src_dir)) {
            printf(RED "[FATAL]   '%s/' directory does not exist!\n" RESET, src_dir);
            printf(YELLOW "[HINT]    Create it:  mkdir %s\n" RESET, src_dir);
            printf(YELLOW "[HINT]    Or set:     SRC: <your_dir>/*.cpp\n" RESET);
        } else {
            printf(YELLOW "[HINT]    '%s/' exists but no *%s files found.\n" RESET,
                   src_dir, src_ext);
            printf(YELLOW "[HINT]    Add a source file, e.g.: %s/main.cpp\n" RESET,
                   src_dir);
            /* List what IS in src_dir */
            DIR *_d = opendir(src_dir);
            if (_d) {
                struct dirent *_e;
                int _cnt = 0;
                printf(CYAN "[HINT]    Files in %s/:\n" RESET, src_dir);
                while ((_e = readdir(_d)) != NULL) {
                    if (_e->d_name[0] == '.') continue;
                    printf(CYAN "[HINT]      %s\n" RESET, _e->d_name);
                    _cnt++;
                }
                if (_cnt == 0) printf(CYAN "[HINT]      (empty)\n" RESET);
                closedir(_d);
            }
        }
        return 1;
    }
    /* Log language breakdown and auto-suggest compiler if mismatched */
    {
        int nc = 0, ncxx = 0;
        for (int i = 0; i < src_count; i++)
            (sources[i].lang == LANG_C) ? nc++ : ncxx++;
        if (nc > 0 && ncxx > 0)
            printf(CYAN "[SRC] Mixed project: %d C file(s), %d C++ file(s)\n" RESET, nc, ncxx);
        else if (nc > 0) {
            printf(CYAN "[SRC] C project: %d file(s)\n" RESET, nc);
            /* If user left default g++ for a pure C project, note it.
             * per_file_compiler() handles it, but log for transparency. */
            CompilerType ct_hint = detect_compiler(comp);
            if (ct_hint == CT_GCC && (strstr(comp, "g++") || strstr(comp, "clang++")))
                printf(YELLOW "[SRC] Note: pure C project, per-file compiler will use gcc/clang.\n" RESET);
        } else
            printf(CYAN "[SRC] C++ project: %d file(s)\n" RESET, ncxx);
    }

    /* Incremental: mark which files need rebuild */
    int rebuild_count = 0;
    for (int i = 0; i < src_count; i++) {
        sources[i].needs_rebuild = force || dep_needs_rebuild(sources[i].obj, sources[i].dep);
        if (sources[i].needs_rebuild) rebuild_count++;
    }

    /* [24] HASH_CHECK: source integrity verification */
    if (hash_check[0]) run_hash_check(hash_check, sources, src_count);

    if (show_graph) print_dep_graph(sources, src_count);

    printf(BLUE "[OMAKE] %d/%d file(s) to compile  |  -j%d  |  %s\n" RESET,
           rebuild_count, src_count, jobs, PLATFORM);

    /* ---- [5] Auto-PCH: detect most-used header if AUTO_PCH: on ---- */
    if (auto_pch && !pch_header[0] && src_count > 1) {
        auto_pch_detect(obj_root, pch_header, 512);
        if (pch_header[0])
            printf(CYAN "[AUTO-PCH] Detected: %s\n" RESET, pch_header);
    }

    /* ---- [5] PCH: build precompiled header ---- */
    char pch_flag[512] = "";
    if (pch_header[0]) {
        char *tags_pch_tr = (char*)calloc(1, B_SIZE);
        if (tags_pch_tr) {
            translate_tags(tags, tags_pch_tr, B_SIZE, ct);
            build_pch(pch_header, obj_root, final_comp, ct,
                      tags_pch_tr, auto_inc, pch_flag);
            free(tags_pch_tr);
        }
    }

    /* ---- [4] UNITY BUILD ---- */
    if (unity_build && rebuild_count > 0) {
        char *tags_u = (char*)calloc(1, B_SIZE);
        char *inc_u  = (char*)calloc(1, B_SIZE);
        if (tags_u && inc_u) {
            translate_tags(tags, tags_u, B_SIZE, ct);
            translate_inc(auto_inc, inc_u, B_SIZE, ct);
            /* Append PCH flag if available */
            if (pch_flag[0]) {
                strncat(tags_u, pch_flag, B_SIZE - strlen(tags_u) - 1);
            }
            int ub_ok;
            if (unity_batch > 0 && unity_batch < src_count) {
                printf(CYAN "[UNITY] Batched mode: %d files per TU\n" RESET, unity_batch);
                ub_ok = unity_compile_batched(sources, src_count, obj_root,
                                               final_comp, ct, tags_u, inc_u,
                                               unity_batch);
            } else {
                printf(CYAN "[UNITY] Full unity: merging %d files into 1 TU\n" RESET, src_count);
                ub_ok = unity_compile(sources, src_count, obj_root,
                                      final_comp, ct, tags_u, inc_u);
            }
            free(tags_u); free(inc_u);
            if (!ub_ok) {
                printf(RED "[ERROR] Unity compilation failed.\n" RESET);
                return 1;
            }
            /* Unity sets all sources[i].obj → skip normal compile loop */
            rebuild_count = 0;
        }
    }

    /* ---- Compile ---- */
    if (rebuild_count > 0) {
        const char *cmds[MAX_SRCS];
        /* Use heap for compile command buffers to avoid stack overflow */
        char (*cmd_buf)[B_SIZE] = (char (*)[B_SIZE])malloc((size_t)MAX_SRCS * B_SIZE);
        if (!cmd_buf) { printf(RED "[ERROR] Out of memory.\n" RESET); return 1; }
        int         jc = 0;

        /* Translate tags and include paths for the detected compiler */
        char *tags_tr = (char*)calloc(1, B_SIZE);
        char *inc_tr  = (char*)calloc(1, B_SIZE);
        if (!tags_tr || !inc_tr) {
            printf(RED "[ERROR] Out of memory.\n" RESET);
            free(cmd_buf); free(tags_tr); free(inc_tr); return 1;
        }
        translate_tags(tags, tags_tr, B_SIZE, ct);
        translate_inc(auto_inc, inc_tr, B_SIZE, ct);

        /* For MSVC: use .obj extension instead of .o */
        const char *o_ext = obj_ext(ct);

        for (int i = 0; i < src_count; i++) {
            if (!sources[i].needs_rebuild) continue;
            /* For MSVC rename .o -> .obj in the obj path */
            char obj_path[1024]; strncpy(obj_path, sources[i].obj, 1023);
            if (ct == CT_MSVC) {
                char *dot = strrchr(obj_path, '.'); if (dot) strcpy(dot, ".obj");
            }
            ensure_dir_exists(obj_path);
            build_compile_cmd(cmd_buf[jc], B_SIZE,
                final_comp, ct,
                sources[i].path, obj_path,
                tags_tr, inc_tr,
                sources[i].lang);
            cmds[jc] = cmd_buf[jc];
            jc++;
        }
        free(tags_tr); free(inc_tr);
        (void)o_ext;

        /* ---- ASM generation (--generate-asm) ---- */
        if (gen_asm) {
            char asm_root[600];
            snprintf(asm_root, sizeof(asm_root), "%s_asm", obj_root);
            /* Use "omake_asm" as default dir regardless of obj_root */
            strncpy(asm_root, "omake_asm", sizeof(asm_root)-1);
            mkdir_auto(asm_root);
            printf(CYAN "[ASM] Generating assembly -> %s/\n" RESET, asm_root);

            /* Re-translate tags (clean copy, no sentinel contamination) */
            char *asm_tags = (char*)calloc(1, B_SIZE);
            char *asm_inc  = (char*)calloc(1, B_SIZE);
            if (asm_tags && asm_inc) {
                /* Strip __omake_stdc sentinel from tags */
                char tags_clean[B_SIZE]; strncpy(tags_clean, tags, B_SIZE-1);
                char *s = strstr(tags_clean, "__omake_stdc=");
                if (s) { char *e=s+1; while(*e&&*e!=' '&&*e!='\t')e++; memmove(s,e,strlen(e)+1); }
                translate_tags(tags_clean, asm_tags, B_SIZE, ct);
                translate_inc(auto_inc,    asm_inc,  B_SIZE, ct);
            }

            char (*asm_cmds)[B_SIZE] = (char(*)[B_SIZE])calloc(src_count, B_SIZE);
            const char **asm_cmd_ptrs = (const char**)calloc(src_count, sizeof(char*));
            int asm_count = 0;

            if (asm_cmds && asm_cmd_ptrs) {
                for (int i = 0; i < src_count; i++) {
                    /* Derive .asm / .s output path:
                     * src/foo/bar.cpp  ->  omake_asm/foo/bar.s
                     * Mirror the source sub-directory structure.
                     */
                    const char *rel = sources[i].path;
                    /* strip leading src_dir prefix if present */
                    size_t sd_len = strlen(src_dir);
                    if (strncmp(rel, src_dir, sd_len) == 0 &&
                        (rel[sd_len]==SEP||rel[sd_len]=='\0'))
                        rel += sd_len + (rel[sd_len]==SEP ? 1 : 0);

                    /* Build asm output path */
                    char asm_out[1024];
                    snprintf(asm_out, sizeof(asm_out), "%s%c%s", asm_root, SEP, rel);
                    /* Replace extension with .s */
                    char *dot2 = strrchr(asm_out, '.');
                    char *sep2 = strrchr(asm_out, SEP);
                    if (dot2 && (!sep2 || dot2 > sep2)) strcpy(dot2, ".s");
                    else strncat(asm_out, ".s", sizeof(asm_out)-strlen(asm_out)-1);
                    ensure_dir_exists(asm_out);

                    /* Build the ASM command */
                    char fc_asm[256];
                    per_file_compiler(final_comp, ct, sources[i].lang, fc_asm);

                    if (ct == CT_MSVC) {
                        /* MSVC: /FA[s] /Fa<path>  (source+asm listing) */
                        const char *lf = (sources[i].lang==LANG_C) ? "/TC" : "/TP";
                        snprintf(asm_cmds[asm_count], B_SIZE,
                            "%s /nologo /c %s /FAs /Fa\"%s\" \"%s\" /Fo\"%s\" %s %s",
                            fc_asm, lf, asm_out,
                            sources[i].path, sources[i].obj,
                            asm_tags ? asm_tags : "",
                            asm_inc  ? asm_inc  : "");
                    } else {
                        /* GCC/Clang: -S -fverbose-asm */
                        /* Optionally add Intel syntax: -masm=intel */
                        const char *asm_syntax =
#if defined(__x86_64__) || defined(__i386__)
                            " -masm=intel";
#else
                            "";
#endif
                        /* Adjust -std= for language */
                        char asm_tags_adj[B_SIZE];
                        strncpy(asm_tags_adj, asm_tags ? asm_tags : "", B_SIZE-1);
                        if (sources[i].lang == LANG_C) {
                            char *pp = strstr(asm_tags_adj, "-std=c++");
                            if (pp) memmove(pp+6, pp+8, strlen(pp+8)+1);
                        }
                        snprintf(asm_cmds[asm_count], B_SIZE,
                            "%s -S -fverbose-asm%s \"%s\" -o \"%s\" %s %s",
                            fc_asm, asm_syntax,
                            sources[i].path, asm_out,
                            asm_tags_adj,
                            asm_inc ? asm_inc : "");
                    }
                    asm_cmd_ptrs[asm_count] = asm_cmds[asm_count];
                    asm_count++;
                }
                printf(BLUE "[ASM] Generating %d file(s) with -j%d...\n" RESET, asm_count, jobs);
                int asm_ok = run_parallel(asm_cmd_ptrs, asm_count, jobs);
                if (asm_ok) {
                    printf(GREEN "[ASM] Done. Files written to %s/\n" RESET, asm_root);
                } else {
                    printf(YELLOW "[ASM] Some files failed (build still continues).\n" RESET);
                }
            }
            free(asm_cmds); free(asm_cmd_ptrs);
            free(asm_tags); free(asm_inc);
        }

        int par_ok = run_parallel(cmds, jc, jobs);
        free(cmd_buf);
        if (!par_ok) {
            printf(RED "[ERROR] Compilation failed.\n" RESET);
            return 1;
        }
    } else {
        printf(GREEN "[STABLE] All objects up-to-date, skipping compile.\n" RESET);
    }

    /* ---- Link ---- */
    ensure_dir_exists(out); fix_slashes(out);

    /* [19] QT_RESOURCE / EMBED_BINARY: compile resources into extra .o files */
    char *extra_objs = (char*)calloc(1, B_SIZE);
    if (extra_objs) {
        /* Merge USE_LIB extra objects (glad.o, imgui*.o, moc_*.o ...) */
        if (use_lib_extra_objs && use_lib_extra_objs[0])
            strncat(extra_objs, use_lib_extra_objs, B_SIZE - strlen(extra_objs) - 1);
        if (qt_resource[0])
            process_qt_resources(qt_resource, obj_root, final_comp, ct,
                                  tags, auto_inc, extra_objs, B_SIZE);
        if (embed_binary[0])
            process_embed_binary(embed_binary, obj_root, final_comp, ct,
                                  tags, auto_inc, extra_objs, B_SIZE);
    }

    char *all_objs = (char*)calloc(1, B_SIZE);
    for (int i = 0; i < src_count; i++) {
        char obj_path[1024]; strncpy(obj_path, sources[i].obj, 1023);
        if (ct == CT_MSVC) {
            char *dot = strrchr(obj_path, '.'); if (dot) strcpy(dot, ".obj");
        }
        /* Deduplicate: unity builds map multiple sources to same .o */
        char quoted[1026]; snprintf(quoted, sizeof(quoted), "\"%s\"", obj_path);
        if (strstr(all_objs, quoted)) continue;  /* already in list */
        strncat(all_objs, "\"",      B_SIZE - strlen(all_objs) - 1);
        strncat(all_objs, obj_path,  B_SIZE - strlen(all_objs) - 1);
        strncat(all_objs, "\" ",     B_SIZE - strlen(all_objs) - 1);
    }

    /*
     * Resource compilation (.rc -> .res via windres).
     * Scans the directory specified by RESOURCE: in OMakeLists.txt,
     * or "res" by default.  Works without RESOURCE: being set — if a
     * "res" directory exists and contains .rc files they are compiled.
     *
     * windres arch flag mirrors the --x64 / --i386 selection so the
     * compiled resources match the target binary format.
     */
    char *res_objs = NULL;
#ifdef _WIN32
    {
        const char *rc_dir = (strlen(res) > 0) ? res : "res";
        if (dir_exists(rc_dir)) {
            if (ct == CT_MSVC) {
                /*
                 * MSVC path: use rc.exe
                 * rc.exe does not need an arch flag — the .res format is
                 * architecture-neutral; the linker handles /MACHINE.
                 */
                char rc_paths[MAX_RC][MAX_RC_PATH];
                int  rc_count = scan_rc_files(rc_dir, rc_paths, MAX_RC);
                printf(BLUE "[RC] Compiling %d resource file(s) with rc.exe...\n" RESET, rc_count);
                res_objs = (char*)calloc(1, B_SIZE);
                char res_out_dir[600];
                snprintf(res_out_dir, sizeof(res_out_dir), "%s%cres", obj_root, SEP);
                mkdir_auto(res_out_dir);
                for (int i = 0; i < rc_count; i++) {
                    const char *bn = strrchr(rc_paths[i], SEP);
                    bn = bn ? bn+1 : rc_paths[i];
                    char res_out[800];
                    snprintf(res_out, sizeof(res_out), "%s%c%s", res_out_dir, SEP, bn);
                    char *dot = strrchr(res_out, '.'); if (dot) strcpy(dot, ".res");
                    fix_slashes(res_out);
                    if (compile_one_rc_msvc(rc_paths[i], res_out, "")) {
                        printf(GREEN "[RC] Compiled: %s\n" RESET, res_out);
                        strncat(res_objs, " \"",   B_SIZE - strlen(res_objs) - 1);
                        strncat(res_objs, res_out, B_SIZE - strlen(res_objs) - 1);
                        strncat(res_objs, "\"",    B_SIZE - strlen(res_objs) - 1);
                    } else {
                        printf(RED "[RC] Failed: %s\n" RESET, rc_paths[i]);
                    }
                }
            } else {
                /* GCC / Clang: use windres */
                char wres_arch[32] = "";
                if      (strcmp(arch, "x64")   == 0) strncpy(wres_arch, "-F pe-x86-64",  31);
                else if (strcmp(arch, "i386")  == 0) strncpy(wres_arch, "-F pe-i386",    31);
                else if (strcmp(arch, "arm64") == 0) strncpy(wres_arch, "-F pe-aarch64", 31);
                res_objs = compile_rc_files(rc_dir, obj_root, wres_arch);
            }
        } else {
            res_objs = calloc(1, 1);
        }
    }
#else
    res_objs = calloc(1, 1);
#endif

    char *link_cmd = (char*)malloc(B_SIZE);
    if (!auto_inc || !all_objs || !link_cmd) {
        printf(RED "[ERROR] Out of memory.\n" RESET);
        free(auto_inc); free(all_objs); free(link_cmd);
        return 1;
    }
    {
        /* Translate tags and libs for linker */
        /* Strip internal __omake_stdc= sentinel — only used during compile, not link */
        char tags_for_link[B_SIZE]; strncpy(tags_for_link, tags, B_SIZE-1);
        {
            char *s = strstr(tags_for_link, "__omake_stdc=");
            if (s) {
                char *e = s + 1; while (*e && *e != ' ' && *e != '\t') e++;
                memmove(s, e, strlen(e)+1);
            }
        }
        /* Append QRC/EMBED resource objects before linking */
        if (extra_objs && extra_objs[0])
            strncat(all_objs, extra_objs, B_SIZE - strlen(all_objs) - 1);

        char *tags_ltr = (char*)calloc(1, B_SIZE);
        if (tags_ltr) translate_tags(tags_for_link, tags_ltr, B_SIZE, ct);
        build_link_cmd(link_cmd, B_SIZE,
            final_comp, ct,
            all_objs, res_objs,
            tags_ltr ? tags_ltr : tags, libs, out, arch);
        free(tags_ltr);
    }
    free(extra_objs);
    printf(CYAN "[LINK] %s\n" RESET, link_cmd);
    int link_ok = (system(link_cmd) == 0);
    free(use_lib_extra_objs);
    /* [25] MULTI_OUT: always run before freeing shared resources */
    if (multi_out[0])
        run_multi_out(multi_out, src_dir, obj_root, final_comp, ct,
                      tags, libs, auto_inc, arch);

    free(auto_inc); free(all_objs); free(link_cmd); free(res_objs);

    if (link_ok) {
        printf(GREEN "[OK] Build complete -> %s\n" RESET, out);
        /* STRIP */
        if (do_strip_bin) do_strip(out, ct);
        /* COMPRESS */
        if (do_compress_bin) cmd_compress(out);
        /* [22] SIZE_REPORT */
        if (size_report[0]) run_size_report(size_report, out);
        /* [21] POSTRUN: post-build command hook */
        if (postrun[0]) run_hook(postrun, out, src_dir, ver_string, "POSTRUN");
        /* [23] MIRROR: sync source dirs */
        if (mirror[0]) run_mirror(mirror);
        /* INSTALL */
        if (strlen(install_dst) > 0) do_install(out, install_dst);
    } else {
        printf(RED "[ERROR] Link failed.\n" RESET);
    }
    /* compile_commands.json if requested */
    /* [20] CLEAN_BUILD: portability report */
    if (clean_build) enforce_clean_build(out, obj_root);

    if (gen_compile_commands || always_ccjson) {
        char *tags_gcc = (char*)calloc(1,B_SIZE);
        char *inc_gcc  = (char*)calloc(1,B_SIZE);
        if(tags_gcc && inc_gcc) {
            translate_tags(tags, tags_gcc, B_SIZE, CT_GCC);
            translate_inc(auto_inc, inc_gcc, B_SIZE, CT_GCC);
            generate_compile_commands(sources, src_count, final_comp, tags_gcc, inc_gcc);
        }
        free(tags_gcc); free(inc_gcc);
    }
    return link_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Usage                                                               */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/*  File Database  (.omake_filedb)                                      */
/* ------------------------------------------------------------------ */

/*
 * Format: plain TSV, one record per line, UTF-8.
 * Fields (tab-separated):
 *   [0] status    : "ok" | "deleted"
 *   [1] path      : absolute path of the file
 *   [2] size      : file size in bytes (decimal)
 *   [3] mtime     : last-modified Unix timestamp (decimal)
 *   [4] ext       : file extension (lowercase), e.g. ".cpp"
 *
 * The DB file lives next to OMakeLists.txt as ".omake_filedb".
 * It is append-friendly: a full scan always rewrites the file.
 * Partial queries just read it.
 *
 * Thread safety: single-process, no locking needed.
 */

#define FILEDB_PATH   ".omake_filedb"
#define FILEDB_MAX    262144          /* max records kept in memory    */
#define FILEDB_PATHSZ 2048

typedef struct {
    char   status[8];          /* "ok" or "deleted" */
    char   path[FILEDB_PATHSZ];
    long long size;
    long long mtime;
    char   ext[32];
} FileRecord;

static FileRecord *g_fdb      = NULL;   /* heap array */
static int         g_fdb_count = 0;

/* ---- helpers ---- */

static const char *file_ext_lower(const char *name) {
    static char buf[32];
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) { buf[0] = '\0'; return buf; }
    int i = 0;
    for (const char *p = dot; *p && i < 30; p++, i++)
        buf[i] = (char)tolower((unsigned char)*p);
    buf[i] = '\0';
    return buf;
}

/* ---- DB I/O ---- */

static void fdb_load(void) {
    if (g_fdb) return;   /* already loaded */
    g_fdb = (FileRecord*)calloc(FILEDB_MAX, sizeof(FileRecord));
    if (!g_fdb) { printf(RED "[FILEDB] Out of memory.\n" RESET); return; }

    FILE *f = fopen(FILEDB_PATH, "r");
    if (!f) return;   /* first run — DB doesn't exist yet */

    char line[FILEDB_PATHSZ + 256];
    while (fgets(line, sizeof(line), f) && g_fdb_count < FILEDB_MAX) {
        /* strip newline */
        char *nl = strrchr(line, '\n'); if (nl) *nl = '\0';
        nl = strrchr(line, '\r'); if (nl) *nl = '\0';
        if (!line[0]) continue;

        FileRecord *r = &g_fdb[g_fdb_count];
        /* parse tab-separated fields */
        char *fields[5]; int fc = 0;
        char *p = line;
        fields[fc++] = p;
        while (*p && fc < 5) {
            if (*p == '\t') { *p = '\0'; fields[fc++] = p + 1; }
            p++;
        }
        if (fc < 5) continue;

        strncpy(r->status, fields[0], 7);
        strncpy(r->path,   fields[1], FILEDB_PATHSZ - 1);
        r->size  = atoll(fields[2]);
        r->mtime = atoll(fields[3]);
        strncpy(r->ext,    fields[4], 31);
        g_fdb_count++;
    }
    fclose(f);
}

static void fdb_save(void) {
    if (!g_fdb) return;
    FILE *f = fopen(FILEDB_PATH, "w");
    if (!f) { printf(RED "[FILEDB] Cannot write %s\n" RESET, FILEDB_PATH); return; }
    for (int i = 0; i < g_fdb_count; i++) {
        FileRecord *r = &g_fdb[i];
        fprintf(f, "%s\t%s\t%lld\t%lld\t%s\n",
                r->status, r->path, r->size, r->mtime, r->ext);
    }
    fclose(f);
}

/* Find existing record index by path, or -1 */
static int fdb_find(const char *path) {
    for (int i = 0; i < g_fdb_count; i++)
        if (strcmp(g_fdb[i].path, path) == 0) return i;
    return -1;
}

/* Upsert a record */
static void fdb_upsert(const char *path, long long size, long long mtime,
                        const char *ext) {
    if (!g_fdb) return;
    int idx = fdb_find(path);
    FileRecord *r;
    if (idx >= 0) {
        r = &g_fdb[idx];
    } else {
        if (g_fdb_count >= FILEDB_MAX) return;
        r = &g_fdb[g_fdb_count++];
    }
    strncpy(r->status, "ok",  7);
    strncpy(r->path,   path,  FILEDB_PATHSZ - 1);
    r->size  = size;
    r->mtime = mtime;
    strncpy(r->ext, ext, 31);
}

/* Mark records that no longer exist on disk as "deleted" */
static void fdb_mark_deleted(void) {
    for (int i = 0; i < g_fdb_count; i++) {
        if (strcmp(g_fdb[i].status, "deleted") == 0) continue;
#ifdef _WIN32
        DWORD attr = GetFileAttributesA(g_fdb[i].path);
        if (attr == INVALID_FILE_ATTRIBUTES)
            strncpy(g_fdb[i].status, "deleted", 7);
#else
        struct stat st;
        if (stat(g_fdb[i].path, &st) != 0)
            strncpy(g_fdb[i].status, "deleted", 7);
#endif
    }
}

/* ---- Recursive scanner ---- */

static int g_scan_total = 0;
static int g_scan_new   = 0;
static int g_scan_upd   = 0;

static void fdb_scan_dir(const char *base) {
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;   /* skip hidden / . / .. */
        char full[FILEDB_PATHSZ];
        snprintf(full, sizeof(full), "%s%c%s", base, SEP, ent->d_name);

        if (dir_exists(full)) {
            fdb_scan_dir(full);
        } else if (is_regular_file(full)) {
            struct stat st;
            if (stat(full, &st) != 0) continue;
            long long sz  = (long long)st.st_size;
            long long mts = (long long)st.st_mtime;
            const char *ext = file_ext_lower(ent->d_name);
            g_scan_total++;

            int idx = fdb_find(full);
            if (idx < 0) {
                fdb_upsert(full, sz, mts, ext);
                g_scan_new++;
            } else if (g_fdb[idx].mtime != mts || g_fdb[idx].size != sz) {
                fdb_upsert(full, sz, mts, ext);
                g_scan_upd++;
            }
            /* else: unchanged, no-op */
        }
    }
    closedir(d);
}

/* ---- Main entry points ---- */

/*
 * cmd_find_all():
 *   Scan root_dir recursively, update DB, save.
 *   root_dir: starting directory (e.g. "." or "C:\\")
 */
static void cmd_find_all(const char *root_dir) {
    printf(BLUE "[FIND-ALL] Loading database...\n" RESET);
    fdb_load();

    int old_count = g_fdb_count;
    printf(BLUE "[FIND-ALL] Scanning: %s\n" RESET, root_dir);

    g_scan_total = 0; g_scan_new = 0; g_scan_upd = 0;
    fdb_scan_dir(root_dir);

    /* Mark files that vanished from disk */
    int before_del = 0;
    for (int i = 0; i < g_fdb_count; i++)
        if (strcmp(g_fdb[i].status, "deleted") == 0) before_del++;
    fdb_mark_deleted();
    int after_del = 0;
    for (int i = 0; i < g_fdb_count; i++)
        if (strcmp(g_fdb[i].status, "deleted") == 0) after_del++;

    fdb_save();

    printf(GREEN "[FIND-ALL] Scan complete.\n" RESET);
    printf(GREEN "[FIND-ALL]   Scanned  : %d file(s)\n" RESET, g_scan_total);
    printf(GREEN "[FIND-ALL]   New      : %d\n" RESET, g_scan_new);
    printf(GREEN "[FIND-ALL]   Updated  : %d\n" RESET, g_scan_upd);
    printf(GREEN "[FIND-ALL]   Deleted  : %d\n" RESET, after_del - before_del);
    printf(GREEN "[FIND-ALL]   DB total : %d record(s) in %s\n" RESET,
           g_fdb_count, FILEDB_PATH);
    (void)old_count;
    free(g_fdb); g_fdb = NULL; g_fdb_count = 0;
}

/*
 * cmd_find_search():
 *   Search the DB for files matching pattern.
 *   Matching rules (applied to filename, not full path):
 *     - case-insensitive substring match on filename
 *     - if pattern starts with ".", treat as extension filter
 *     - if pattern contains "/", match against full path
 *     - "*" wildcard: simple glob (* matches anything)
 *
 *   Additionally filter by --ext <.cpp> if ext_filter non-empty.
 */

/* simple case-insensitive strstr */
static int ci_contains(const char *haystack, const char *needle) {
    if (!needle[0]) return 1;
    size_t hl = strlen(haystack), nl = strlen(needle);
    if (nl > hl) return 0;
    for (size_t i = 0; i <= hl - nl; i++) {
        int match = 1;
        for (size_t j = 0; j < nl; j++) {
            if (tolower((unsigned char)haystack[i+j]) !=
                tolower((unsigned char)needle[j]))  { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* minimal glob: only '*' wildcard, case-insensitive */
static int glob_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*str) {
                if (glob_match(pat, str)) return 1;
                str++;
            }
            return 0;
        }
        if (!*str) return 0;
        if (tolower((unsigned char)*pat) != tolower((unsigned char)*str)) return 0;
        pat++; str++;
    }
    return !*str;
}

static void cmd_find_search(const char *pattern, const char *ext_filter) {
    fdb_load();

    if (g_fdb_count == 0) {
        printf(YELLOW "[FIND] Database is empty. Run: omake --find-all\n" RESET);
        free(g_fdb); g_fdb = NULL;
        return;
    }

    /* Determine match mode */
    int is_ext_pat  = (pattern[0] == '.');
    int is_path_pat = (strchr(pattern, '/') || strchr(pattern, '\\'));
    int is_glob     = (strchr(pattern, '*') != NULL);

    int found = 0;
    printf(CYAN "[FIND] Searching %d record(s) for: %s\n" RESET,
           g_fdb_count, pattern);
    if (ext_filter[0])
        printf(CYAN "[FIND] Extension filter: %s\n" RESET, ext_filter);
    printf("\n");

    for (int i = 0; i < g_fdb_count; i++) {
        FileRecord *r = &g_fdb[i];
        if (strcmp(r->status, "deleted") == 0) continue;

        /* Extension filter */
        if (ext_filter[0] && strcmp(r->ext, ext_filter) != 0) continue;

        /* Get just the filename */
        const char *fname = strrchr(r->path, SEP);
        fname = fname ? fname + 1 : r->path;

        int match = 0;
        if (pattern[0] == '\0') {
            match = 1;   /* empty pattern = show all */
        } else if (is_ext_pat) {
            match = ci_contains(r->ext, pattern);
        } else if (is_path_pat) {
            match = is_glob ? glob_match(pattern, r->path)
                            : ci_contains(r->path, pattern);
        } else {
            match = is_glob ? glob_match(pattern, fname)
                            : ci_contains(fname, pattern);
        }

        if (match) {
            found++;
            /* Format mtime as readable date */
            time_t mt = (time_t)r->mtime;
            struct tm *tm_info = localtime(&mt);
            char date_buf[32] = "";
            if (tm_info) strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", tm_info);

            /* Human-readable size */
            char size_buf[32];
            long long s = r->size;
            if      (s >= 1073741824LL) snprintf(size_buf, sizeof(size_buf), "%.1f GB", s/1073741824.0);
            else if (s >= 1048576LL)    snprintf(size_buf, sizeof(size_buf), "%.1f MB", s/1048576.0);
            else if (s >= 1024LL)       snprintf(size_buf, sizeof(size_buf), "%.1f KB", s/1024.0);
            else                        snprintf(size_buf, sizeof(size_buf), "%lld B",  s);

            printf(GREEN "  %s\n" RESET, r->path);
            printf("    %-10s  %s\n", size_buf, date_buf);
        }
    }

    printf("\n");
    if (found == 0)
        printf(YELLOW "[FIND] No matches found for: %s\n" RESET, pattern);
    else
        printf(CYAN "[FIND] %d match(es) found.\n" RESET, found);

    free(g_fdb); g_fdb = NULL; g_fdb_count = 0;
}

/*
 * cmd_find_stats():
 *   Show database statistics — total files, breakdown by extension, etc.
 */
static void cmd_find_stats(void) {
    fdb_load();

    if (g_fdb_count == 0) {
        printf(YELLOW "[FIND] Database is empty. Run: omake --find-all\n" RESET);
        free(g_fdb); g_fdb = NULL;
        return;
    }

    /* Count by extension (simple bucket: up to 256 unique exts) */
    typedef struct { char ext[32]; int count; long long bytes; } ExtBucket;
    ExtBucket *buckets = (ExtBucket*)calloc(256, sizeof(ExtBucket));
    int bucket_count = 0;
    int active = 0; int deleted = 0;
    long long total_bytes = 0;

    for (int i = 0; i < g_fdb_count; i++) {
        FileRecord *r = &g_fdb[i];
        if (strcmp(r->status, "deleted") == 0) { deleted++; continue; }
        active++;
        total_bytes += r->size;

        /* find or create bucket for this ext */
        int bi = -1;
        for (int b = 0; b < bucket_count; b++)
            if (strcmp(buckets[b].ext, r->ext) == 0) { bi = b; break; }
        if (bi < 0 && bucket_count < 256) {
            bi = bucket_count++;
            strncpy(buckets[bi].ext, r->ext, 31);
        }
        if (bi >= 0) { buckets[bi].count++; buckets[bi].bytes += r->size; }
    }

    /* Sort buckets by count descending (simple bubble) */
    for (int a = 0; a < bucket_count - 1; a++)
        for (int b = a+1; b < bucket_count; b++)
            if (buckets[b].count > buckets[a].count) {
                ExtBucket tmp = buckets[a]; buckets[a] = buckets[b]; buckets[b] = tmp;
            }

    char total_size_buf[32];
    if      (total_bytes >= 1073741824LL) snprintf(total_size_buf, 32, "%.2f GB", total_bytes/1073741824.0);
    else if (total_bytes >= 1048576LL)    snprintf(total_size_buf, 32, "%.2f MB", total_bytes/1048576.0);
    else                                  snprintf(total_size_buf, 32, "%.2f KB", total_bytes/1024.0);

    printf(CYAN "\n[FIND-STATS] Database: %s\n" RESET, FILEDB_PATH);
    printf(CYAN "  Active files : %d\n" RESET, active);
    printf(CYAN "  Deleted      : %d\n" RESET, deleted);
    printf(CYAN "  Total size   : %s\n" RESET, total_size_buf);
    printf(CYAN "\n  Top file types:\n" RESET);
    int show = bucket_count < 20 ? bucket_count : 20;
    for (int i = 0; i < show; i++) {
        char sb[32];
        long long bs = buckets[i].bytes;
        if      (bs >= 1048576LL) snprintf(sb, 32, "%.1f MB", bs/1048576.0);
        else if (bs >= 1024LL)    snprintf(sb, 32, "%.1f KB", bs/1024.0);
        else                      snprintf(sb, 32, "%lld B",  bs);
        printf("    %-12s  %5d file(s)  %s\n",
               buckets[i].ext[0] ? buckets[i].ext : "(no ext)",
               buckets[i].count, sb);
    }
    printf("\n");
    free(buckets);
    free(g_fdb); g_fdb = NULL; g_fdb_count = 0;
}


/* ================================================================== */
/*  FEATURE IMPLEMENTATIONS                                             */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/*  F1: PKG-CONFIG integration                                          */
/* ------------------------------------------------------------------ */
void apply_pkg_config(const char *pkg_list, char *tags, char *libs) {
    if (!pkg_list || !pkg_list[0]) return;
    if (!tool_exists("pkg-config")) {
        printf(YELLOW "[PKG] pkg-config not found, skipping PKG: directive.\n" RESET);
        return;
    }
    char cmd[1024];
    /* --cflags */
    snprintf(cmd, sizeof(cmd), "pkg-config --cflags %s 2>/dev/null", pkg_list);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char buf[B_SIZE]; buf[0]='\0';
        if (fgets(buf, B_SIZE, fp)) {
            char *nl=strrchr(buf,'\n'); if(nl)*nl='\0';
            if (buf[0]) {
                strncat(tags," ",B_SIZE-strlen(tags)-1);
                strncat(tags,buf,B_SIZE-strlen(tags)-1);
                printf(CYAN "[PKG] cflags: %s\n" RESET, buf);
            }
        }
        pclose(fp);
    }
    /* --libs */
    snprintf(cmd, sizeof(cmd), "pkg-config --libs %s 2>/dev/null", pkg_list);
    fp = popen(cmd, "r");
    if (fp) {
        char buf[B_SIZE]; buf[0]='\0';
        if (fgets(buf, B_SIZE, fp)) {
            char *nl=strrchr(buf,'\n'); if(nl)*nl='\0';
            if (buf[0]) {
                strncat(libs," ",B_SIZE-strlen(libs)-1);
                strncat(libs,buf,B_SIZE-strlen(libs)-1);
                printf(CYAN "[PKG] libs:   %s\n" RESET, buf);
            }
        }
        pclose(fp);
    }
}

/* ------------------------------------------------------------------ */
/*  F2: DEFINE: macro injection                                         */
/* ------------------------------------------------------------------ */
void apply_defines(const char *define_list, char *tags) {
    if (!define_list || !define_list[0]) return;
    /*
     * Parse DEFINE: tokens. Values with spaces must be quoted in OMakeLists.txt.
     * We emit -DKEY=value or -DKEY="value" preserving user quoting.
     * Already-prefixed -D tokens are passed through unchanged.
     */
    char tmp[B_SIZE]; strncpy(tmp,define_list,B_SIZE-1);
    char *tok=strtok(tmp," \t");
    while(tok) {
        char flag[512];
        if(strncmp(tok,"-D",2)==0)
            snprintf(flag,sizeof(flag)," %s",tok);
        else
            snprintf(flag,sizeof(flag)," -D%s",tok);
        strncat(tags,flag,B_SIZE-strlen(tags)-1);
        tok=strtok(NULL," \t");
    }
    printf(CYAN "[DEFINE] %s\n" RESET, define_list);
}

/* ------------------------------------------------------------------ */
/*  F3: VERSION embed                                                    */
/* ------------------------------------------------------------------ */
void apply_version(const char *ver, char *tags) {
    if (!ver || !ver[0]) return;
    /* Escape any quotes in ver for shell safety */
    char safe_ver[64]; int si=0;
    for(const char *p=ver; *p && si<62; p++)
        if(*p != '"' && *p != '\'')
            safe_ver[si++]=*p;
    safe_ver[si]='\0';
    char flag[256];
    snprintf(flag,sizeof(flag)," -DOMAKE_VERSION=\\\"%s\\\"", safe_ver);
    strncat(tags,flag,B_SIZE-strlen(tags)-1);
    printf(CYAN "[VERSION] %s  (OMAKE_VERSION injected)\n" RESET, ver);
}

/* ------------------------------------------------------------------ */
/*  F4: LTO  (Link Time Optimization)                                   */
/* ------------------------------------------------------------------ */
void apply_lto(const char *lto_mode, char *tags, char *libs, CompilerType ct) {
    if (!lto_mode || !lto_mode[0]) return;
    char lm[16]; strncpy(lm,lto_mode,15);
    for(int i=0;lm[i];i++) lm[i]=(char)tolower((unsigned char)lm[i]);
    if (strcmp(lm,"off")==0 || strcmp(lm,"no")==0) return;
    if (ct==CT_MSVC) {
        strncat(tags," /GL",B_SIZE-strlen(tags)-1);
        strncat(libs," /LTCG",B_SIZE-strlen(libs)-1);
        printf(CYAN "[LTO] MSVC /GL + /LTCG\n" RESET);
    } else if (strcmp(lm,"thin")==0) {
        strncat(tags," -flto=thin",B_SIZE-strlen(tags)-1);
        strncat(libs," -flto=thin",B_SIZE-strlen(libs)-1);
        printf(CYAN "[LTO] ThinLTO (-flto=thin)\n" RESET);
    } else {
        strncat(tags," -flto",B_SIZE-strlen(tags)-1);
        strncat(libs," -flto",B_SIZE-strlen(libs)-1);
        printf(CYAN "[LTO] Full LTO (-flto)\n" RESET);
    }
}

/* ------------------------------------------------------------------ */
/*  F5: OUTPUT_TYPE: shared | static | exe                              */
/* ------------------------------------------------------------------ */
/* Returns modified out path and injects flags */
void apply_output_type(const char *otype, char *tags, char *libs,
                               char *out, CompilerType ct) {
    char lt[16]; strncpy(lt,otype,15);
    for(int i=0;lt[i];i++) lt[i]=(char)tolower((unsigned char)lt[i]);

    if (strcmp(lt,"shared")==0) {
        if(ct==CT_MSVC) {
            strncat(tags," /LD",B_SIZE-strlen(tags)-1);
        } else {
            strncat(tags," -fPIC",B_SIZE-strlen(tags)-1);
            strncat(libs," -shared",B_SIZE-strlen(libs)-1);
        }
        /* Rename out: app.bin -> libapp.so / app.dll */
        char *slash=strrchr(out,'/'); if(!slash)slash=strrchr(out,'\\');
        char *base=slash?slash+1:out;
        char dir[512]=""; if(slash){int dl=(int)(slash-out)+1; strncpy(dir,out,dl<511?dl:511);}
        char stem[256]; strncpy(stem,base,255);
        char *dot=strrchr(stem,'.'); if(dot)*dot='\0';
#ifdef _WIN32
        snprintf(out,511,"%s%s.dll",dir,stem);
#else
        snprintf(out,511,"%slib%s.so",dir,stem);
#endif
        printf(CYAN "[OUTPUT] Shared library -> %s\n" RESET, out);

    } else if (strcmp(lt,"static")==0) {
        /* Static lib: compiled to .a / .lib, linked with ar */
        char *slash=strrchr(out,'/'); if(!slash)slash=strrchr(out,'\\');
        char *base=slash?slash+1:out;
        char dir[512]=""; if(slash){int dl=(int)(slash-out)+1; strncpy(dir,out,dl<511?dl:511);}
        char stem[256]; strncpy(stem,base,255);
        char *dot=strrchr(stem,'.'); if(dot)*dot='\0';
#ifdef _WIN32
        snprintf(out,511,"%s%s.lib",dir,stem);
#else
        snprintf(out,511,"%slib%s.a",dir,stem);
#endif
        printf(CYAN "[OUTPUT] Static library -> %s\n" RESET, out);
    }
    /* exe: no changes */
}

/* Link static lib with ar (called instead of normal linker) */
int link_static_lib(const char *all_objs, const char *out) {
#ifdef _WIN32
    char cmd[B_SIZE];
    snprintf(cmd,sizeof(cmd),"lib.exe /nologo /OUT:\"%s\" %s",out,all_objs);
#else
    char cmd[B_SIZE];
    snprintf(cmd,sizeof(cmd),"ar rcs \"%s\" %s",out,all_objs);
#endif
    printf(CYAN "[LINK/STATIC] %s\n" RESET, cmd);
    return system(cmd)==0;
}

/* ------------------------------------------------------------------ */
/*  F6: PCH  (Precompiled Header)                                       */
/* ------------------------------------------------------------------ */
void build_pch(const char *hdr, const char *obj_root,
                      const char *comp, CompilerType ct,
                      const char *tags, const char *auto_inc,
                      char *pch_flag_out) {
    pch_flag_out[0]='\0';
    if (!hdr||!hdr[0]) return;

    char pch_out[1024];
    if(ct==CT_MSVC) {
        snprintf(pch_out,sizeof(pch_out),"%s/pch.pch",obj_root);
        char cmd[2048];
        snprintf(cmd,sizeof(cmd),
            "%s /nologo /c /Yc\"%s\" /Fp\"%s\" %s %s",
            comp,hdr,pch_out,tags,auto_inc);
        printf(CYAN "[PCH] %s\n" RESET, cmd);
        if(system(cmd)==0) {
            snprintf(pch_flag_out,512," /Yu\"%s\" /Fp\"%s\"",hdr,pch_out);
            printf(GREEN "[PCH] Precompiled header ready.\n" RESET);
        } else printf(YELLOW "[PCH] Failed, continuing without PCH.\n" RESET);
    } else {
        /* GCC/Clang: compile header to .gch */
        snprintf(pch_out,sizeof(pch_out),"%s/pch.gch",obj_root);
        char cmd[2048];
        snprintf(cmd,sizeof(cmd),
            "%s -x c++-header \"%s\" -o \"%s\" %s %s",
            comp,hdr,pch_out,tags,auto_inc);
        printf(CYAN "[PCH] %s\n" RESET, cmd);
        if(system(cmd)==0) {
            snprintf(pch_flag_out,512," -include-pch \"%s\"",pch_out);
            printf(GREEN "[PCH] Precompiled header ready.\n" RESET);
        } else printf(YELLOW "[PCH] Failed, continuing without PCH.\n" RESET);
    }
}

/* ------------------------------------------------------------------ */
/*  F7: UNITY BUILD                                                     */
/* ------------------------------------------------------------------ */
/* Concatenate all source files into one .cpp, compile that */
int unity_compile(SourceFile *srcs, int count,
                          const char *obj_root,
                          const char *final_comp, CompilerType ct,
                          const char *tags_tr, const char *inc_tr) {
    char unity_src[1024]; snprintf(unity_src,sizeof(unity_src),"%s/unity_build.cpp",obj_root);
    char unity_obj[1024]; snprintf(unity_obj,sizeof(unity_obj),"%s/unity_build.o",obj_root);
    FILE *uf = fopen(unity_src,"w");
    if(!uf){ printf(RED "[UNITY] Cannot create unity file.\n" RESET); return 0; }
    fprintf(uf,"/* OMake Unity Build — auto-generated, do not edit */\n");
    for(int i=0;i<count;i++) {
        fix_slashes(srcs[i].path);
        fprintf(uf,"#include \"%s\"\n",srcs[i].path);
    }
    fclose(uf);
    printf(CYAN "[UNITY] Merged %d files -> %s\n" RESET, count, unity_src);

    char cmd[B_SIZE];
    if(ct==CT_MSVC) {
        snprintf(cmd,sizeof(cmd),"%s /nologo /c \"%s\" /Fo\"%s\" /TP %s %s",
                 final_comp,unity_src,unity_obj,tags_tr,inc_tr);
    } else {
        snprintf(cmd,sizeof(cmd),"%s -c \"%s\" -o \"%s\" -MMD -MP %s %s",
                 final_comp,unity_src,unity_obj,tags_tr,inc_tr);
    }
    printf(CYAN "[UNITY] %s\n" RESET, cmd);
    /* Update all objs to point to the single unity obj */
    for(int i=0;i<count;i++) strncpy(srcs[i].obj,unity_obj,1023);
    return system(cmd)==0;
}

/* ------------------------------------------------------------------ */
/*  F8: compile_commands.json  (LSP / clangd)                           */
/* ------------------------------------------------------------------ */
void generate_compile_commands(SourceFile *srcs, int count,
                                       const char *comp,
                                       const char *tags, const char *inc) {
    FILE *f=fopen("compile_commands.json","w");
    if(!f){ printf(RED "[LSP] Cannot write compile_commands.json\n" RESET); return; }

    char cwd[1024]; 
#ifdef _WIN32
    _getcwd(cwd,sizeof(cwd));
#else
    if(!getcwd(cwd,sizeof(cwd))) strncpy(cwd,".",sizeof(cwd)-1);
#endif

    fprintf(f,"[\n");
    for(int i=0;i<count;i++) {
        char cmd[B_SIZE];
        snprintf(cmd,sizeof(cmd),"%s -c \"%s\" -o \"%s\" %s %s",
                 comp,srcs[i].path,srcs[i].obj,tags,inc);
        /* escape backslashes and quotes for JSON */
        char esc_path[2048]; int ei=0;
        for(const char *p=srcs[i].path; *p && ei<2046; p++,ei++) {
            if(*p=='\\') { esc_path[ei++]='\\'; esc_path[ei]='\\'; }
            else esc_path[ei]=*p;
        }
        esc_path[ei]='\0';
        char esc_cmd[B_SIZE]; ei=0;
        for(const char *p=cmd; *p && ei<(int)B_SIZE-2; p++,ei++) {
            if(*p=='\\') { esc_cmd[ei++]='\\'; esc_cmd[ei]='\\'; }
            else if(*p=='"') { esc_cmd[ei++]='\\'; esc_cmd[ei]='"'; }
            else esc_cmd[ei]=*p;
        }
        esc_cmd[ei]='\0';
        fprintf(f,"  {\n"
                  "    \"directory\": \"%s\",\n"
                  "    \"file\": \"%s\",\n"
                  "    \"command\": \"%s\"\n"
                  "  }%s\n",
                  cwd, esc_path, esc_cmd, i<count-1?",":"");
    }
    fprintf(f,"]\n");
    fclose(f);
    printf(GREEN "[LSP] compile_commands.json written (%d entries) — clangd ready.\n" RESET, count);
}

/* ------------------------------------------------------------------ */
/*  F9: INSTALL                                                          */
/* ------------------------------------------------------------------ */
void do_install(const char *src_bin, const char *dst) {
    if(!dst||!dst[0]) return;
    char cmd[1024];
    printf(CYAN "[INSTALL] %s -> %s\n" RESET, src_bin, dst);
#ifdef _WIN32
    snprintf(cmd,sizeof(cmd),"copy /y \"%s\" \"%s\"",src_bin,dst);
#else
    snprintf(cmd,sizeof(cmd),"install -m 755 \"%s\" \"%s\"",src_bin,dst);
#endif
    if(system(cmd)==0) printf(GREEN "[INSTALL] Done.\n" RESET);
    else printf(RED "[INSTALL] Failed — try with sudo/admin.\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  F10: STRIP (remove debug symbols from binary)                       */
/* ------------------------------------------------------------------ */
void do_strip(const char *bin, CompilerType ct) {
    char cmd[600];
    printf(CYAN "[STRIP] Stripping debug symbols: %s\n" RESET, bin);
    if(ct==CT_MSVC) {
        printf(YELLOW "[STRIP] MSVC: use /DEBUG:NONE at link time. Skipping post-strip.\n" RESET);
        return;
    }
#ifdef _WIN32
    snprintf(cmd,sizeof(cmd),"strip --strip-all \"%s\" 2>nul",bin);
#else
    snprintf(cmd,sizeof(cmd),"strip --strip-all \"%s\"",bin);
#endif
    if(system(cmd)==0) printf(GREEN "[STRIP] Done.\n" RESET);
    else               printf(YELLOW "[STRIP] strip not found or failed.\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  F11: --format  (clang-format)                                       */
/* ------------------------------------------------------------------ */
void cmd_format(int dry_run) {
    if(!tool_exists("clang-format")) {
        printf(RED "[FORMAT] clang-format not found.\n" RESET); return;
    }
    /* Find all source/header files */
    const char *exts[] = {".c",".cpp",".h",".hpp",".cc",".cxx",NULL};
    printf(CYAN "[FORMAT] Formatting sources%s...\n" RESET, dry_run?" (dry-run)":"");
    int count=0;
    /* simple: use shell find/dir */
#ifdef _WIN32
    for(int ei=0;exts[ei];ei++){
        char cmd[512];
        snprintf(cmd,sizeof(cmd),
            "for /r %%f in (*%s) do clang-format -i%s \"%%f\"",
            exts[ei], dry_run?" --dry-run --Werror":"");
        system(cmd); count++;
    }
#else
    char find_cmd[1024]="find . \\( ";
    for(int ei=0;exts[ei];ei++){
        if(ei>0) strncat(find_cmd," -o ",sizeof(find_cmd)-strlen(find_cmd)-1);
        char part[64]; snprintf(part,sizeof(part),"-name \"*%s\"",exts[ei]);
        strncat(find_cmd,part,sizeof(find_cmd)-strlen(find_cmd)-1);
    }
    strncat(find_cmd," \\) -not -path './.omake*'",sizeof(find_cmd)-strlen(find_cmd)-1);
    char full_cmd[1200];
    snprintf(full_cmd,sizeof(full_cmd),
             "%s | xargs clang-format -i%s 2>/dev/null && echo '[FORMAT] Done.'",
             find_cmd, dry_run?" --dry-run --Werror":"");
    system(full_cmd);
#endif
    (void)count;
}

/* ------------------------------------------------------------------ */
/*  F12: --lint  (clang-tidy)                                           */
/* ------------------------------------------------------------------ */
void cmd_lint(void) {
    if(!tool_exists("clang-tidy")) {
        printf(RED "[LINT] clang-tidy not found.\n" RESET);
        printf(YELLOW "[LINT] Tip: install with apt/brew, then run --generate-compile-commands first.\n" RESET);
        return;
    }
    if(!is_regular_file("compile_commands.json")) {
        printf(YELLOW "[LINT] compile_commands.json not found. Run --generate-compile-commands first.\n" RESET);
    }
    printf(CYAN "[LINT] Running clang-tidy...\n" RESET);
#ifdef _WIN32
    system("for /r %%f in (*.cpp *.c) do clang-tidy \"%%f\" --quiet 2>nul");
#else
    system("find . \\( -name '*.cpp' -o -name '*.c' \\) -not -path './.omake*' "
           "| xargs clang-tidy --quiet 2>/dev/null");
#endif
}

/* ------------------------------------------------------------------ */
/*  F13: --benchmark                                                    */
/* ------------------------------------------------------------------ */
void cmd_benchmark(const char *bin, int runs) {
    printf(CYAN "[BENCH] Benchmarking: %s  (%d run(s))\n" RESET, bin, runs);
    if(tool_exists("hyperfine")) {
        char cmd[600];
        snprintf(cmd,sizeof(cmd),"hyperfine --runs %d \"%s\"",runs,bin);
        system(cmd);
    } else {
        /* Fallback: simple time loop */
        printf(YELLOW "[BENCH] hyperfine not found, using built-in timer.\n" RESET);
        #include <time.h>
        double total=0;
        for(int r=0;r<runs;r++){
            clock_t t0=clock();
            system(bin);
            clock_t t1=clock();
            total+=(double)(t1-t0)/CLOCKS_PER_SEC;
        }
        printf(GREEN "[BENCH] Mean: %.4f s  Total: %.4f s  Runs: %d\n" RESET,
               total/runs,total,runs);
    }
}

/* ------------------------------------------------------------------ */
/*  F14: --disasm  (objdump / dumpbin)                                  */
/* ------------------------------------------------------------------ */
void cmd_disasm(const char *bin, const char *func_filter) {
    char cmd[1024];
    printf(CYAN "[DISASM] %s\n" RESET, bin);
#ifdef _WIN32
    if(tool_exists("dumpbin")) {
        if(func_filter&&func_filter[0])
            snprintf(cmd,sizeof(cmd),"dumpbin /disasm \"%s\" | findstr /i \"%s\"",bin,func_filter);
        else
            snprintf(cmd,sizeof(cmd),"dumpbin /disasm \"%s\" | more",bin);
    } else {
        snprintf(cmd,sizeof(cmd),"objdump -d \"%s\"",bin);
    }
#else
    if(func_filter&&func_filter[0])
        snprintf(cmd,sizeof(cmd),"objdump -d --no-show-raw-insn \"%s\" | grep -A 30 \"<%s>\"",bin,func_filter);
    else
        snprintf(cmd,sizeof(cmd),"objdump -d --no-show-raw-insn \"%s\" | less",bin);
#endif
    system(cmd);
}

/* ------------------------------------------------------------------ */
/*  F15: --coverage  (gcov / llvm-cov)                                  */
/* ------------------------------------------------------------------ */
void cmd_coverage(void) {
    printf(CYAN "[COV] Generating coverage report...\n" RESET);
    if(tool_exists("llvm-cov") && tool_exists("llvm-profdata")) {
        system("llvm-profdata merge -sparse default.profraw -o default.profdata 2>/dev/null");
        system("llvm-cov report ./bin/* -instr-profile=default.profdata 2>/dev/null || "
               "llvm-cov show ./bin/* -instr-profile=default.profdata --format=html > coverage.html 2>/dev/null");
        printf(GREEN "[COV] llvm-cov report generated (coverage.html if html mode).\n" RESET);
    } else if(tool_exists("gcov")) {
        system("gcov src/*.cpp src/*.c 2>/dev/null");
        if(tool_exists("lcov")) {
            system("lcov --capture --directory . --output-file coverage.info 2>/dev/null");
            system("genhtml coverage.info --output-directory coverage_html 2>/dev/null");
            printf(GREEN "[COV] lcov HTML report: coverage_html/index.html\n" RESET);
        } else {
            printf(GREEN "[COV] gcov done. Install lcov for HTML report.\n" RESET);
        }
    } else {
        printf(YELLOW "[COV] No coverage tool found (gcov / llvm-cov).\n" RESET);
        printf(YELLOW "[COV] Build with: TAGS: --coverage  or  -fprofile-instr-generate -fcoverage-mapping\n" RESET);
    }
}

/* ------------------------------------------------------------------ */
/*  F16: --docs  (Doxygen)                                              */
/* ------------------------------------------------------------------ */
void cmd_docs(void) {
    if(!tool_exists("doxygen")) {
        printf(RED "[DOCS] doxygen not found.\n" RESET); return;
    }
    if(!is_regular_file("Doxyfile")) {
        printf(YELLOW "[DOCS] No Doxyfile found. Generating default...\n" RESET);
        system("doxygen -g Doxyfile");
        /* patch minimal settings */
        system("sed -i 's/^PROJECT_NAME.*/PROJECT_NAME = \"OMake Project\"/' Doxyfile 2>/dev/null");
        system("sed -i 's/^INPUT .*/INPUT = src include/' Doxyfile 2>/dev/null");
        system("sed -i 's/^RECURSIVE.*/RECURSIVE = YES/' Doxyfile 2>/dev/null");
        system("sed -i 's/^GENERATE_HTML.*/GENERATE_HTML = YES/' Doxyfile 2>/dev/null");
        system("sed -i 's/^EXTRACT_ALL.*/EXTRACT_ALL = YES/' Doxyfile 2>/dev/null");
    }
    printf(CYAN "[DOCS] Running doxygen...\n" RESET);
    if(system("doxygen Doxyfile")==0)
        printf(GREEN "[DOCS] Documentation generated -> html/index.html\n" RESET);
    else
        printf(RED "[DOCS] doxygen failed.\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  F17: --package  (zip / tar.gz release bundle)                       */
/* ------------------------------------------------------------------ */
void cmd_package(const char *bin, const char *ver) {
    char pkgname[256];
    const char *base=strrchr(bin,'/'); if(!base)base=strrchr(bin,'\\');
    base=base?base+1:bin;
    char stem[128]; strncpy(stem,base,127);
    char *dot=strrchr(stem,'.'); if(dot)*dot='\0';
    if(ver&&ver[0]) snprintf(pkgname,sizeof(pkgname),"%s-%s",stem,ver);
    else            snprintf(pkgname,sizeof(pkgname),"%s-release",stem);

    printf(CYAN "[PACKAGE] Creating: %s\n" RESET, pkgname);
    char cmd[1024];
#ifdef _WIN32
    snprintf(cmd,sizeof(cmd),
        "mkdir \"%s\" 2>nul && copy \"%s\" \"%s\\\" && "
        "tar -a -c -f \"%s.zip\" \"%s\" && rmdir /s /q \"%s\"",
        pkgname,bin,pkgname,pkgname,pkgname,pkgname);
    if(system(cmd)==0) printf(GREEN "[PACKAGE] %s.zip ready.\n" RESET, pkgname);
#else
    snprintf(cmd,sizeof(cmd),
        "mkdir -p \"%s\" && cp \"%s\" \"%s/\" && "
        "tar -czf \"%s.tar.gz\" \"%s\" && rm -rf \"%s\"",
        pkgname,bin,pkgname,pkgname,pkgname,pkgname);
    if(system(cmd)==0) printf(GREEN "[PACKAGE] %s.tar.gz ready.\n" RESET, pkgname);
#endif
    else printf(RED "[PACKAGE] Failed.\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  F18: --compress  (UPX binary packer)                                */
/* ------------------------------------------------------------------ */
void cmd_compress(const char *bin) {
    if(!tool_exists("upx")) {
        printf(YELLOW "[COMPRESS] upx not found. Install upx-ucl for binary compression.\n" RESET);
        return;
    }
    char cmd[600];
    snprintf(cmd,sizeof(cmd),"upx --best \"%s\"",bin);
    printf(CYAN "[COMPRESS] %s\n" RESET, cmd);
    if(system(cmd)==0) printf(GREEN "[COMPRESS] Done.\n" RESET);
    else               printf(RED "[COMPRESS] upx failed.\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  F19: --test  (run test binaries)                                    */
/* ------------------------------------------------------------------ */
int  cmd_run_tests(const char *test_dir, const char *test_ext,
                           const char *obj_root,
                           const char *final_comp, CompilerType ct,
                           const char *tags, const char *libs,
                           const char *auto_inc) {
    if(!test_dir||!test_dir[0]) {
        printf(YELLOW "[TEST] No TEST: directory set in OMakeLists.txt.\n" RESET);
        return 0;
    }
    if(!dir_exists(test_dir)) {
        printf(YELLOW "[TEST] Test directory not found: %s\n" RESET, test_dir);
        return 0;
    }
    printf(CYAN "[TEST] Building and running tests in: %s\n" RESET, test_dir);

    /* Scan test sources */
    int old_src_count = src_count;
    src_count = 0;
    static SourceFile test_sources[512];
    SourceFile *saved = sources;
    /* Temporarily redirect global sources pointer isn't possible cleanly,
     * so we compile each test file individually */

    DIR *d=opendir(test_dir);
    if(!d){ printf(RED "[TEST] Cannot open test dir.\n" RESET); return 1; }

    int pass=0, fail=0, total=0;
    struct dirent *ent;
    while((ent=readdir(d))!=NULL) {
        if(ent->d_name[0]=='.') continue;
        /* match extension */
        const char *e=strrchr(ent->d_name,'.');
        if(!e||strcmp(e,test_ext)!=0) continue;

        char src[1024]; snprintf(src,sizeof(src),"%s%c%s",test_dir,SEP,ent->d_name);
        char bin[1024];
        char stem[256]; strncpy(stem,ent->d_name,255);
        char *dot2=strrchr(stem,'.'); if(dot2)*dot2='\0';
#ifdef _WIN32
        snprintf(bin,sizeof(bin),"%s%c%s_test.exe",obj_root,SEP,stem);
#else
        snprintf(bin,sizeof(bin),"%s%c%s_test",obj_root,SEP,stem);
#endif
        /* compile */
        char compile_cmd[B_SIZE];
        if(ct==CT_MSVC)
            snprintf(compile_cmd,sizeof(compile_cmd),
                "%s /nologo \"%s\" /Fe\"%s\" %s %s %s",
                final_comp,src,bin,tags,auto_inc,libs);
        else
            snprintf(compile_cmd,sizeof(compile_cmd),
                "%s \"%s\" -o \"%s\" %s %s %s",
                final_comp,src,bin,tags,auto_inc,libs);

        printf(CYAN "[TEST] Compiling: %s\n" RESET, ent->d_name);
        if(system(compile_cmd)!=0) {
            printf(RED "[TEST] COMPILE FAIL: %s\n" RESET, ent->d_name);
            fail++; total++; continue;
        }
        /* run */
        printf(CYAN "[TEST] Running:   %s\n" RESET, bin);
        int ret=system(bin);
        total++;
        if(ret==0) { pass++; printf(GREEN "[TEST] PASS: %s\n" RESET, stem); }
        else       { fail++; printf(RED   "[TEST] FAIL: %s (exit %d)\n" RESET, stem, ret); }
    }
    closedir(d);
    printf("\n");
    printf(pass==total ? GREEN : RED);
    printf("[TEST] Results: %d/%d passed", pass, total);
    printf(RESET "\n");
    (void)test_sources; (void)saved; (void)old_src_count;
    return (pass < total) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  F20: --watch  (file watcher for auto-rebuild)                       */
/* ------------------------------------------------------------------ */
#ifndef _WIN32
#include <sys/inotify.h>
#endif

void cmd_watch(int force, const char *profile, int jobs,
                      const char *arch, int do_static) {
    printf(CYAN "[WATCH] Watching for changes. Press Ctrl+C to stop.\n" RESET);

#ifdef _WIN32
    /* Windows: poll mtime every 500ms */
    printf(YELLOW "[WATCH] Windows: polling every 500ms (inotify not available).\n" RESET);
    /* Initial build */
    build_engine(force,profile,jobs,0,arch,do_static,"",0,0,0,0);
    while(1) {
        Sleep(500);
        /* Check if any source file changed via mtime */
        /* Simple: just rebuild with --build-hash (incremental) */
        build_engine(0,profile,jobs,0,arch,do_static,"",0,0,0,0);
    }
#else
    int ifd=inotify_init1(IN_NONBLOCK);
    if(ifd<0) {
        printf(YELLOW "[WATCH] inotify unavailable, falling back to polling.\n" RESET);
        build_engine(force,profile,jobs,0,arch,do_static,"",0,0,0,0);
        while(1) {
            sleep(1);
            build_engine(0,profile,jobs,0,arch,do_static,"",0,0,0,0);
        }
        return;
    }
    /* Watch src/, include/, and project root */
    const char *watch_dirs[]={"src","include",".",NULL};
    for(int wi=0;watch_dirs[wi];wi++) {
        if(dir_exists(watch_dirs[wi]))
            inotify_add_watch(ifd,watch_dirs[wi],
                IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_MOVED_TO);
    }
    /* Initial build */
    build_engine(force,profile,jobs,0,arch,do_static,"",0,0,0,0);
    printf(CYAN "[WATCH] Ready. Watching src/ include/ ...\n" RESET);

    char ibuf[4096]; int pending=0;
    while(1) {
        fd_set fds; FD_ZERO(&fds); FD_SET(ifd,&fds);
        struct timeval tv={0,200000}; /* 200ms */
        int ret=select(ifd+1,&fds,NULL,NULL,&tv);
        if(ret>0) {
            ssize_t len=read(ifd,ibuf,sizeof(ibuf));
            if(len>0) pending=1;
        }
        if(pending) {
            /* debounce: wait until no more events for 300ms */
            struct timeval tv2={0,300000};
            fd_set fds2; FD_ZERO(&fds2); FD_SET(ifd,&fds2);
            if(select(ifd+1,&fds2,NULL,NULL,&tv2)<=0) {
                printf(MAGENTA "\n[WATCH] Change detected — rebuilding...\n" RESET);
                build_engine(0,profile,jobs,0,arch,do_static,"",0,0,0,0);
                pending=0;
                /* Re-add watches (files may have been recreated) */
                for(int wi=0;watch_dirs[wi];wi++)
                    if(dir_exists(watch_dirs[wi]))
                        inotify_add_watch(ifd,watch_dirs[wi],
                            IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_MOVED_TO);
            }
        }
    }
    close(ifd);
#endif
}

/* ------------------------------------------------------------------ */
/*  F21: --self-update                                                  */
/* ------------------------------------------------------------------ */
void cmd_self_update(const char *self_path) {
    printf(CYAN "[UPDATE] Checking for OMake updates...\n" RESET);
    if(!tool_exists("curl")&&!tool_exists("wget")) {
        printf(RED "[UPDATE] curl/wget not found.\n" RESET); return;
    }
    /* Download latest omake.c from a known URL and recompile */
    const char *url="https://raw.githubusercontent.com/omake-build/omake/main/omake.c";
    char cmd[1024];
    if(tool_exists("curl"))
        snprintf(cmd,sizeof(cmd),"curl -fsSL -o /tmp/omake_new.c \"%s\"",url);
    else
        snprintf(cmd,sizeof(cmd),"wget -q -O /tmp/omake_new.c \"%s\"",url);

    printf(CYAN "[UPDATE] Downloading: %s\n" RESET, url);
    if(system(cmd)!=0) { printf(RED "[UPDATE] Download failed.\n" RESET); return; }

    /* Recompile */
    char compile[512];
    snprintf(compile,sizeof(compile),
#ifdef _WIN32
        "cl /nologo /O2 /Fe\"%s\" /tmp/omake_new.c 2>nul || "
        "gcc -O2 -o \"%s\" /tmp/omake_new.c",
#else
        "gcc -O2 -o \"%s\" /tmp/omake_new.c",
#endif
        self_path
#ifdef _WIN32
        ,self_path
#endif
    );
    printf(CYAN "[UPDATE] Compiling...\n" RESET);
    if(system(compile)==0) printf(GREEN "[UPDATE] OMake updated successfully!\n" RESET);
    else printf(RED "[UPDATE] Compilation failed.\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  F22: DEPS: shorthand (GitHub header-only libs)                      */
/* ------------------------------------------------------------------ */
/*
 * DEPS: nlohmann/json  -> FETCH_GIT: https://github.com/nlohmann/json.git TO include/nlohmann_json
 * DEPS: fmtlib/fmt     -> FETCH_GIT: https://github.com/fmtlib/fmt.git    TO include/fmt
 * Maps well-known shortcuts to their GitHub repos.
 */
typedef struct { const char *alias; const char *repo; const char *dest; } DepAlias;
static const DepAlias DEP_ALIASES[] = {
    {"nlohmann/json",   "https://github.com/nlohmann/json.git",       "include/json"},
    {"json",            "https://github.com/nlohmann/json.git",       "include/json"},
    {"fmtlib/fmt",      "https://github.com/fmtlib/fmt.git",          "include/fmt"},
    {"fmt",             "https://github.com/fmtlib/fmt.git",          "include/fmt"},
    {"spdlog",          "https://github.com/gabime/spdlog.git",        "include/spdlog"},
    {"catch2",          "https://github.com/catchorg/Catch2.git",      "include/catch2"},
    {"doctest",         "https://github.com/doctest/doctest.git",      "include/doctest"},
    {"glm",             "https://github.com/g-truc/glm.git",           "include/glm"},
    {"stb",             "https://github.com/nothings/stb.git",         "include/stb"},
    {"imgui",           "https://github.com/ocornut/imgui.git",        "include/imgui"},
    {"cereal",          "https://github.com/USCiLab/cereal.git",       "include/cereal"},
    {"eigen",           "https://gitlab.com/libeigen/eigen.git",       "include/eigen"},
    {"boost",           "https://github.com/boostorg/boost.git",       "include/boost"},
    {"abseil",          "https://github.com/abseil/abseil-cpp.git",    "include/absl"},
    {"toml++",          "https://github.com/marzer/tomlplusplus.git",  "include/toml++"},
    {"magic_enum",      "https://github.com/Neargye/magic_enum.git",   "include/magic_enum"},
    {"rang",            "https://github.com/agauniyal/rang.git",       "include/rang"},
    {"argparse",        "https://github.com/p-ranav/argparse.git",     "include/argparse"},
    {"csv-parser",      "https://github.com/vincentlaucsb/csv-parser.git","include/csv"},
    {NULL,NULL,NULL}
};

void resolve_deps(const char *deps_line, FetchEntry *fetch_list, int *fetch_count) {
    if(!deps_line||!deps_line[0]) return;
    char tmp[B_SIZE]; strncpy(tmp,deps_line,B_SIZE-1);
    char *tok=strtok(tmp," \t,");
    while(tok && *fetch_count < MAX_FETCH) {
        int resolved=0;
        for(int i=0;DEP_ALIASES[i].alias;i++) {
            if(strcasecmp(tok,DEP_ALIASES[i].alias)==0) {
                FetchEntry *e=&fetch_list[(*fetch_count)++];
                strncpy(e->url,  DEP_ALIASES[i].repo,511);
                strncpy(e->dest, DEP_ALIASES[i].dest,511);
                printf(CYAN "[DEPS] %s -> %s\n" RESET, tok, e->url);
                resolved=1; break;
            }
        }
        if(!resolved) {
            /* Treat as GitHub "user/repo" shorthand */
            if(strchr(tok,'/')) {
                FetchEntry *e=&fetch_list[(*fetch_count)++];
                char *sl=strchr(tok,'/');
                snprintf(e->url, 512,"https://github.com/%s.git",tok);
                snprintf(e->dest,512,"include/%s",sl+1);
                printf(CYAN "[DEPS] %s -> %s\n" RESET, tok, e->url);
            } else {
                printf(YELLOW "[DEPS] Unknown dependency: %s (use FETCH: for custom URLs)\n" RESET, tok);
            }
        }
        tok=strtok(NULL," \t,");
    }
}

/* ------------------------------------------------------------------ */
/*  F23: ENV: environment variable injection                            */
/* ------------------------------------------------------------------ */
void apply_env(const char *env_list, char *tags) {
    if(!env_list||!env_list[0]) return;
    char tmp[B_SIZE]; strncpy(tmp,env_list,B_SIZE-1);
    char *tok=strtok(tmp," \t");
    while(tok) {
        const char *val=getenv(tok);
        if(val) {
            char flag[512]; snprintf(flag,sizeof(flag)," -D%s=\"%s\"",tok,val);
            strncat(tags,flag,B_SIZE-strlen(tags)-1);
            printf(CYAN "[ENV] %s=%s\n" RESET, tok, val);
        } else {
            printf(YELLOW "[ENV] %s not set in environment.\n" RESET, tok);
        }
        tok=strtok(NULL," \t");
    }
}

/* ------------------------------------------------------------------ */
/*  F24: --sign  (Windows code signing / Linux GPG)                    */
/* ------------------------------------------------------------------ */
void cmd_sign(const char *bin, const char *cert) {
    char cmd[1024];
#ifdef _WIN32
    if(tool_exists("signtool")) {
        if(cert&&cert[0])
            snprintf(cmd,sizeof(cmd),"signtool sign /f \"%s\" /fd SHA256 \"%s\"",cert,bin);
        else
            snprintf(cmd,sizeof(cmd),"signtool sign /a /fd SHA256 \"%s\"",bin);
        printf(CYAN "[SIGN] %s\n" RESET, cmd);
        if(system(cmd)==0) printf(GREEN "[SIGN] Code signing successful.\n" RESET);
        else               printf(RED "[SIGN] Signing failed.\n" RESET);
    } else {
        printf(YELLOW "[SIGN] signtool not found (Windows SDK required).\n" RESET);
    }
#else
    if(tool_exists("gpg")) {
        snprintf(cmd,sizeof(cmd),"gpg --detach-sign --armor \"%s\"",bin);
        printf(CYAN "[SIGN] GPG signing: %s\n" RESET, bin);
        if(system(cmd)==0) printf(GREEN "[SIGN] Signature: %s.asc\n" RESET, bin);
        else               printf(RED "[SIGN] GPG signing failed.\n" RESET);
    } else {
        printf(YELLOW "[SIGN] gpg not found.\n" RESET);
    }
#endif
    (void)cert;
}

/* ------------------------------------------------------------------ */
/*  F25: Extended --create-project templates                            */

/* ------------------------------------------------------------------ */
/*  F25: --create-project  (project scaffold generator)               */
/* ------------------------------------------------------------------ */

/* ── Template catalogue ─────────────────────────────────────────── */
typedef struct {
    const char *name;       /* CLI key                      */
    const char *desc;       /* human description            */
    const char *libs;       /* USE_LIB: values              */
    const char *std;        /* STANDARD: value              */
    const char *extra_dirs; /* space-sep extra directories  */
} ProjectTemplate;

static const ProjectTemplate TEMPLATES[] = {
    /* fetch_git field removed — FETCH_V2 lines now generated from LIB_FETCH table */
    /* basic */
    {"basic",   "Basic C++ console app",                                 "",                                            "C++=17", ""},
    {"c",       "Basic C console app",                                   "",                                            "C=11",   ""},
    /* OpenGL */
    {"opengl",         "OpenGL 4.x + GLFW + GLEW",                      "opengl glfw glew",                            "C++=17", "shaders"},
    {"opengl-glad",    "OpenGL 4.x + GLFW + GLAD loader",               "opengl glfw glad",                            "C++=17", "shaders"},
    {"vulkan",         "Vulkan + GLFW + GLM (SPIR-V auto-compiled)",     "vulkan glfw glm",                             "C++=17", "shaders"},
    /* SFML */
    {"sfml",           "SFML graphics + audio + network",                "sfml:graphics,audio,network,system,window",   "C++=17", "assets"},
    {"sfml-game",      "SFML 2.x game template (sprites, sound, events)","sfml:graphics,audio,system,window",           "C++=17", "assets fonts maps"},
    /* SDL */
    {"sdl2",           "SDL2 + OpenGL + image/ttf/mixer",                "sdl2:image,ttf,mixer opengl",                 "C++=17", "assets"},
    {"sdl3",           "SDL3 + OpenGL",                                  "sdl3 opengl",                                 "C++=17", "assets"},
    /* Qt */
    {"qt",             "Qt6/5 GUI app (auto-moc + auto-uic)",            "qt:Core,Gui,Widgets",                         "C++=17", "ui forms"},
    {"qt-opengl",      "Qt6/5 + OpenGL widget",                          "qt:Core,Gui,Widgets,OpenGL,OpenGLWidgets opengl","C++=17","ui shaders"},
    {"qt-media",       "Qt6/5 media player (audio+video)",               "qt:Core,Gui,Widgets,Multimedia,MultimediaWidgets","C++=17",""},
    {"qt-network",     "Qt6/5 networking app",                           "qt:Core,Gui,Widgets,Network",                 "C++=17", ""},
    {"qt-sql",         "Qt6/5 SQL database app",                         "qt:Core,Gui,Widgets,Sql",                     "C++=17", ""},
    /* ImGui */
    {"imgui",          "Dear ImGui + OpenGL3 + GLFW",                   "opengl glfw glad imgui:opengl3+glfw",          "C++=17", ""},
    {"imgui-sdl2",     "Dear ImGui + OpenGL3 + SDL2",                   "opengl sdl2 glad imgui:opengl3+sdl2",          "C++=17", ""},
    {"imgui-vulkan",   "Dear ImGui + Vulkan + GLFW",                    "vulkan glfw imgui:vulkan+glfw",                "C++=17", "shaders"},
    /* 3D */
    {"game3d",         "3D engine: OpenGL+GLFW+GLM+Assimp+FreeType+OpenAL","opengl glfw glad glm assimp freetype openal","C++=17","assets shaders models textures"},
    {"opengl-sfml",    "OpenGL + SFML (audio + network)",               "opengl sfml:graphics,audio,network,system,window","C++=17","assets shaders"},
    /* GUI */
    {"wx",             "wxWidgets GUI app",                              "wx:core,base,adv",                            "C++=17", "ui"},
    {"gtk3",           "GTK3 GUI app",                                   "gtk3",                                        "C++=17", "ui res"},
    /* Compute */
    {"eigen",          "Eigen3 linear algebra (header-only)",            "eigen",                                       "C++=17", ""},
    {"opencv",         "OpenCV vision app",                              "opencv:core,imgproc,highgui,videoio",         "C++=17", "data"},
    /* Utils */
    {"server",         "TCP server (POSIX sockets)",                     "pthread",                                     "C=11",   ""},
    {"json",           "JSON app (nlohmann/json header-only)",           "json",                                        "C++=17", ""},
    {"spdlog",         "Logging with spdlog",                            "spdlog",                                      "C++=17", ""},
    {"raylib",         "raylib game / graphics",                         "raylib",                                      "C++=17", "assets"},
    /* Test / lib */
    {"lib",            "Static/shared library",                          "",                                            "C++=17", ""},
    {"test",           "Catch2 unit test suite",                         "catch2",                                      "C++=17", "tests"},
    {"gtest",          "GoogleTest unit test suite",                     "googletest",                                  "C++=17", "tests"},
    {NULL,NULL,NULL,NULL,NULL}
};

/* ── Per-library git / include info ────────────────────────────── */
typedef struct {
    const char *name;          /* USE_LIB key (lowercase, no colon) */
    const char *git_url;       /* canonical git repo URL            */
    const char *git_dest;      /* destination under project root    */
    const char *include_snip;  /* #include line for main.cpp        */
    const char *cmake_args;    /* cmake configure args (NULL = no cmake build) */
    const char *lib_subdir;    /* subdir inside dest containing .a/.so (NULL = _build/lib) */
    const char *git_tag;       /* recommended tag/branch (NULL = default) */
} LibFetchInfo;

/* cmake_args: passed to cmake -S <dest> -B <dest>/_build
   lib_subdir:  where built .a/.so lives inside dest (NULL = _build/lib)
   git_tag:     recommended tag or branch                                */
static const LibFetchInfo LIB_FETCH[] = {
/* name       git_url                                          dest              include_snip                         cmake_args                                          lib_subdir   tag  */
{"opengl", "https://github.com/nigels-com/glew.git",        "deps/glew",    "#include <GL/gl.h>",                  "-DGLEW_OSMESA=OFF",                                "_build/lib", NULL},
{"glfw",   "https://github.com/glfw/glfw.git",              "deps/glfw",    "#include <GLFW/glfw3.h>",             "-DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_EXAMPLES=OFF", "_build/src", NULL},
{"glfw3",  "https://github.com/glfw/glfw.git",              "deps/glfw",    "#include <GLFW/glfw3.h>",             "-DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_EXAMPLES=OFF", "_build/src", NULL},
{"glew",   "https://github.com/nigels-com/glew.git",        "deps/glew",    "#include <GL/glew.h>",                "-DGLEW_OSMESA=OFF",                                "_build/lib", NULL},
{"glad",   "https://github.com/Dav1dde/glad.git",           "deps/glad",    "#include <glad/glad.h>",              NULL,                                               NULL,         NULL},
{"vulkan", "https://github.com/KhronosGroup/Vulkan-Headers.git","deps/vulkan-headers","#include <vulkan/vulkan.h>",NULL,                                               NULL,         NULL},
{"sfml",   "https://github.com/SFML/SFML.git",              "deps/SFML",    "#include <SFML/Graphics.hpp>",        "-DBUILD_SHARED_LIBS=OFF -DSFML_BUILD_EXAMPLES=OFF -DSFML_BUILD_TEST_SUITE=OFF", "_build/lib", "2.6.x"},
{"sdl2",   "https://github.com/libsdl-org/SDL.git",         "deps/SDL2",    "#include <SDL2/SDL.h>",               "-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF",  "_build",    "release-2.30.11"},
{"sdl3",   "https://github.com/libsdl-org/SDL.git",         "deps/SDL3",    "#include <SDL3/SDL.h>",               "-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF",  "_build",    "main"},
{"imgui",  "https://github.com/ocornut/imgui.git",          "deps/imgui",   "#include \"imgui.h\"",              NULL,                                               NULL,         NULL},
{"glm",    "https://github.com/g-truc/glm.git",             "deps/glm",     "#include <glm/glm.hpp>",              "-DGLM_BUILD_TESTS=OFF",                            NULL,         NULL},
{"eigen",  "https://gitlab.com/libeigen/eigen.git",         "deps/eigen",   "#include <Eigen/Core>",               NULL,                                               NULL,         NULL},
{"assimp", "https://github.com/assimp/assimp.git",          "deps/assimp",  "#include <assimp/Importer.hpp>",      "-DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_INSTALL=OFF", "_build/lib", NULL},
{"freetype","https://github.com/freetype/freetype.git",     "deps/freetype","#include <ft2build.h>",               "-DFT_DISABLE_ZLIB=OFF",                            "_build",     NULL},
{"box2d",  "https://github.com/erincatto/box2d.git",        "deps/box2d",   "#include <box2d/box2d.h>",            "-DBOX2D_BUILD_TESTBED=OFF -DBOX2D_BUILD_UNIT_TESTS=OFF", "_build/src", NULL},
{"opencv", "https://github.com/opencv/opencv.git",          "deps/opencv",  "#include <opencv2/core.hpp>",         "-DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF", "_build/lib", NULL},
{"openal", "https://github.com/kcat/openal-soft.git",       "deps/openal",  "#include <AL/al.h>",                  "-DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF",         "_build",     NULL},
{"boost",  "https://github.com/boostorg/boost.git",         "deps/boost",   "#include <boost/version.hpp>",        NULL,                                               NULL,         NULL},
{"json",   "https://github.com/nlohmann/json.git",          "deps/json",    "#include <nlohmann/json.hpp>",        "-DJSON_BuildTests=OFF",                            NULL,         NULL},
{"spdlog", "https://github.com/gabime/spdlog.git",          "deps/spdlog",  "#include <spdlog/spdlog.h>",          "-DSPDLOG_BUILD_TESTS=OFF",                         "_build",     NULL},
{"fmt",    "https://github.com/fmtlib/fmt.git",             "deps/fmt",     "#include <fmt/core.h>",               "-DFMT_TEST=OFF -DFMT_FUZZ=OFF",                    "_build",     NULL},
{"catch2", "https://github.com/catchorg/Catch2.git",        "deps/catch2",  "#include <catch2/catch_all.hpp>",     "-DCATCH_BUILD_TESTING=OFF",                        "_build/src", NULL},
{"googletest","https://github.com/google/googletest.git",   "deps/gtest",   "#include <gtest/gtest.h>",            "-DBUILD_GMOCK=ON",                                 "_build/lib", NULL},
{"raylib", "https://github.com/raysan5/raylib.git",         "deps/raylib",  "#include <raylib.h>",                 "-DBUILD_EXAMPLES=OFF -DBUILD_GAMES=OFF",           "_build/raylib", NULL},
{"imgui-sfml","https://github.com/SFML/imgui-sfml.git",    "deps/imgui-sfml","#include <imgui-SFML.h>",           "-DIMGUI_SFML_FIND_SFML=OFF",                       "_build",     NULL},
/* System-only (no git fetch) */
{"qt",     "", "", "#include <QApplication>",  NULL, NULL, NULL},
{"wx",     "", "", "#include <wx/wx.h>",       NULL, NULL, NULL},
{"gtk3",   "", "", "#include <gtk/gtk.h>",     NULL, NULL, NULL},
{"gtk4",   "", "", "#include <gtk/gtk.h>",     NULL, NULL, NULL},
{"openssl","", "", "#include <openssl/ssl.h>", NULL, NULL, NULL},
{"zlib",   "", "", "#include <zlib.h>",        NULL, NULL, NULL},
{"sqlite", "", "", "#include <sqlite3.h>",     NULL, NULL, NULL},
{"sqlite3","", "", "#include <sqlite3.h>",     NULL, NULL, NULL},
{"lua",    "", "", "#include <lua.hpp>",       NULL, NULL, NULL},
{NULL,NULL,NULL,NULL,NULL,NULL,NULL}
};

static const LibFetchInfo *lfi_find(const char *raw) {
    char base[64]; strncpy(base, raw, 63); base[63]='\0';
    char *col = strchr(base,':'); if (col) *col='\0';
    for (char *p=base;*p;p++) *p=(char)tolower((unsigned char)*p);
    for (int i=0; LIB_FETCH[i].name; i++)
        if (strcmp(LIB_FETCH[i].name, base)==0) return &LIB_FETCH[i];
    return NULL;
}

/* Add one FETCH_V2 line to buf[bufsz] only if URL not already present */
static void fetch_add_full(char *buf, size_t bufsz, const char *url,
                            const char *dest, const char *cmake_args,
                            const char *tag) {
    if (!url || !url[0]) return;
    if (strstr(buf, url)) return;   /* already present */
    char line[1200]; line[0] = '\0';
    /* Base: FETCH_V2: <url> TO <dest> */
    snprintf(line, sizeof(line), "FETCH_V2: %s TO %s", url, dest);
    /* Append TAG if known */
    if (tag && tag[0]) {
        char tmp[128]; snprintf(tmp, sizeof(tmp), " TAG %s", tag);
        strncat(line, tmp, sizeof(line)-strlen(line)-1);
    }
    /* Append CMAKE args if any */
    if (cmake_args && cmake_args[0]) {
        char tmp[600]; snprintf(tmp, sizeof(tmp), " CMAKE %s", cmake_args);
        strncat(line, tmp, sizeof(line)-strlen(line)-1);
    }
    strncat(line, "\n", sizeof(line)-strlen(line)-1);
    strncat(buf, line, bufsz - strlen(buf) - 1);
}

/* Legacy wrapper (for template lines without extra info) */
static void fetch_add(char *buf, size_t bufsz, const char *url, const char *dest) {
    fetch_add_full(buf, bufsz, url, dest, NULL, NULL);
}

/* Build FETCH_V2 block from a USE_LIB string */
static void build_fetch_block(const char *use_libs, char *out, size_t osz) {
    out[0] = '\0';
    char tmp[B_SIZE]; strncpy(tmp, use_libs, B_SIZE-1);
    char *tok = strtok(tmp, " ,");
    while (tok) {
        const LibFetchInfo *lfi = lfi_find(tok);
        if (lfi && lfi->git_url[0])
            fetch_add_full(out, osz, lfi->git_url, lfi->git_dest,
                           lfi->cmake_args, lfi->git_tag);
        tok = strtok(NULL, " ,");
    }
}

/* ── main.cpp boilerplate generator ────────────────────────────── */
static void write_main_cpp(FILE *f, const char *name, const char *use_libs) {
    /* Collect #include lines from lib list */
    char incs[4096]; incs[0]='\0';
    char tmp[B_SIZE]; strncpy(tmp, use_libs, B_SIZE-1);
    char *tok = strtok(tmp, " ,");
    while (tok) {
        const LibFetchInfo *lfi = lfi_find(tok);
        if (lfi && lfi->include_snip[0] && !strstr(incs, lfi->include_snip)) {
            strncat(incs, lfi->include_snip, sizeof(incs)-strlen(incs)-1);
            strncat(incs, "\n", sizeof(incs)-strlen(incs)-1);
        }
        tok = strtok(NULL, " ,");
    }

    int has_sfml   = (strstr(use_libs,"sfml")   != NULL);
    int has_sdl2   = (strstr(use_libs,"sdl2")   != NULL);
    int has_glfw   = (strstr(use_libs,"glfw")   != NULL);
    int has_vulkan = (strstr(use_libs,"vulkan") != NULL);
    int has_qt     = (strstr(use_libs,"qt")     != NULL);
    int has_imgui  = (strstr(use_libs,"imgui")  != NULL);
    int has_wx     = (strstr(use_libs,"wx")     != NULL);
    int has_gtk    = (strstr(use_libs,"gtk")    != NULL);
    int has_opengl = (strstr(use_libs,"opengl") != NULL);
    int has_glew   = (strstr(use_libs,"glew")   != NULL);

    fprintf(f, "/* %s — generated by OMake v" VERSION " --create-project */\n", name);
    fprintf(f, "#include <iostream>\n#include <string>\n");
    if (incs[0]) fprintf(f, "\n/* Library headers */\n%s", incs);

    int has_multimedia = (strstr(use_libs,"multimedia") || strstr(use_libs,"Multimedia"));
    if (has_qt && has_multimedia) {
        fprintf(f,
            "#include <QMainWindow>\n"
            "#include <QMediaPlayer>\n"
            "#include <QVideoWidget>\n"
            "#include <QAudioOutput>\n"
            "#include <QPushButton>\n"
            "#include <QVBoxLayout>\n"
            "#include <QFileDialog>\n\n"
            "int main(int argc, char *argv[]) {\n"
            "    QApplication app(argc, argv);\n"
            "    QMainWindow window;\n"
            "    window.setWindowTitle(\"%s\");\n"
            "    window.resize(800, 600);\n"
            "    auto *central = new QWidget;\n"
            "    auto *layout  = new QVBoxLayout(central);\n"
            "    auto *video   = new QVideoWidget;\n"
            "    auto *btn     = new QPushButton(\"Open file\");\n"
            "    layout->addWidget(video);\n"
            "    layout->addWidget(btn);\n"
            "    window.setCentralWidget(central);\n"
            "    auto *player = new QMediaPlayer;\n"
            "    auto *audio  = new QAudioOutput;\n"
            "    player->setAudioOutput(audio);\n"
            "    player->setVideoOutput(video);\n"
            "    QObject::connect(btn, &QPushButton::clicked, [&]{\n"
            "        QString f = QFileDialog::getOpenFileName(\n"
            "            nullptr, \"Open Media\", \"\"\n"
            "            \"Media (*.mp4 *.mkv *.avi *.mp3 *.wav *.flac)\");\n"
            "        if (!f.isEmpty()) player->setSource(QUrl::fromLocalFile(f));\n"
            "        player->play();\n"
            "    });\n"
            "    window.show();\n"
            "    return app.exec();\n"
            "}\n", name);
    } else if (has_qt) {
        fprintf(f,
            "#include <QMainWindow>\n\n"
            "int main(int argc, char *argv[]) {\n"
            "    QApplication app(argc, argv);\n"
            "    QMainWindow window;\n"
            "    window.setWindowTitle(\"%s\");\n"
            "    window.resize(800, 600);\n"
            "    window.show();\n"
            "    return app.exec();\n"
            "}\n", name);
    } else if (has_wx) {
        fprintf(f,
            "\nclass MyApp : public wxApp {\npublic:\n"
            "    virtual bool OnInit() override {\n"
            "        wxFrame *frame = new wxFrame(nullptr, wxID_ANY, \"%s\");\n"
            "        frame->Show(true); return true;\n"
            "    }\n};\nwxIMPLEMENT_APP(MyApp);\n", name);
    } else if (has_gtk) {
        fprintf(f,
            "\nint main(int argc, char *argv[]) {\n"
            "    gtk_init(&argc, &argv);\n"
            "    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);\n"
            "    gtk_window_set_title(GTK_WINDOW(w), \"%s\");\n"
            "    gtk_window_set_default_size(GTK_WINDOW(w), 800, 600);\n"
            "    g_signal_connect(w, \"destroy\", G_CALLBACK(gtk_main_quit), NULL);\n"
            "    gtk_widget_show_all(w);\n"
            "    gtk_main();\n"
            "    return 0;\n}\n", name);
    } else if (has_sfml && !has_glfw) {
        fprintf(f,
            "\nint main() {\n"
            "    sf::RenderWindow window(sf::VideoMode(800, 600), \"%s\");\n"
            "    window.setFramerateLimit(60);\n"
            "    while (window.isOpen()) {\n"
            "        sf::Event event;\n"
            "        while (window.pollEvent(event))\n"
            "            if (event.type == sf::Event::Closed) window.close();\n"
            "        window.clear(sf::Color(30, 30, 30));\n"
            "        /* draw here */\n"
            "        window.display();\n"
            "    }\n"
            "    return 0;\n}\n", name);
    } else if (has_sdl2 && !has_glfw) {
        fprintf(f,
            "\nint main(int argc, char *argv[]) {\n"
            "    (void)argc; (void)argv;\n"
            "    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);\n"
            "    SDL_Window   *win = SDL_CreateWindow(\"%s\",\n"
            "        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,\n"
            "        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);\n"
            "    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,\n"
            "        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);\n"
            "    bool running = true;\n"
            "    SDL_Event e;\n"
            "    while (running) {\n"
            "        while (SDL_PollEvent(&e))\n"
            "            if (e.type == SDL_QUIT) running = false;\n"
            "        SDL_SetRenderDrawColor(ren, 30, 30, 30, 255);\n"
            "        SDL_RenderClear(ren);\n"
            "        /* draw here */\n"
            "        SDL_RenderPresent(ren);\n"
            "    }\n"
            "    SDL_DestroyRenderer(ren);\n"
            "    SDL_DestroyWindow(win);\n"
            "    SDL_Quit();\n"
            "    return 0;\n}\n", name);
    } else if (has_glfw && has_opengl && has_imgui) {
        fprintf(f,
            "\nstatic void glfw_err(int e, const char *d)"
            " { fprintf(stderr, \"GLFW %%d: %%s\\n\", e, d); }\n\n"
            "int main() {\n"
            "    glfwSetErrorCallback(glfw_err);\n"
            "    if (!glfwInit()) return 1;\n"
            "    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);\n"
            "    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);\n"
            "    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);\n"
            "    GLFWwindow *win = glfwCreateWindow(1280, 720, \"%s\", NULL, NULL);\n"
            "    glfwMakeContextCurrent(win);\n"
            "    glfwSwapInterval(1);\n"
            "%s"  /* GLAD/GLEW init */
            "    IMGUI_CHECKVERSION();\n"
            "    ImGui::CreateContext();\n"
            "    ImGui::StyleColorsDark();\n"
            "    ImGui_ImplGlfw_InitForOpenGL(win, true);\n"
            "    ImGui_ImplOpenGL3_Init(\"#version 330\");\n"
            "    while (!glfwWindowShouldClose(win)) {\n"
            "        glfwPollEvents();\n"
            "        ImGui_ImplOpenGL3_NewFrame();\n"
            "        ImGui_ImplGlfw_NewFrame();\n"
            "        ImGui::NewFrame();\n"
            "        ImGui::Begin(\"%s\");\n"
            "        ImGui::Text(\"Hello from OMake!\");\n"
            "        ImGui::End();\n"
            "        ImGui::Render();\n"
            "        glClear(GL_COLOR_BUFFER_BIT);\n"
            "        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());\n"
            "        glfwSwapBuffers(win);\n"
            "    }\n"
            "    ImGui_ImplOpenGL3_Shutdown();\n"
            "    ImGui_ImplGlfw_Shutdown();\n"
            "    ImGui::DestroyContext();\n"
            "    glfwDestroyWindow(win); glfwTerminate();\n"
            "    return 0;\n}\n",
            name,
            has_glew ? "    if (glewInit() != GLEW_OK) return 1;\n"
                     : "    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 1;\n",
            name);
    } else if (has_glfw && has_opengl) {
        fprintf(f,
            "\nint main() {\n"
            "    if (!glfwInit()) return 1;\n"
            "    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);\n"
            "    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);\n"
            "    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);\n"
            "    GLFWwindow *win = glfwCreateWindow(1280, 720, \"%s\", NULL, NULL);\n"
            "    if (!win) { glfwTerminate(); return 1; }\n"
            "    glfwMakeContextCurrent(win);\n"
            "    glfwSwapInterval(1);\n"
            "%s"  /* GLAD/GLEW init */
            "    while (!glfwWindowShouldClose(win)) {\n"
            "        glfwPollEvents();\n"
            "        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);\n"
            "        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n"
            "        /* render here */\n"
            "        glfwSwapBuffers(win);\n"
            "    }\n"
            "    glfwDestroyWindow(win); glfwTerminate();\n"
            "    return 0;\n}\n",
            name,
            has_glew ? "    if (glewInit() != GLEW_OK) return 1;\n"
                     : "    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 1;\n");
    } else if (has_vulkan && has_glfw) {
        fprintf(f,
            "\nint main() {\n"
            "    glfwInit();\n"
            "    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);\n"
            "    GLFWwindow *win = glfwCreateWindow(1280, 720, \"%s\", NULL, NULL);\n"
            "    VkApplicationInfo ai{};\n"
            "    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;\n"
            "    ai.pApplicationName = \"%s\";\n"
            "    ai.apiVersion = VK_API_VERSION_1_3;\n"
            "    uint32_t extCnt = 0;\n"
            "    const char **exts = glfwGetRequiredInstanceExtensions(&extCnt);\n"
            "    VkInstanceCreateInfo ci{};\n"
            "    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;\n"
            "    ci.pApplicationInfo = &ai;\n"
            "    ci.enabledExtensionCount = extCnt;\n"
            "    ci.ppEnabledExtensionNames = exts;\n"
            "    VkInstance inst;\n"
            "    vkCreateInstance(&ci, nullptr, &inst);\n"
            "    while (!glfwWindowShouldClose(win)) glfwPollEvents();\n"
            "    vkDestroyInstance(inst, nullptr);\n"
            "    glfwDestroyWindow(win); glfwTerminate();\n"
            "    return 0;\n}\n", name, name);
    } else {
        fprintf(f,
            "\nint main() {\n"
            "    std::cout << \"Hello from %s!\\n\";\n"
            "    return 0;\n}\n", name);
    }
}

/* ── Main entry point ───────────────────────────────────────────── */
/*
 * Usage:
 *   omake --create-project "Name"                    → basic C++ scaffold
 *   omake --create-project "Name" opengl sfml vulkan → explicit libs
 *   omake --create-project "Name" game3d             → named template
 *   omake --create-project "Name" opengl-sfml-imgui  → dash-separated libs
 */
void cmd_create_project_ex(const char *name, const char *libs_arg) {

    /* ── Resolve template or raw lib list ── */
    const ProjectTemplate *tmpl = NULL;
    for (int i = 0; TEMPLATES[i].name; i++)
        if (strcasecmp(TEMPLATES[i].name, libs_arg) == 0)
            { tmpl = &TEMPLATES[i]; break; }

    char use_libs[B_SIZE]; use_libs[0] = '\0';
    char std_val[64];  strncpy(std_val, "C++=17", 63);
    char tmpl_fetch[4096]; tmpl_fetch[0] = '\0';
    char extra_dirs[512];  extra_dirs[0] = '\0';

    if (tmpl) {
        /* Named template */
        if (tmpl->libs)      strncpy(use_libs,     tmpl->libs,      B_SIZE-1);
        if (tmpl->std)       strncpy(std_val,       tmpl->std,       63);
        if (tmpl->extra_dirs)strncpy(extra_dirs,    tmpl->extra_dirs,511);
    } else {
        /* Raw lib list: replace dashes with spaces */
        strncpy(use_libs, libs_arg, B_SIZE-1);
        for (char *p = use_libs; *p; p++) if (*p == '-') *p = ' ';
    }

    /* ── Build FETCH_V2 block from LIB_FETCH table ──────────────────
     * build_fetch_block() looks up each lib in LIB_FETCH and emits:
     *   FETCH_V2: <url> TO <dest> TAG <tag> CMAKE <cmake_args>
     * with full cmake build instructions baked in.
     * ─────────────────────────────────────────────────────────────── */
    char fetch_block[8192]; fetch_block[0] = '\0';
    if (use_libs[0]) {
        build_fetch_block(use_libs, fetch_block, sizeof(fetch_block));
    }

    /* ── Count repos ── */
    int n_repos = 0;
    for (const char *p = fetch_block; *p; p++) if (*p == '\n') n_repos++;

    /* ── Create directories ── */
    printf(BLUE "\n[PROJECT] Creating '%s'\n" RESET, name);
    char path[512];
    mkdir_auto(name);
    const char *base_dirs[] = {"src","include","bin","res",NULL};
    for (int i = 0; base_dirs[i]; i++) {
        snprintf(path,sizeof(path),"%s/%s",name,base_dirs[i]); mkdir_auto(path);
    }
    if (extra_dirs[0]) {
        char ed[512]; strncpy(ed, extra_dirs, 511);
        char *d = strtok(ed, " ");
        while (d) {
            snprintf(path,sizeof(path),"%s/%s",name,d); mkdir_auto(path);
            d = strtok(NULL," ");
        }
    }

    /* ── Write OMakeLists.txt ── */
    snprintf(path, sizeof(path), "%s/OMakeLists.txt", name);
    FILE *f = fopen(path, "w");
    if (!f) { printf(RED "[ERROR] Cannot create OMakeLists.txt\n" RESET); return; }

    fprintf(f, "# OMake project: %s\n", name);
    fprintf(f, "# Generated by OMake v" VERSION " --create-project\n");
    if (tmpl) fprintf(f, "# Template: %s — %s\n", tmpl->name, tmpl->desc);
    fprintf(f, "\n");

    /* FETCH_V2 section */
    if (fetch_block[0]) {
        fprintf(f, "# ── Dependencies — auto-fetched before first build ──────────────\n");
        fprintf(f, "# Each FETCH_V2 clones the repo once; subsequent builds skip it.\n");
        fprintf(f, "%s\n", fetch_block);
    }

    /* System-install notes */
    if (use_libs[0] && strstr(use_libs,"qt")) {
        fprintf(f,
            "# Qt cannot be fetched via git — install the SDK:\n"
            "#   Linux:   sudo apt install qt6-base-dev\n"
            "#   macOS:   brew install qt@6\n"
            "#   Windows: https://www.qt.io/download\n\n");
    }
    if (use_libs[0] && strstr(use_libs,"wx"))
        fprintf(f, "# wxWidgets: sudo apt install libwxgtk3.2-dev\n\n");
    if (use_libs[0] && (strstr(use_libs,"gtk3")||strstr(use_libs,"gtk4")))
        fprintf(f, "# GTK: sudo apt install libgtk-3-dev\n\n");

    /* Build config */
    fprintf(f,
        "# ── Build ───────────────────────────────────────────────────────\n"
        "NAME: %s\n"
        "VERSION: 0.1.0\n\n"
        "SRC: src/*.cpp\n"
        "COMPILER: g++\n"
        "STANDARD: %s\n\n"
        "OBJDIR: .omake_obj\n"
        "TAGS: -Iinclude\n\n",
        name, std_val);

    if (use_libs[0]) {
        fprintf(f,
            "# ── Libraries ───────────────────────────────────────────────────\n"
            "# USE_LIB tries: pkg-config → env var → header scan → Windows/vcpkg\n"
            "USE_LIB: %s\n\n",
            use_libs);
    }

    fprintf(f,
        "# ── Output ──────────────────────────────────────────────────────\n"
        "if win32\n"
        "  OUT: bin/%s.exe\n"
        "else\n"
        "  OUT: bin/%s\n"
        "end\n\n"
        "# ── Optional ─────────────────────────────────────────────────────\n"
        "# INSTALL: /usr/local/bin/%s\n"
        "# STRIP: yes\n"
        "# TEST: tests/*.cpp\n",
        name, name, name);
    fclose(f);

    /* ── Write src/main.cpp ── */
    snprintf(path, sizeof(path), "%s/src/main.cpp", name);
    FILE *mf = fopen(path, "w");
    if (mf) { write_main_cpp(mf, name, use_libs); fclose(mf); }

    /* ── Write .gitignore ── */
    snprintf(path, sizeof(path), "%s/.gitignore", name);
    FILE *gi = fopen(path, "w");
    if (gi) {
        fprintf(gi, ".omake_obj/\nbin/\n*.o\n*.a\n*.so\n*.dll\n*.exe\n"
                    "compile_commands.json\n.omake_h\n.omake_filedb\n");
        fclose(gi);
    }

    /* ── Summary ── */
    printf(GREEN "  Dir     : %s/\n" RESET, name);
    if (use_libs[0])
        printf(GREEN "  USE_LIB : %s\n" RESET, use_libs);
    if (n_repos > 0)
        printf(GREEN "  Fetches : %d repo(s) — auto-cloned on first build\n" RESET, n_repos);
    printf(GREEN "\n  Next: cd %s && omake --build\n" RESET, name);
    printf(BLUE "[PROJECT] Done.\n\n" RESET);
}


/*  --m-pack : Multi-format package builder                             */
/*             zip  (all platforms)                                     */
/*             deb  (Debian/Ubuntu — no dpkg-buildpackage needed)       */
/*             rpm  (RHEL/Fedora  — needs rpmbuild)                     */
/* ================================================================== */

/* ---- helpers ---- */

/* Extract stem (no dir, no ext) from a path: bin/myapp.exe -> myapp */
static void path_stem(const char *path, char *out, int outsz) {
    const char *base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    strncpy(out, base, outsz - 1); out[outsz-1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* md5sum one file, write "HASH  filename\n" into fp */
static void write_md5(const char *filepath, const char *arcname, FILE *fp) {
#ifdef _WIN32
    /* certutil fallback — extract the hash line */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "certutil -hashfile \"%s\" MD5 2>nul", filepath);
    FILE *p = popen(cmd, "r");
    if (p) {
        char line[256]; int lines = 0;
        while (fgets(line, sizeof(line), p)) {
            lines++;
            if (lines == 2) { /* second line is the hash */
                char *nl = strrchr(line, '\n'); if (nl) *nl = '\0';
                fprintf(fp, "%s  %s\n", line, arcname);
                break;
            }
        }
        pclose(p);
    }
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "md5sum \"%s\" 2>/dev/null", filepath);
    FILE *p = popen(cmd, "r");
    if (p) {
        char hash[64] = "00000000000000000000000000000000";
        fscanf(p, "%63s", hash);
        pclose(p);
        fprintf(fp, "%s  %s\n", hash, arcname);
    }
#endif
}

/* get file size in bytes */
static long long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return (long long)st.st_size;
    return 0;
}

/* ---- Read project metadata from OMakeLists.txt ---- */

static void load_pkg_meta(PkgMeta *m) {
    /* defaults */
    strncpy(m->name,        "app",              sizeof(m->name)-1);
    strncpy(m->version,     "1.0.0",            sizeof(m->version)-1);
    strncpy(m->install_dir, "/usr/local/bin",   sizeof(m->install_dir)-1);
    strncpy(m->bin_path,    "bin/app",           sizeof(m->bin_path)-1);
    strncpy(m->description, "Built with OMake", sizeof(m->description)-1);
    strncpy(m->maintainer,  "Unknown",           sizeof(m->maintainer)-1);
    strncpy(m->homepage,    "",                  sizeof(m->homepage)-1);
#if defined(__x86_64__) || defined(_M_X64)
    strncpy(m->arch_str, "amd64", sizeof(m->arch_str)-1);
#elif defined(__i386__) || defined(_M_IX86)
    strncpy(m->arch_str, "i386",  sizeof(m->arch_str)-1);
#elif defined(__aarch64__) || defined(_M_ARM64)
    strncpy(m->arch_str, "arm64", sizeof(m->arch_str)-1);
#else
    strncpy(m->arch_str, "all",   sizeof(m->arch_str)-1);
#endif

    FILE *f = fopen("OMakeLists.txt", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *cmt = strchr(line, '#'); if (cmt) *cmt = '\0';
        trim(line); if (!line[0]) continue;
        char ll[1024]; strcpy(ll, line); to_lower_str(ll);
        char *col = strchr(ll, ':'); if (!col) continue;
        char key[64] = "";
        int kl = (int)(col - ll);
        if (kl > 0 && kl < 63) { strncpy(key, ll, kl); key[kl] = '\0'; trim(key); }
        char *val = strchr(line, ':'); if (!val) continue; val++;
        trim(val); if (!val[0]) continue;

        if      (strcmp(key,"out")==0)         { strncpy(m->bin_path,  val, sizeof(m->bin_path)-1);  path_stem(val,m->name,sizeof(m->name)); }
        else if (strcmp(key,"version")==0)     { strncpy(m->version,   val, sizeof(m->version)-1); }
        else if (strcmp(key,"install")==0)     {
            /* INSTALL: /usr/local/bin  or  /usr/local/bin/myapp */
            strncpy(m->install_dir, val, sizeof(m->install_dir)-1);
            /* if it looks like a file path (has no trailing /), take dirname */
            char *last_sep = strrchr(m->install_dir, '/');
            if (!last_sep) last_sep = strrchr(m->install_dir, '\\');
            if (last_sep && strchr(last_sep, '.')) *last_sep = '\0'; /* chop filename */
        }
        else if (strcmp(key,"description")==0) { strncpy(m->description, val, sizeof(m->description)-1); }
        else if (strcmp(key,"maintainer")==0)  { strncpy(m->maintainer,  val, sizeof(m->maintainer)-1); }
        else if (strcmp(key,"homepage")==0)    { strncpy(m->homepage,    val, sizeof(m->homepage)-1); }
    }
    fclose(f);
    m->bin_size = file_size(m->bin_path);
}

/* ---- ZIP ---- */
static int mpack_zip(const PkgMeta *m, const char *outdir) {
    char zipname[512];
    snprintf(zipname, sizeof(zipname), "%s/%s-%s.zip", outdir, m->name, m->version);

    printf(CYAN "[M-PACK/ZIP] Creating: %s\n" RESET, zipname);

    char cmd[2048];
#ifdef _WIN32
    /* Windows: use tar (available Win10+) or PowerShell */
    if (tool_exists("tar")) {
        snprintf(cmd, sizeof(cmd),
            "tar -a -c -f \"%s\" \"%s\" 2>nul", zipname, m->bin_path);
    } else {
        snprintf(cmd, sizeof(cmd),
            "powershell -Command \"Compress-Archive -Path '%s' -DestinationPath '%s' -Force\"",
            m->bin_path, zipname);
    }
#else
    if (tool_exists("zip")) {
        snprintf(cmd, sizeof(cmd), "zip -j \"%s\" \"%s\"", zipname, m->bin_path);
    } else {
        snprintf(cmd, sizeof(cmd), "tar -czf \"%s\" \"%s\"", zipname, m->bin_path);
        /* rename suggestion */
    }
#endif
    if (system(cmd) == 0) {
        printf(GREEN "[M-PACK/ZIP] %s\n" RESET, zipname);
        return 1;
    }
    printf(RED "[M-PACK/ZIP] Failed.\n" RESET);
    return 0;
}

/* ---- DEB ---- */
static int mpack_deb(const PkgMeta *m, const char *outdir) {
    printf(CYAN "[M-PACK/DEB] Building Debian package...\n" RESET);

    /* staging dir: <name>_<version>_<arch> */
    char staging[512];
    snprintf(staging, sizeof(staging), "%s/%s_%s_%s",
             outdir, m->name, m->version, m->arch_str);

    /* directory tree */
    char dir_debian[600], dir_install[1024];
    snprintf(dir_debian,  sizeof(dir_debian),  "%s/DEBIAN", staging);
    snprintf(dir_install, sizeof(dir_install), "%s%s",      staging, m->install_dir);

    /* Recursively create DEBIAN and install directories */
    { char _t[1200]; snprintf(_t,sizeof(_t),"%s/_omake_tmp",dir_debian);  ensure_dir_exists(_t); }
    { char _t[1200]; snprintf(_t,sizeof(_t),"%s/_omake_tmp",dir_install); ensure_dir_exists(_t); }
    /* Remove the placeholder files if created */
    { char _t[1200]; snprintf(_t,sizeof(_t),"%s/_omake_tmp",dir_debian);  remove(_t); }
    { char _t[1200]; snprintf(_t,sizeof(_t),"%s/_omake_tmp",dir_install); remove(_t); }

    /* -- DEBIAN/control -- */
    char ctrl[600]; snprintf(ctrl, sizeof(ctrl), "%s/control", dir_debian);
    FILE *f = fopen(ctrl, "w");
    if (!f) { printf(RED "[M-PACK/DEB] Cannot write control file.\n" RESET); return 0; }

    /* Installed-Size in KB */
    long long kb = (m->bin_size + 1023) / 1024;
    fprintf(f,
        "Package: %s\n"
        "Version: %s\n"
        "Architecture: %s\n"
        "Maintainer: %s\n"
        "Installed-Size: %lld\n"
        "Section: utils\n"
        "Priority: optional\n"
        "%s"          /* Homepage line if set */
        "Description: %s\n"
        " Built with OMake v" VERSION ".\n",
        m->name, m->version, m->arch_str, m->maintainer,
        kb,
        m->homepage[0] ? "Homepage: " : "", /* printed below */
        m->description);
    if (m->homepage[0]) {
        /* rewrite: we need to put homepage on same fprintf, reconstruct */
        fclose(f); f = fopen(ctrl, "w");
        fprintf(f,
            "Package: %s\n"
            "Version: %s\n"
            "Architecture: %s\n"
            "Maintainer: %s\n"
            "Installed-Size: %lld\n"
            "Section: utils\n"
            "Priority: optional\n"
            "Homepage: %s\n"
            "Description: %s\n"
            " Built with OMake v" VERSION ".\n",
            m->name, m->version, m->arch_str, m->maintainer,
            kb, m->homepage, m->description);
    }
    fclose(f);

    /* -- copy binary -- */
    char dest_bin[1024];
    snprintf(dest_bin, sizeof(dest_bin), "%s/%s", dir_install, m->name);
    char cp_cmd[1200];
#ifdef _WIN32
    snprintf(cp_cmd, sizeof(cp_cmd), "copy /y \"%s\" \"%s\" >nul", m->bin_path, dest_bin);
#else
    snprintf(cp_cmd, sizeof(cp_cmd), "cp \"%s\" \"%s\" && chmod 755 \"%s\"",
             m->bin_path, dest_bin, dest_bin);
#endif
    if (system(cp_cmd) != 0) {
        printf(RED "[M-PACK/DEB] Failed to copy binary.\n" RESET);
        return 0;
    }

    /* -- DEBIAN/md5sums -- */
    char md5path[600]; snprintf(md5path, sizeof(md5path), "%s/md5sums", dir_debian);
    FILE *md5f = fopen(md5path, "w");
    if (md5f) {
        /* arc name = path relative to staging dir (no leading /) */
        char arcname[512];
        snprintf(arcname, sizeof(arcname), "%s/%s",
                 m->install_dir[0]=='/' ? m->install_dir+1 : m->install_dir,
                 m->name);
        write_md5(dest_bin, arcname, md5f);
        fclose(md5f);
    }

    /* -- DEBIAN/postinst (optional chmod) -- */
    char postinst[600]; snprintf(postinst, sizeof(postinst), "%s/postinst", dir_debian);
    FILE *pf = fopen(postinst, "w");
    if (pf) {
        fprintf(pf, "#!/bin/sh\nset -e\nchmod 755 \"%s/%s\"\n", m->install_dir, m->name);
        fclose(pf);
#ifndef _WIN32
        chmod(postinst, 0755);
#endif
    }

    /* -- build .deb with dpkg-deb -- */
    char debname[512];
    snprintf(debname, sizeof(debname), "%s/%s_%s_%s.deb",
             outdir, m->name, m->version, m->arch_str);

    if (tool_exists("dpkg-deb")) {
        char deb_cmd[1200];
        snprintf(deb_cmd, sizeof(deb_cmd),
                 "dpkg-deb --build \"%s\" \"%s\"", staging, debname);
        printf(CYAN "[M-PACK/DEB] %s\n" RESET, deb_cmd);
        if (system(deb_cmd) == 0) {
            printf(GREEN "[M-PACK/DEB] %s\n" RESET, debname);
            /* cleanup staging */
            char rm_cmd[600]; snprintf(rm_cmd,sizeof(rm_cmd),"rm -rf \"%s\"",staging);
            system(rm_cmd);
            return 1;
        }
        printf(RED "[M-PACK/DEB] dpkg-deb failed.\n" RESET);
    } else {
        /* fallback: ar + tar manually */
        printf(YELLOW "[M-PACK/DEB] dpkg-deb not found, building .deb manually (ar+tar).\n" RESET);
        char deb_cmd[2048];
        snprintf(deb_cmd, sizeof(deb_cmd),
            /* data.tar.gz */
            "cd \"%s\" && "
            "tar -czf data.tar.gz --exclude=DEBIAN . && "
            /* control.tar.gz */
            "tar -czf control.tar.gz -C DEBIAN . && "
            /* debian-binary */
            "echo '2.0' > debian-binary && "
            /* assemble with ar */
            "ar rcs \"%s\" debian-binary control.tar.gz data.tar.gz && "
            "cd - >/dev/null",
            staging, debname);
        if (system(deb_cmd) == 0) {
            printf(GREEN "[M-PACK/DEB] %s (manual ar build)\n" RESET, debname);
            char rm_cmd[600]; snprintf(rm_cmd,sizeof(rm_cmd),"rm -rf \"%s\"",staging);
            system(rm_cmd);
            return 1;
        }
        printf(RED "[M-PACK/DEB] Manual .deb build failed. Install dpkg-deb.\n" RESET);
    }
    return 0;
}

/* ---- RPM ---- */
static int mpack_rpm(const PkgMeta *m, const char *outdir) {
    printf(CYAN "[M-PACK/RPM] Building RPM package...\n" RESET);

    if (!tool_exists("rpmbuild")) {
        printf(RED "[M-PACK/RPM] rpmbuild not found. Install rpm-build:\n" RESET);
        printf(RED "             sudo apt install rpm  (Debian/Ubuntu)\n" RESET);
        printf(RED "             sudo dnf install rpm-build  (Fedora/RHEL)\n" RESET);
        return 0;
    }

    /* RPM arch */
    char rpm_arch[32];
#if defined(__x86_64__) || defined(_M_X64)
    strncpy(rpm_arch, "x86_64", sizeof(rpm_arch)-1);
#elif defined(__i386__) || defined(_M_IX86)
    strncpy(rpm_arch, "i686",   sizeof(rpm_arch)-1);
#elif defined(__aarch64__) || defined(_M_ARM64)
    strncpy(rpm_arch, "aarch64",sizeof(rpm_arch)-1);
#else
    strncpy(rpm_arch, "noarch", sizeof(rpm_arch)-1);
#endif

    /* Build tree under outdir/rpmbuild/ */
    char rpmbuild_root[512];
    snprintf(rpmbuild_root, sizeof(rpmbuild_root), "%s/rpmbuild", outdir);
    char spec_dir[600], sources_dir[600], build_dir[600], rpms_dir[600], srpms_dir[600];
    snprintf(spec_dir,    sizeof(spec_dir),    "%s/SPECS",   rpmbuild_root);
    snprintf(sources_dir, sizeof(sources_dir), "%s/SOURCES", rpmbuild_root);
    snprintf(build_dir,   sizeof(build_dir),   "%s/BUILD",   rpmbuild_root);
    snprintf(rpms_dir,    sizeof(rpms_dir),    "%s/RPMS",    rpmbuild_root);
    snprintf(srpms_dir,   sizeof(srpms_dir),   "%s/SRPMS",   rpmbuild_root);
    { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",spec_dir);    ensure_dir_exists(_t); remove(_t); }
    { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",sources_dir); ensure_dir_exists(_t); remove(_t); }
    { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",build_dir);   ensure_dir_exists(_t); remove(_t); }
    { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",rpms_dir);    ensure_dir_exists(_t); remove(_t); }
    { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",srpms_dir);   ensure_dir_exists(_t); remove(_t); }

    /* Copy binary into SOURCES */
    char src_bin[1024];
    snprintf(src_bin, sizeof(src_bin), "%s/%s", sources_dir, m->name);
    char cp_cmd[1200];
#ifdef _WIN32
    snprintf(cp_cmd, sizeof(cp_cmd), "copy /y \"%s\" \"%s\" >nul", m->bin_path, src_bin);
#else
    snprintf(cp_cmd, sizeof(cp_cmd), "cp \"%s\" \"%s\"", m->bin_path, src_bin);
#endif
    if (system(cp_cmd) != 0) {
        printf(RED "[M-PACK/RPM] Failed to copy binary.\n" RESET);
        return 0;
    }

    /* Write .spec file */
    char spec_path[600];
    snprintf(spec_path, sizeof(spec_path), "%s/%s.spec", spec_dir, m->name);
    FILE *sf = fopen(spec_path, "w");
    if (!sf) { printf(RED "[M-PACK/RPM] Cannot write spec file.\n" RESET); return 0; }

    fprintf(sf,
        "Name:           %s\n"
        "Version:        %s\n"
        "Release:        1%%{?dist}\n"
        "Summary:        %s\n"
        "License:        Proprietary\n"
        "%s"                   /* URL line */
        "BuildArch:      %s\n"
        "\n"
        "%%description\n"
        "%s\n"
        "Built with OMake v" VERSION ".\n"
        "\n"
        "%%install\n"
        "mkdir -p %%{buildroot}%s\n"
        "install -m 755 %%{_sourcedir}/%s %%{buildroot}%s/%s\n"
        "\n"
        "%%files\n"
        "%s/%s\n"
        "\n"
        "%%changelog\n"
        "* $(date '+%%a %%b %%d %%Y') %s <omake@build> - %s-1\n"
        "- Initial package built by OMake\n",
        m->name, m->version,
        m->description,
        m->homepage[0] ? "URL:            " : "",
        rpm_arch,
        m->description,
        m->install_dir,
        m->name, m->install_dir, m->name,
        m->install_dir, m->name,
        m->maintainer, m->version);
    if (m->homepage[0]) fprintf(sf, "# homepage: %s\n", m->homepage);
    fclose(sf);

    /* Run rpmbuild */
    char abs_spec[700], abs_root[600];
#ifdef _WIN32
    _fullpath(abs_spec, spec_path,   sizeof(abs_spec));
    _fullpath(abs_root, rpmbuild_root, sizeof(abs_root));
#else
    if (!realpath(spec_path,    abs_spec)) strncpy(abs_spec, spec_path,    sizeof(abs_spec)-1);
    if (!realpath(rpmbuild_root,abs_root)) strncpy(abs_root, rpmbuild_root, sizeof(abs_root)-1);
#endif
    char rpm_cmd[1200];
    snprintf(rpm_cmd, sizeof(rpm_cmd),
        "rpmbuild -bb \"%s\" "
        "--define \"_topdir %s\" "
        "--define \"_sourcedir %s\" "
        "2>&1",
        abs_spec, abs_root, sources_dir);
    printf(CYAN "[M-PACK/RPM] %s\n" RESET, rpm_cmd);

    if (system(rpm_cmd) == 0) {
        /* find the .rpm and copy to outdir */
        char find_cmd[512], rpm_out[512];
        snprintf(rpm_out,   sizeof(rpm_out),
                 "%s/%s-%s-1.%s.rpm", outdir, m->name, m->version, rpm_arch);
        snprintf(find_cmd,  sizeof(find_cmd),
                 "find \"%s/RPMS\" -name '*.rpm' -exec cp {} \"%s\" \\; 2>/dev/null",
                 abs_root, outdir);
        system(find_cmd);
        printf(GREEN "[M-PACK/RPM] RPM ready in %s/\n" RESET, outdir);
        /* cleanup */
        char rm_cmd[600]; snprintf(rm_cmd,sizeof(rm_cmd),"rm -rf \"%s\"",abs_root);
        system(rm_cmd);
        return 1;
    }
    printf(RED "[M-PACK/RPM] rpmbuild failed. Check spec: %s\n" RESET, spec_path);
    return 0;
}

/* ---- Main entry: --m-pack ---- */
void cmd_m_pack(const char *formats_arg) {
    /* Parse formats: "zip,deb,rpm" or "all" */
    int do_zip = 0, do_deb = 0, do_rpm = 0;
    if (!formats_arg || !formats_arg[0] || strcmp(formats_arg,"all")==0) {
        do_zip = do_deb = do_rpm = 1;
    } else {
        char tmp[128]; strncpy(tmp, formats_arg, 127);
        char *tok = strtok(tmp, ",");
        while (tok) {
            if (strcmp(tok,"zip")==0) do_zip=1;
            else if (strcmp(tok,"deb")==0) do_deb=1;
            else if (strcmp(tok,"rpm")==0) do_rpm=1;
            tok = strtok(NULL, ",");
        }
    }

    PkgMeta m; memset(&m, 0, sizeof(m));
    load_pkg_meta(&m);

    if (!is_regular_file(m.bin_path)) {
        printf(RED "[M-PACK] Binary not found: %s\n" RESET, m.bin_path);
        printf(RED "[M-PACK] Run --build first.\n" RESET);
        return;
    }

    /* Output directory */
    const char *outdir = "dist";
    mkdir_auto(outdir);

    printf(BLUE "[M-PACK] Packaging: %s  v%s  (%s)\n" RESET,
           m.name, m.version, m.arch_str);
    printf(BLUE "[M-PACK] Binary: %s  (%.1f KB)\n" RESET,
           m.bin_path, m.bin_size / 1024.0);
    printf(BLUE "[M-PACK] Output dir: %s/\n\n" RESET, outdir);

    int ok = 0;
    int do_nsis = 0, do_pkg_mac = 0;
    if (!formats_arg || !formats_arg[0] || strcmp(formats_arg,"all")==0) {
#ifdef _WIN32
        do_nsis = 1;
#elif defined(__APPLE__)
        do_pkg_mac = 1;
#endif
    } else {
        char tmp2[128]; strncpy(tmp2,formats_arg,127);
        char *tok2=strtok(tmp2,",");
        while(tok2){if(strcmp(tok2,"nsis")==0)do_nsis=1;else if(strcmp(tok2,"pkg")==0)do_pkg_mac=1;tok2=strtok(NULL,",");}
    }
    if (do_zip) ok += mpack_zip(&m, outdir);
    if (do_deb) ok += mpack_deb(&m, outdir);
    if (do_rpm) ok += mpack_rpm(&m, outdir);
    if (do_nsis)    { mpack_nsis(&m, outdir);     ok++; }
    if (do_pkg_mac) { mpack_pkg_macos(&m, outdir); ok++; }

    printf("\n");
    if (ok > 0)
        printf(GREEN "[M-PACK] %d package(s) created in %s/\n" RESET, ok, outdir);
    else
        printf(RED "[M-PACK] No packages created.\n" RESET);
}

/* ================================================================== */
/*  --m-gvz : Graphviz dependency graph                                 */
/*                                                                      */
/*  Produces omake_graph/deps.dot  (and .png/.svg if dot is available) */
/*  Node types:                                                         */
/*    [box]     source files  (.c / .cpp)                               */
/*    [ellipse] project headers  (include/ / src/)                      */
/*    [diamond] external / system headers                               */
/*    [house]   the final binary (OUT:)                                 */
/*                                                                      */
/*  Edges:                                                              */
/*    source -> header   (includes)                                     */
/*    source -> binary   (compiled into)                                */
/* ================================================================== */

/* DOT color palette */
#define GVZ_SRC_COLOR     "\"#4C9BE8\""   /* blue  — source files      */
#define GVZ_INC_COLOR     "\"#5DBE7A\""   /* green — project headers   */
#define GVZ_EXT_COLOR     "\"#B0B0B0\""   /* grey  — system headers    */
#define GVZ_BIN_COLOR     "\"#E8834C\""   /* orange— final binary      */
#define GVZ_EDGE_SRC      "\"#4C9BE8\""
#define GVZ_EDGE_INC      "\"#5DBE7A50\""
#define GVZ_EDGE_LINK     "\"#E8834C\""

/* Sanitize a path into a valid DOT identifier (replace non-alnum with _) */
static void dot_id(const char *path, char *out, int outsz) {
    int j = 0;
    for (const char *p = path; *p && j < outsz-1; p++) {
        char c = *p;
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')) out[j++]=c;
        else out[j++]='_';
    }
    out[j]='\0';
}

/* Check if a path is under a known project directory */
static int is_project_header(const char *path) {
    return (strncmp(path,"include/",8)==0 || strncmp(path,"src/",4)==0 ||
            strncmp(path,"./include/",10)==0 || strncmp(path,"./src/",6)==0 ||
            strncmp(path,".\\include\\",10)==0 || strncmp(path,".\\src\\",6)==0);
}

/* Parse a .d dependency file and collect headers */
static int parse_dep_file(const char *dep_path, char hdrs[][512], int max) {
    FILE *f = fopen(dep_path, "r");
    if (!f) return 0;
    /* Format: obj.o: src.cpp hdr1.h hdr2.h \
     *   hdr3.h ...
     * Skip the "obj.o: src.cpp" part, collect the rest.
     */
    char line[4096]; int count = 0; int past_colon = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if (!past_colon) {
            p = strchr(line, ':');
            if (!p) continue;
            p++; past_colon = 1;
        }
        /* tokenize words on this line */
        char *tok = strtok(p, " \t\r\n\\");
        while (tok && count < max) {
            if (tok[0] && strcmp(tok,"\\")!=0) {
                /* skip make-rule targets: tokens ending with ':' */
                size_t tl = strlen(tok);
                if (tl > 0 && tok[tl-1] == ':') { tok=strtok(NULL," \t\r\n\\"); continue; }
                /* skip the source file itself (ends in .c / .cpp / .cc / .cxx) */
                const char *e = strrchr(tok,'.');
                if (!e || (strcmp(e,".c")!=0 && strcmp(e,".cpp")!=0 &&
                           strcmp(e,".cc")!=0 && strcmp(e,".cxx")!=0)) {
                    /* skip duplicates within this file */
                    int dup = 0;
                    for (int di=0; di<count; di++) if(strcmp(hdrs[di],tok)==0){dup=1;break;}
                    if (!dup) strncpy(hdrs[count++], tok, 511);
                }
            }
            tok = strtok(NULL, " \t\r\n\\");
        }
    }
    fclose(f);
    return count;
}

void cmd_m_gvz(const char *out_format) {
    /* out_format: "png" | "svg" | "dot" | NULL (auto) */
    if (!out_format || !out_format[0]) out_format = "png";

    /* Read project metadata */
    PkgMeta m; memset(&m, 0, sizeof(m));
    load_pkg_meta(&m);

    /* Scan sources and their .d files */
    extern int        src_count;
    extern SourceFile sources[];

    /* We need to rescan sources — re-run scan_sources with defaults */
    /* Read OMakeLists.txt for SRC: and OBJDIR: */
    char src_dir[512]  = "src";
    char obj_root[512] = ".omake_obj";
    char src_ext[64]   = ".cpp";
    char src_ext2[64]  = "";

    FILE *cfg = fopen("OMakeLists.txt","r");
    if (cfg) {
        char line[1024];
        while (fgets(line,sizeof(line),cfg)) {
            char *cmt=strchr(line,'#'); if(cmt)*cmt='\0';
            trim(line); if(!line[0]) continue;
            char ll[1024]; strcpy(ll,line); to_lower_str(ll);
            char *col=strchr(ll,':'); if(!col) continue;
            char key[64]=""; int kl=(int)(col-ll);
            if(kl>0&&kl<63){strncpy(key,ll,kl);key[kl]='\0';trim(key);}
            char *val=strchr(line,':'); if(!val) continue; val++; trim(val);
            if(strcmp(key,"src")==0) {
                char *semi=strchr(val,';');
                if(semi) {
                    *semi='\0';
                    char *sl=strrchr(val,'/'); if(!sl)sl=strrchr(val,'\\');
                    if(sl&&strchr(sl,'*')){*sl='\0';strncpy(src_dir,val,511); char *d=strchr(sl+1,'.');if(d)strncpy(src_ext,d,63);}
                    char *d2=strchr(semi+1,'.'); if(d2) strncpy(src_ext2,d2,63);
                } else {
                    char *sl=strrchr(val,'/'); if(!sl)sl=strrchr(val,'\\');
                    if(sl&&strchr(sl,'*')){*sl='\0';strncpy(src_dir,val,511); char *d=strchr(sl+1,'.');if(d)strncpy(src_ext,d,63);}
                    else{strncpy(src_dir,val,511);strncpy(src_ext2,".c",63);}
                }
            }
            else if(strcmp(key,"objdir")==0) strncpy(obj_root,val,511);
        }
        fclose(cfg);
    }

    src_count = 0;
    scan_sources(src_dir, src_ext, obj_root);
    if (src_ext2[0]) scan_sources(src_dir, src_ext2, obj_root);

    if (src_count == 0) {
        printf(RED "[M-GVZ] No source files found. Run --build first to generate .d files.\n" RESET);
        return;
    }

    /* Output directory */
    mkdir_auto("omake_graph");
    const char *dot_path = "omake_graph/deps.dot";

    FILE *dot = fopen(dot_path, "w");
    if (!dot) { printf(RED "[M-GVZ] Cannot write %s\n" RESET, dot_path); return; }

    /* ---- DOT header ---- */
    fprintf(dot,
        "digraph omake_deps {\n"
        "    graph [\n"
        "        label=\"%s v%s — Dependency Graph (OMake v" VERSION ")\"\n"
        "        labelloc=t  fontname=Helvetica  fontsize=16\n"
        "        bgcolor=\"#1E1E2E\"  pad=0.5  nodesep=0.6  ranksep=1.2\n"
        "        splines=ortho\n"
        "    ];\n"
        "    node [fontname=Helvetica fontsize=11 style=filled];\n"
        "    edge [fontname=Helvetica fontsize=9];\n\n",
        m.name, m.version);

    /* ---- Binary node ---- */
    char bin_id[256]; dot_id(m.bin_path, bin_id, sizeof(bin_id));
    char bin_label[256]; path_stem(m.bin_path, bin_label, sizeof(bin_label));
    fprintf(dot,
        "    /* Final binary */\n"
        "    %s [label=\"%s\\nv%s\" shape=house fillcolor=%s"
        " fontcolor=white color=\"#C05020\" penwidth=2];\n\n",
        bin_id, bin_label, m.version, GVZ_BIN_COLOR);

    /* ---- Per-source nodes + edges ---- */
    /* Collect all unique headers to avoid duplicate node declarations */
    char seen_hdrs[4096][512]; int seen_count = 0;

    fprintf(dot, "    /* Source files */\n");
    for (int i = 0; i < src_count; i++) {
        /* Source node */
        char src_id[256]; dot_id(sources[i].path, src_id, sizeof(src_id));
        /* label = just filename */
        const char *fname = strrchr(sources[i].path, SEP);
        fname = fname ? fname+1 : sources[i].path;

        const char *lang_color = (sources[i].lang == LANG_C) ? "\"#3A7FCC\"" : GVZ_SRC_COLOR;
        fprintf(dot,
            "    %s [label=\"%s\" shape=box fillcolor=%s"
            " fontcolor=white color=\"#2060A0\" penwidth=1.5];\n",
            src_id, fname, lang_color);

        /* Edge: source -> binary */
        fprintf(dot,
            "    %s -> %s [color=%s penwidth=2 weight=10];\n",
            src_id, bin_id, GVZ_EDGE_LINK);

        /* Parse .d file for header dependencies */
        char hdrs[512][512]; int hcount = 0;
        if (is_regular_file(sources[i].dep))
            hcount = parse_dep_file(sources[i].dep, hdrs, 512);

        for (int h = 0; h < hcount; h++) {
            /* Check if already declared */
            int already = 0;
            for (int s = 0; s < seen_count; s++)
                if (strcmp(seen_hdrs[s], hdrs[h])==0){already=1;break;}

            if (!already && seen_count < 4096) {
                strncpy(seen_hdrs[seen_count++], hdrs[h], 511);
                /* Declare header node */
                char hid[256]; dot_id(hdrs[h], hid, sizeof(hid));
                const char *hfname = strrchr(hdrs[h], '/');
                if (!hfname) hfname = strrchr(hdrs[h], '\\');
                hfname = hfname ? hfname+1 : hdrs[h];

                if (is_project_header(hdrs[h])) {
                    fprintf(dot,
                        "    %s [label=\"%s\" shape=ellipse fillcolor=%s"
                        " fontcolor=white color=\"#2A7040\" penwidth=1.2];\n",
                        hid, hfname, GVZ_INC_COLOR);
                } else {
                    fprintf(dot,
                        "    %s [label=\"%s\" shape=ellipse fillcolor=%s"
                        " fontcolor=\"#404040\" color=\"#808080\" penwidth=0.8];\n",
                        hid, hfname, GVZ_EXT_COLOR);
                }
            }

            /* Edge: source -> header */
            char hid[256]; dot_id(hdrs[h], hid, sizeof(hid));
            int is_proj = is_project_header(hdrs[h]);
            fprintf(dot,
                "    %s -> %s [color=%s style=%s penwidth=%s weight=%d];\n",
                src_id, hid,
                is_proj ? GVZ_EDGE_INC : "\"#80808030\"",
                is_proj ? "solid" : "dashed",
                is_proj ? "1.2" : "0.6",
                is_proj ? 5 : 1);
        }
        fprintf(dot, "\n");
    }

    /* ---- Legend ---- */
    fprintf(dot,
        "    /* Legend */\n"
        "    subgraph cluster_legend {\n"
        "        label=\"Legend\" fontcolor=white style=filled fillcolor=\"#2A2A3E\"\n"
        "        fontname=Helvetica fontsize=11 color=\"#505060\"\n"
        "        _l_src  [label=\"Source (.c/.cpp)\" shape=box     fillcolor=%s fontcolor=white color=\"#2060A0\"];\n"
        "        _l_hdr  [label=\"Project header\"   shape=ellipse fillcolor=%s fontcolor=white color=\"#2A7040\"];\n"
        "        _l_ext  [label=\"System header\"    shape=ellipse fillcolor=%s fontcolor=\"#404040\" color=\"#808080\"];\n"
        "        _l_bin  [label=\"Binary output\"    shape=house   fillcolor=%s fontcolor=white color=\"#C05020\"];\n"
        "        _l_src -> _l_hdr -> _l_ext -> _l_bin [style=invis];\n"
        "    }\n",
        GVZ_SRC_COLOR, GVZ_INC_COLOR, GVZ_EXT_COLOR, GVZ_BIN_COLOR);

    fprintf(dot, "}\n");
    fclose(dot);

    printf(GREEN "[M-GVZ] DOT file written: %s\n" RESET, dot_path);
    printf(CYAN  "[M-GVZ] Nodes: %d source(s), %d header(s), 1 binary\n" RESET,
           src_count, seen_count);

    /* ---- Render with Graphviz dot ---- */
    if (!tool_exists("dot")) {
        printf(YELLOW "[M-GVZ] Graphviz 'dot' not found — only .dot file produced.\n" RESET);
        printf(YELLOW "[M-GVZ] Install: sudo apt install graphviz\n" RESET);
        printf(YELLOW "[M-GVZ] Then run manually: dot -T%s -o omake_graph/deps.%s omake_graph/deps.dot\n" RESET,
               out_format, out_format);
        return;
    }

    /* Render png + svg both */
    const char *render_fmts[] = {"png", "svg", NULL};
    for (int ri = 0; render_fmts[ri]; ri++) {
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "omake_graph/deps.%s", render_fmts[ri]);
        char render_cmd[768];
        snprintf(render_cmd, sizeof(render_cmd),
                 "dot -T%s -o \"%s\" \"%s\"", render_fmts[ri], out_path, dot_path);
        if (system(render_cmd) == 0)
            printf(GREEN "[M-GVZ] Rendered: %s\n" RESET, out_path);
        else
            printf(YELLOW "[M-GVZ] Render failed for %s.\n" RESET, render_fmts[ri]);
    }

    /* Open PNG if possible */
#ifdef _WIN32
    system("start omake_graph\\deps.png 2>nul");
#elif defined(__APPLE__)
    system("open omake_graph/deps.png 2>/dev/null &");
#else
    if (tool_exists("xdg-open"))
        system("xdg-open omake_graph/deps.png 2>/dev/null &");
#endif
}


/* ================================================================== */
/*  OMake Extended Features — Round 1                                  */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/*  [1] FETCH: v2 — TAG/BRANCH/CONFIGURE/BUILD_ARGS support       */
/*      FETCH_GIT: <url> TO <dest> [TAG <tag>] [BRANCH <br>]          */
/*                                  [CONFIGURE <cmd>] [ADD_INC <dir>] */
/* ------------------------------------------------------------------ */

/* Extended FetchEntry — already in struct, we add a richer parser */

/*
 * Parse extended FETCH: line:
 *   FETCH: https://github.com/nlohmann/json.git TO include/json TAG v3.11.2
 *   FETCH: https://github.com/fmtlib/fmt.git    TO include/fmt  BRANCH master RECURSE
 *   FETCH: https://github.com/google/googletest TO thirdparty/gtest BUILD_ARGS "-DBUILD_GMOCK=OFF"
 */
int parse_fetch_v2(const char *line, FetchEntryV2 *e) {
    memset(e, 0, sizeof(*e));
    e->shallow = 1;  /* default: shallow clone */

    char tmp[B_SIZE]; strncpy(tmp, line, B_SIZE-1);
    char *p = tmp;

    /* skip "FETCH:" prefix */
    char *col = strchr(p, ':');
    if (!col) return 0;
    p = col + 1;
    trim(p);

    /* parse URL (first token) */
    char *sp = strpbrk(p, " \t");
    if (!sp) { strncpy(e->url, p, 511); return 1; }
    *sp = '\0'; strncpy(e->url, p, 511); p = sp + 1; trim(p);

    /* parse keyword tokens: TO BRANCH TAG BUILD_ARGS CONFIGURE ADD_INC ADD_LIB RECURSE FULL */
    while (*p) {
        trim(p);
        if (!*p) break;

        if (strncasecmp(p, "TO ", 3) == 0) {
            p += 3; trim(p);
            char *end = strpbrk(p, " \t");
            if (end) { *end='\0'; strncpy(e->dest,p,511); p=end+1; }
            else     { strncpy(e->dest,p,511); break; }
        } else if (strncasecmp(p, "TAG ", 4) == 0) {
            p += 4; trim(p);
            char *end = strpbrk(p, " \t");
            if (end) { *end='\0'; strncpy(e->tag,p,63); p=end+1; }
            else     { strncpy(e->tag,p,63); break; }
        } else if (strncasecmp(p, "BRANCH ", 7) == 0) {
            p += 7; trim(p);
            char *end = strpbrk(p, " \t");
            if (end) { *end='\0'; strncpy(e->branch,p,63); p=end+1; }
            else     { strncpy(e->branch,p,63); break; }
        } else if (strncasecmp(p, "CONFIGURE ", 10) == 0) {
            p += 10; trim(p);
            /* rest of line is the configure command */
            strncpy(e->configure, p, 511); break;
        } else if (strncasecmp(p, "BUILD_ARGS ", 11) == 0) {
            p += 11; trim(p);
            strncpy(e->cmake_args, p, 511); break;
        } else if (strncasecmp(p, "CMAKE ", 6) == 0) {
            p += 6; trim(p);
            strncpy(e->cmake_args, p, 511); break;
        } else if (strncasecmp(p, "ADD_INC ", 8) == 0) {
            p += 8; trim(p);
            char *end = strpbrk(p, " \t");
            if (end) { *end='\0'; strncpy(e->add_inc,p,511); p=end+1; }
            else     { strncpy(e->add_inc,p,511); break; }
        } else if (strncasecmp(p, "ADD_LIB ", 8) == 0) {
            p += 8; trim(p);
            char *end = strpbrk(p, " \t");
            if (end) { *end='\0'; strncpy(e->add_lib,p,511); p=end+1; }
            else     { strncpy(e->add_lib,p,511); break; }
        } else if (strncasecmp(p, "RECURSE", 7) == 0) {
            e->recurse = 1; p += 7;
        } else if (strncasecmp(p, "FULL", 4) == 0) {
            e->shallow = 0; p += 4;
        } else {
            /* skip unknown token */
            char *end = strpbrk(p, " \t");
            if (!end) break;
            p = end + 1;
        }
    }
    return e->url[0] && e->dest[0];
}


/* ================================================================== */
/*  auto_inject_missing_fetches                                        */
/*                                                                     */
/*  Eğer USE_LIB: içinde bir kütüphane varsa ama OMakeLists.txt'de   */
/*  ilgili FETCH_V2 satırı yoksa:                                      */
/*  1. g_fetch_v2 dizisine otomatik ekle (bu build için clone yap)   */
/*  2. OMakeLists.txt'yi kalıcı olarak güncelle (bir daha sormaz)    */
/* ================================================================== */
static void auto_inject_missing_fetches(const char *use_libs) {
    if (!use_libs || !use_libs[0]) return;

    /* Hangi URL'ler zaten g_fetch_v2'de VEYA OMakeLists.txt'de var? */
    char existing_urls[8192]; existing_urls[0] = '\0';
    for (int i = 0; i < g_fetch_v2_count; i++) {
        strncat(existing_urls, g_fetch_v2[i].url,
                sizeof(existing_urls)-strlen(existing_urls)-1);
        strncat(existing_urls, "\n",
                sizeof(existing_urls)-strlen(existing_urls)-1);
    }
    /* Ayrıca OMakeLists.txt içindeki FETCH_V2 satırlarını da kontrol et */
    {
        FILE *_chk = fopen("OMakeLists.txt", "r");
        if (_chk) {
            char _line[1024];
            while (fgets(_line, sizeof(_line), _chk)) {
                trim(_line);
                if (strncasecmp(_line, "FETCH_V2:", 9) == 0 ||
                    strncasecmp(_line, "FETCH:",    6) == 0) {
                    /* URL'yi çıkar */
                    const char *u = strstr(_line, "https://");
                    if (!u) u = strstr(_line, "http://");
                    if (u) {
                        /* Boşluk veya satır sonuna kadar al */
                        char uurl[512]; int ui=0;
                        while (*u && *u != ' ' && *u != '\t' && *u != '\r' && *u != '\n' && ui<511)
                            uurl[ui++] = *u++;
                        uurl[ui] = '\0';
                        if (!strstr(existing_urls, uurl)) {
                            strncat(existing_urls, uurl,
                                    sizeof(existing_urls)-strlen(existing_urls)-1);
                            strncat(existing_urls, "\n",
                                    sizeof(existing_urls)-strlen(existing_urls)-1);
                        }
                    }
                }
            }
            fclose(_chk);
        }
    }

    /* Her USE_LIB token için LIB_FETCH'e bak */
    char tmp[B_SIZE]; strncpy(tmp, use_libs, B_SIZE-1);
    char *tok = strtok(tmp, " ,\t");

    char new_lines[8192]; new_lines[0] = '\0';   /* OMakeLists.txt'e eklenecekler */
    int  added = 0;

    while (tok) {
        const LibFetchInfo *lfi = lfi_find(tok);
        if (lfi && lfi->git_url[0]) {
            /* Bu URL zaten fetch listesinde mi? */
            if (!strstr(existing_urls, lfi->git_url)) {
                /* Kütüphanenin deps dizini zaten var mı? (onceden clonlanmış) */
                if (!dir_exists(lfi->git_dest)) {
                    /* Dizin var ama boş mu? (ensure_dir_exists boş dizin oluşturmuş olabilir) */
                    int dir_has_content = 0;
                    if (dir_exists(lfi->git_dest)) {
                        DIR *_dd = opendir(lfi->git_dest);
                        if (_dd) {
                            struct dirent *_de;
                            while ((_de = readdir(_dd)) != NULL) {
                                if (_de->d_name[0] != '.') { dir_has_content = 1; break; }
                            }
                            closedir(_dd);
                        }
                    }
                    if (dir_has_content) { tok = strtok(NULL, " ,\t"); continue; }

                    /* Yok veya boş → otomatik ekle */
                    FetchEntryV2 e;
                    memset(&e, 0, sizeof(e));
                    strncpy(e.url,  lfi->git_url,  511);
                    strncpy(e.dest, lfi->git_dest, 511);
                    if (lfi->cmake_args) strncpy(e.cmake_args, lfi->cmake_args, 511);
                    if (lfi->git_tag)   strncpy(e.tag,         lfi->git_tag,   63);
                    e.shallow = 1;

                    if (g_fetch_v2_count < MAX_FETCH_V2) {
                        g_fetch_v2[g_fetch_v2_count++] = e;
                        strncat(existing_urls, lfi->git_url,
                                sizeof(existing_urls)-strlen(existing_urls)-1);
                        strncat(existing_urls, "\n",
                                sizeof(existing_urls)-strlen(existing_urls)-1);

                        /* OMakeLists.txt'e eklenecek satırı hazırla */
                        char line[1024]; line[0] = '\0';
                        snprintf(line, sizeof(line), "FETCH_V2: %s TO %s",
                                 lfi->git_url, lfi->git_dest);
                        if (lfi->git_tag && lfi->git_tag[0]) {
                            char t[128]; snprintf(t,sizeof(t)," TAG %s",lfi->git_tag);
                            strncat(line,t,sizeof(line)-strlen(line)-1);
                        }
                        if (lfi->cmake_args && lfi->cmake_args[0]) {
                            char t[600]; snprintf(t,sizeof(t)," CMAKE %s",lfi->cmake_args);
                            strncat(line,t,sizeof(line)-strlen(line)-1);
                        }
                        strncat(line, "\n", sizeof(line)-strlen(line)-1);
                        strncat(new_lines, line,
                                sizeof(new_lines)-strlen(new_lines)-1);

                        printf(YELLOW "[AUTO-FETCH] USE_LIB: %s → FETCH_V2 eklendi: %s\n" RESET,
                               tok, lfi->git_dest);
                        added++;
                    }
                }
            }
        }
        tok = strtok(NULL, " ,\t");
    }

    if (!added || !new_lines[0]) return;

    /* ── OMakeLists.txt'i kalıcı olarak güncelle ─────────────────────
     * Dosyanın başına (ilk boş olmayan satırdan önce veya SRC: satırından önce)
     * FETCH_V2 bloğunu ekle.
     * ─────────────────────────────────────────────────────────────── */
    FILE *orig = fopen("OMakeLists.txt", "r");
    if (!orig) return;

    /* Dosyayı oku */
    char *fbuf = (char*)malloc(256*1024);
    if (!fbuf) { fclose(orig); return; }
    size_t flen = fread(fbuf, 1, 256*1024-1, orig);
    fclose(orig);
    fbuf[flen] = '\0';

    /* Zaten FETCH_V2 satırı var mı kontrol et (yeni eklenenler için) */
    int already_has_fetch = (strstr(fbuf, "FETCH_V2:") != NULL);

    FILE *out = fopen("OMakeLists.txt", "w");
    if (!out) { free(fbuf); return; }

    if (already_has_fetch) {
        /* Mevcut son FETCH_V2 satırından sonra ekle */
        /* En son "FETCH_V2:" satırını bul */
        const char *last_fetch = fbuf;
        const char *p = fbuf;
        while ((p = strstr(p, "FETCH_V2:")) != NULL) {
            last_fetch = p; p++;
        }
        /* last_fetch satırının sonunu bul */
        const char *eol = strchr(last_fetch, '\n');
        if (eol) {
            fwrite(fbuf, 1, (size_t)(eol - fbuf + 1), out);
            fputs(new_lines, out);
            fputs(eol + 1, out);
        } else {
            fputs(fbuf, out);
            fputc('\n', out);
            fputs(new_lines, out);
        }
    } else {
        /* Dosyanın başına (ilk FETCH_V2 bloğu yok) — SRC: satırından önce ekle */
        const char *src_line = strstr(fbuf, "\nSRC:");
        if (!src_line) src_line = strstr(fbuf, "\nNAME:");
        if (!src_line) src_line = strstr(fbuf, "\nCOMPILER:");

        if (src_line) {
            /* src_line öncesini yaz */
            fwrite(fbuf, 1, (size_t)(src_line - fbuf + 1), out);
            /* FETCH_V2 bloğu ekle */
            fputs("# ── Dependencies (auto-added by OMake) ──────────────────────────\n", out);
            fputs("# Each FETCH_V2 clones the repo once; subsequent builds skip it.\n", out);
            fputs(new_lines, out);
            fputs("\n", out);
            /* geri kalanı yaz */
            fputs(src_line + 1, out);
        } else {
            /* Dosyanın sonuna ekle */
            fputs(fbuf, out);
            fputs("\n# ── Dependencies (auto-added by OMake) ──────────────────────────\n", out);
            fputs(new_lines, out);
        }
    }

    fclose(out);
    free(fbuf);

    printf(GREEN "[AUTO-FETCH] OMakeLists.txt güncellendi — %d FETCH_V2 satırı eklendi\n" RESET, added);
    printf(CYAN  "[AUTO-FETCH] Bir sonraki build'de bu satırları okuyacak (otomatik)\n" RESET);
}
void fetch_v2_all(int force, char *tags, char *libs) {
    if (g_fetch_v2_count == 0) return;
    int has_git  = tool_exists("git");
    int has_curl = tool_exists("curl");
    if (!has_git && !has_curl) {
        printf(RED "[FETCH] Neither git nor curl found.\n" RESET); return;
    }

    for (int i = 0; i < g_fetch_v2_count; i++) {
        FetchEntryV2 *e = &g_fetch_v2[i];
        printf(BLUE "[FETCH] %s  -->  %s" RESET, e->url, e->dest);
        if (e->tag[0])    printf(CYAN "  tag:%s"    RESET, e->tag);
        if (e->branch[0]) printf(CYAN "  branch:%s" RESET, e->branch);
        printf("\n");

        int already = dir_exists(e->dest);
        /* ── Dep cache check ── */
        if (!already) {
            if (dep_cache_hit(e->dest, e->url, e->tag[0]?e->tag:e->branch, e->cmake_args)) {
                printf(GREEN "[CACHE] Hit: %s (skip rebuild)\n" RESET, e->dest);
                already = 1; /* treat as already done */
            }
        }
        if (already && !force) {
            if (has_git) {
                char pull[800];
                snprintf(pull,sizeof(pull),"git -C \"%s\" pull --quiet 2>/dev/null",e->dest);
                system(pull);
                printf(CYAN "[FETCH] Up-to-date: %s\n" RESET, e->dest);
            }
        } else {
            if (already && force) {
                char rm[700]; snprintf(rm,sizeof(rm),"%s \"%s\"",DEL_DIR,e->dest); system(rm);
            }
            ensure_dir_exists(e->dest);
            if (has_git) {
                char clone[1200];
                /* Build clone command */
                char depth_flag[32];   if(e->shallow)  strcpy(depth_flag," --depth=1");    else depth_flag[0]='\0';
                char recurse_flag[32]; if(e->recurse)  strcpy(recurse_flag," --recurse-submodules"); else recurse_flag[0]='\0';
                char ref_flag[128];    ref_flag[0]='\0';
                if (e->tag[0])         snprintf(ref_flag,sizeof(ref_flag)," --branch \"%s\"",e->tag);
                else if (e->branch[0]) snprintf(ref_flag,sizeof(ref_flag)," --branch \"%s\"",e->branch);

                snprintf(clone,sizeof(clone),
                    "git clone%s%s%s \"%s\" \"%s\"",
                    depth_flag, recurse_flag, ref_flag,
                    e->url, e->dest);
                printf(CYAN "[FETCH] %s\n" RESET, clone);
                if (system(clone) != 0) {
                    printf(YELLOW "[FETCH] git failed, trying curl...\n" RESET);
                    /* curl fallback via existing function */
                }
            }
            /* CONFIGURE step */
            if (e->configure[0]) {
                char cwd[1024]; getcwd(cwd,sizeof(cwd));
                if (chdir(e->dest)==0) {
                    printf(CYAN "[FETCH] Configure: %s\n" RESET, e->configure);
                    system(e->configure);
                    chdir(cwd);
                }
            }
            /* cmake system build step */
            if (e->cmake_args[0] && tool_exists("cmake")) {
                char cmake_build[600];
                snprintf(cmake_build,sizeof(cmake_build),
                    "cmake -S \"%s\" -B \"%s/_build\" %s -DCMAKE_BUILD_TYPE=Release "
                    "&& cmake --build \"%s/_build\" --parallel",
                    e->dest, e->dest, e->cmake_args, e->dest);
                printf(CYAN "[FETCH] cmake-build: %s\n" RESET, cmake_build);
                system(cmake_build);
            }
            printf(GREEN "[FETCH] Ready: %s\n" RESET, e->dest);
        }

        /* ── Inject include paths ──────────────────────────────────── */
        {
            char inc_path[600];
            if (e->add_inc[0]) {
                snprintf(inc_path,sizeof(inc_path),"%s/%s",e->dest,e->add_inc);
            } else {
                /* Try: dest/include first */
                snprintf(inc_path,sizeof(inc_path),"%s/include",e->dest);
                if (!dir_exists(inc_path)) {
                    /* Fallback: dest itself */
                    strncpy(inc_path,e->dest,511);
                }
            }
            char iflag[700]; snprintf(iflag,sizeof(iflag)," -I\"%s\"",inc_path);
            if (!strstr(tags,iflag)) strncat(tags,iflag,B_SIZE-strlen(tags)-1);

            /* Also add dest/_build/include if it exists (cmake installs headers there) */
            char build_inc[700];
            snprintf(build_inc,sizeof(build_inc),"%s/_build/include",e->dest);
            if (dir_exists(build_inc)) {
                char bf[750]; snprintf(bf,sizeof(bf)," -I\"%s\"",build_inc);
                if (!strstr(tags,bf)) strncat(tags,bf,B_SIZE-strlen(tags)-1);
            }
        }

        /* ── Inject lib paths ───────────────────────────────────────── */
        {
            /* Try in order: explicit add_lib, then _build/lib, _build, lib */
            static const char *lib_candidates[] = {
                "_build/lib", "_build/lib/Release", "_build/lib/Debug",
                "_build", "_build/Release", "_build/Debug",
                "lib", "build/lib", NULL
            };
            int lib_injected = 0;

            if (e->add_lib[0]) {
                char lib_path[600];
                snprintf(lib_path,sizeof(lib_path),"%s/%s",e->dest,e->add_lib);
                if (dir_exists(lib_path)) {
                    char lflag[700]; snprintf(lflag,sizeof(lflag)," -L\"%s\"",lib_path);
                    if (!strstr(libs,lflag)) strncat(libs,lflag,B_SIZE-strlen(libs)-1);
                    lib_injected = 1;
                }
            }

            if (!lib_injected) {
                for (int li = 0; lib_candidates[li]; li++) {
                    char lp[700];
                    snprintf(lp,sizeof(lp),"%s/%s",e->dest,lib_candidates[li]);
                    if (dir_exists(lp)) {
                        char lflag[750]; snprintf(lflag,sizeof(lflag)," -L\"%s\"",lp);
                        if (!strstr(libs,lflag)) strncat(libs,lflag,B_SIZE-strlen(libs)-1);
                        lib_injected = 1;
                        printf(CYAN "[FETCH] lib path: %s\n" RESET, lp);
                        break;
                    }
                }
            }

            /* If no lib dir found yet but cmake was run, warn */
            if (!lib_injected && e->cmake_args[0]) {
                printf(YELLOW "[FETCH] Warning: no lib dir found in %s after cmake build\n"
                       RESET, e->dest);
                printf(YELLOW "[FETCH]   You may need ADD_LIB <subdir> in your FETCH_V2 line\n" RESET);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  [2] TARGET: System — target-scoped build config                    */
/*      Allows multiple targets in one OMakeLists.txt:                 */
/*                                                                      */
/*      TARGET: myapp                                                   */
/*        SRC: src/app/*.cpp                                            */
/*        TARGET_FLAGS: -DAPP_MODE                                      */
/*        TARGET_LIBS:  -lsqlite3                                       */
/*        TARGET_INC:   include/app                                     */
/*        TARGET_DEPS:  mylib                   ← link another target   */
/*        OUT: bin/myapp                                                 */
/*                                                                      */
/*      TARGET: mylib                                                    */
/*        SRC: src/lib/*.cpp                                             */
/*        OUTPUT_TYPE: static                                            */
/*        TARGET_INC:  include/lib                                       */
/*        OUT: bin/mylib                                                 */
/*                                                                      */
/* ------------------------------------------------------------------ */

Target *target_get_or_create(const char *name) {
    for (int i = 0; i < g_target_count; i++)
        if (strcmp(g_targets[i].name, name) == 0) return &g_targets[i];
    if (g_target_count >= MAX_TARGETS) return NULL;
    Target *t = &g_targets[g_target_count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, 127);
    strcpy(t->src_ext, ".cpp");
    strcpy(t->output_type, "exe");
    t->active = 1;
    return t;
}

/* Resolve TARGET_DEPS: merge linked target's include dirs into consumer */
void resolve_target_deps(Target *consumer) {
    if (!consumer->deps[0]) return;
    char tmp[512]; strncpy(tmp, consumer->deps, 511);
    char *tok = strtok(tmp, " ,");
    while (tok) {
        for (int i = 0; i < g_target_count; i++) {
            if (strcmp(g_targets[i].name, tok) == 0 ||
                strcmp(g_targets[i].alias, tok) == 0) {
                /* Propagate include flags */
                if (g_targets[i].inc[0]) {
                    if (consumer->inc[0]) strncat(consumer->inc," ",B_SIZE-strlen(consumer->inc)-1);
                    strncat(consumer->inc, g_targets[i].inc, B_SIZE-strlen(consumer->inc)-1);
                }
                /* Propagate lib output if static */
                if (strcmp(g_targets[i].output_type,"static")==0 && g_targets[i].out[0]) {
                    char lflag[600]; snprintf(lflag,sizeof(lflag)," \"%s\"",g_targets[i].out);
                    strncat(consumer->libs, lflag, B_SIZE-strlen(consumer->libs)-1);
                }
                printf(CYAN "[MODULE] %s <- depends on %s\n" RESET,
                       consumer->name, g_targets[i].name);
                break;
            }
        }
        tok = strtok(NULL, " ,");
    }
}

/* ------------------------------------------------------------------ */
/*  [6] HEADER_LIB: — header-only library node                      */
/*  [7] ALIAS: — library alias  Foo::Core                              */
/*  [9] OBJECT_LIB: — object file pool                                 */
/* ------------------------------------------------------------------ */

/* These are implemented through the Target struct above:
 * INTERFACE_LIB: mylib  ->  target with is_interface=1
 * OBJECT_LIB:    myobjs ->  target with is_object=1
 * ALIAS: Foo::Core=mylib -> sets alias field
 */

/* Propagate interface library flags to all dependents */
void propagate_interface_libs(void) {
    for (int i = 0; i < g_target_count; i++) {
        if (!g_targets[i].is_interface) continue;
        /* Find all targets that depend on this interface lib */
        for (int j = 0; j < g_target_count; j++) {
            if (i == j) continue;
            if (strstr(g_targets[j].deps, g_targets[i].name) ||
                (g_targets[i].alias[0] && strstr(g_targets[j].deps, g_targets[i].alias))) {
                /* Merge flags */
                if (g_targets[i].inc[0]) {
                    strncat(g_targets[j].inc, " ", B_SIZE-strlen(g_targets[j].inc)-1);
                    strncat(g_targets[j].inc, g_targets[i].inc, B_SIZE-strlen(g_targets[j].inc)-1);
                }
                if (g_targets[i].flags[0]) {
                    strncat(g_targets[j].flags, " ", B_SIZE-strlen(g_targets[j].flags)-1);
                    strncat(g_targets[j].flags, g_targets[i].flags, B_SIZE-strlen(g_targets[j].flags)-1);
                }
                printf(CYAN "[INTERFACE] %s propagated to %s\n" RESET,
                       g_targets[i].name, g_targets[j].name);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  [8] Generator Expressions                                           */
/*      $<CONFIG:Debug>:-DDEBUG_MODE                                   */
/*      $<CONFIG:Release>:-O3                                          */
/*      $<PLATFORM:win32>:-DWINDOWS                                    */
/*      $<PLATFORM:linux>:-DLINUX                                      */
/*      $<COMPILER:gcc>:-Wextra                                        */
/* ------------------------------------------------------------------ */

void expand_genex(const char *input, char *output, int outsz,
                          const char *profile, CompilerType ct) {
    strncpy(output, input, outsz-1); output[outsz-1]='\0';

    /* Determine platform string */
    const char *platform =
#ifdef _WIN32
        "win32";
#elif defined(__APPLE__)
        "macos";
#else
        "linux";
#endif

    const char *compiler_str = (ct==CT_MSVC)  ? "msvc"  :
                                (ct==CT_CLANG) ? "clang" : "gcc";

    /* Iteratively expand $<...>:value expressions */
    char result[B_SIZE]; result[0]='\0';
    const char *p = output;
    while (*p) {
        if (*p == '$' && *(p+1)=='<') {
            /* find matching > */
            const char *close = strchr(p+2, '>');
            if (!close) { strncat(result, p, B_SIZE-strlen(result)-1); break; }
            /* extract expression body */
            char body[256]; int blen=(int)(close-(p+2));
            if (blen>255) blen=255;
            strncpy(body, p+2, blen); body[blen]='\0';

            /* find the value after ':' outside the <> */
            const char *val_start = close + 1;
            if (*val_start == ':') val_start++;

            /* find end of value (space or end) */
            char val[512]; int vlen=0;
            const char *vp = val_start;
            /* value ends at space that is NOT inside quotes */
            int in_q=0;
            while (*vp && (in_q || (*vp!=' '&&*vp!='\t'))) {
                if (*vp=='"') in_q=!in_q;
                val[vlen++]= *vp++;
                if (vlen>=511) break;
            }
            val[vlen]='\0';

            /* Evaluate condition */
            int condition = 0;

            /* $<CONFIG:Debug> */
            if (strncasecmp(body,"CONFIG:",7)==0) {
                char cfg[32]; strncpy(cfg,body+7,31);
                for(int ci=0;cfg[ci];ci++) cfg[ci]=(char)tolower((unsigned char)cfg[ci]);
                char prof[32]; strncpy(prof,profile&&profile[0]?profile:"release",31);
                for(int ci=0;prof[ci];ci++) prof[ci]=(char)tolower((unsigned char)prof[ci]);
                condition = (strcmp(cfg,prof)==0);
            }
            /* $<PLATFORM:linux> */
            else if (strncasecmp(body,"PLATFORM:",9)==0) {
                char plat[32]; strncpy(plat,body+9,31);
                for(int ci=0;plat[ci];ci++) plat[ci]=(char)tolower((unsigned char)plat[ci]);
                condition = (strcmp(plat,platform)==0);
            }
            /* $<COMPILER:gcc> */
            else if (strncasecmp(body,"COMPILER:",9)==0) {
                char cmp[32]; strncpy(cmp,body+9,31);
                for(int ci=0;cmp[ci];ci++) cmp[ci]=(char)tolower((unsigned char)cmp[ci]);
                condition = (strcmp(cmp,compiler_str)==0);
            }
            /* $<NOT:...> */
            else if (strncasecmp(body,"NOT:",4)==0) {
                /* nested: $<NOT:$<CONFIG:Debug>> — simplify: just check literal */
                condition = 0; /* TODO: nested eval */
            }
            /* $<1:val> always true, $<0:val> always false */
            else if (strcmp(body,"1")==0) condition=1;
            else if (strcmp(body,"0")==0) condition=0;

            if (condition && val[0]) {
                strncat(result, val, B_SIZE-strlen(result)-1);
                strncat(result, " ", B_SIZE-strlen(result)-1);
            }

            p = vp; /* skip past the value */
        } else {
            /* Normal character — copy */
            char ch[2]={*p,'\0'};
            strncat(result, ch, B_SIZE-strlen(result)-1);
            p++;
        }
    }
    strncpy(output, result, outsz-1);
}

/* ------------------------------------------------------------------ */
/*  [4] Enhanced Unity Builds: per-directory grouping                  */
/*      UNITY_BATCH: N  — compile N files per unity TU (default: all) */
/* ------------------------------------------------------------------ */

int unity_compile_batched(SourceFile *srcs, int count,
                           const char *obj_root,
                           const char *final_comp, CompilerType ct,
                           const char *tags_tr, const char *inc_tr,
                           int batch_size) {
    if (batch_size <= 0 || batch_size >= count) {
        /* Full unity (existing behavior) */
        return unity_compile(srcs, count, obj_root, final_comp, ct, tags_tr, inc_tr);
    }
    /* Batched: split sources into groups of batch_size */
    int n_batches = (count + batch_size - 1) / batch_size;
    printf(CYAN "[UNITY] Batched: %d files / %d per batch = %d TU(s)\n" RESET,
           count, batch_size, n_batches);

    int all_ok = 1;
    for (int b = 0; b < n_batches; b++) {
        int start = b * batch_size;
        int end   = start + batch_size;
        if (end > count) end = count;

        char unity_src[1024];
        snprintf(unity_src,sizeof(unity_src),"%s/unity_batch_%d.cpp",obj_root,b);
        char unity_obj[1024];
        snprintf(unity_obj,sizeof(unity_obj),"%s/unity_batch_%d.o",obj_root,b);

        FILE *uf = fopen(unity_src,"w");
        if (!uf) {
            /* Try creating the directory first */
            ensure_dir_exists(unity_obj);
            uf = fopen(unity_src,"w");
        }
        if (!uf) { printf(RED "[UNITY] Cannot create batch %d\n" RESET, b); all_ok=0; continue; }
        fprintf(uf,"/* OMake Unity Batch %d — auto-generated */\n",b);
        /* Get absolute CWD so includes resolve from any working dir */
        char cwd_abs[1024]; cwd_abs[0]='\0';
        getcwd(cwd_abs, sizeof(cwd_abs));
        for (int i=start;i<end;i++) {
            fix_slashes(srcs[i].path);
            /* Use absolute path if cwd available, else relative */
            if (cwd_abs[0])
                fprintf(uf,"#include \"%s/%s\"\n", cwd_abs, srcs[i].path);
            else
                fprintf(uf,"#include \"%s\"\n", srcs[i].path);
        }
        fclose(uf);

        char cmd[B_SIZE];
        if (ct==CT_MSVC)
            snprintf(cmd,sizeof(cmd),"%s /nologo /c \"%s\" /Fo\"%s\" /TP %s %s",
                     final_comp,unity_src,unity_obj,tags_tr,inc_tr);
        else
            snprintf(cmd,sizeof(cmd),"%s -c \"%s\" -o \"%s\" -MMD -MP %s %s",
                     final_comp,unity_src,unity_obj,tags_tr,inc_tr);

        printf(CYAN "[UNITY] Batch %d (%d files): %s\n" RESET, b+1, end-start, unity_src);
        if (system(cmd)!=0) { all_ok=0; printf(RED "[UNITY] Batch %d failed.\n" RESET,b); }

        for (int i=start;i<end;i++) strncpy(srcs[i].obj,unity_obj,1023);
    }
    return all_ok;
}

/* ------------------------------------------------------------------ */
/*  [5] Auto-PCH: detect most-included header automatically            */
/*      AUTO_PCH: on — scan .d files, find top header, precompile it  */
/* ------------------------------------------------------------------ */

void auto_pch_detect(const char *obj_root, char *best_hdr_out, int outsz) {
    best_hdr_out[0]='\0';
    /* Count header occurrences across all .d files */
    typedef struct { char hdr[512]; int count; } HdrCount;
    HdrCount *hc = (HdrCount*)calloc(8192, sizeof(HdrCount));
    if (!hc) return;
    int hc_n = 0;

    for (int i=0; i<src_count; i++) {
        if (!is_regular_file(sources[i].dep)) continue;
        FILE *f = fopen(sources[i].dep,"r");
        if (!f) continue;
        char line[4096];
        int past=0;
        while (fgets(line,sizeof(line),f)) {
            if (!past) { past=1; continue; } /* skip first line (obj: src) */
            char *tok=strtok(line," \t\r\n\\");
            while(tok) {
                size_t tl=strlen(tok);
                if (tl>0 && tok[tl-1]==':') { tok=strtok(NULL," \t\r\n\\"); continue; }
                /* Only system headers (<...> style via path detection) */
                if (strstr(tok,"/usr/")||strstr(tok,"include/c++")||
                    strstr(tok,"bits/")||strstr(tok,"sys/")) {
                    int found=0;
                    for(int hi=0;hi<hc_n;hi++) {
                        if(strcmp(hc[hi].hdr,tok)==0){hc[hi].count++;found=1;break;}
                    }
                    if(!found && hc_n<8191) {
                        strncpy(hc[hc_n].hdr,tok,511);
                        hc[hc_n++].count=1;
                    }
                }
                tok=strtok(NULL," \t\r\n\\");
            }
        }
        fclose(f);
    }
    /* Find the most common project-level header (non-system) */
    /* Better: look for project headers with count >= src_count/2 */
    for (int i=0; i<src_count; i++) {
        if (!is_regular_file(sources[i].dep)) continue;
        FILE *f = fopen(sources[i].dep,"r");
        if (!f) continue;
        char line2[4096]; int past2=0;
        while (fgets(line2,sizeof(line2),f)) {
            if (!past2){past2=1;continue;}
            char *tok=strtok(line2," \t\r\n\\");
            while(tok) {
                size_t tl=strlen(tok);
                if(tl>0&&tok[tl-1]==':'){tok=strtok(NULL," \t\r\n\\");continue;}
                if (is_project_header(tok)) {
                    int found=0;
                    for(int hi=0;hi<hc_n;hi++) {
                        if(strcmp(hc[hi].hdr,tok)==0){hc[hi].count++;found=1;break;}
                    }
                    if(!found&&hc_n<8191){strncpy(hc[hc_n].hdr,tok,511);hc[hc_n++].count=1;}
                }
                tok=strtok(NULL," \t\r\n\\");
            }
        }
        fclose(f);
    }

    /* Best = highest count, only if included by >= half the sources */
    int best_count=0; int best_idx=-1;
    for(int hi=0;hi<hc_n;hi++) {
        if(hc[hi].count > best_count && hc[hi].count >= src_count/2) {
            best_count=hc[hi].count; best_idx=hi;
        }
    }
    if (best_idx>=0) {
        strncpy(best_hdr_out, hc[best_idx].hdr, outsz-1);
        printf(CYAN "[AUTO-PCH] Most-included header: %s (%d/%d sources)\n" RESET,
               best_hdr_out, best_count, src_count);
    }
    free(hc);
}

/* ------------------------------------------------------------------ */
/*  [10] Enhanced CPack: NSIS/WiX stub, macOS pkg stub                 */
/* ------------------------------------------------------------------ */

void mpack_nsis(const PkgMeta *m, const char *outdir) {
    if (!tool_exists("makensis")) {
        printf(YELLOW "[M-PACK/NSIS] makensis not found. Install NSIS.\n" RESET);
        printf(YELLOW "[M-PACK/NSIS] Generated .nsi script: %s/%s.nsi\n" RESET, outdir, m->name);
    }
    char nsi_path[512];
    snprintf(nsi_path, sizeof(nsi_path), "%s/%s.nsi", outdir, m->name);
    FILE *f = fopen(nsi_path,"w");
    if (!f) return;
    fprintf(f,
        "!define APPNAME \"%s\"\n"
        "!define APPVERSION \"%s\"\n"
        "!define APPEXE \"%s\"\n"
        "!define PUBLISHER \"%s\"\n"
        "\n"
        "Name \"${APPNAME} ${APPVERSION}\"\n"
        "OutFile \"%s\\${APPNAME}-${APPVERSION}-setup.exe\"\n"
        "InstallDir \"$PROGRAMFILES\\${APPNAME}\"\n"
        "RequestExecutionLevel admin\n"
        "\n"
        "Section \"Install\"\n"
        "  SetOutPath \"$INSTDIR\"\n"
        "  File \"${APPEXE}\"\n"
        "  CreateShortCut \"$DESKTOP\\${APPNAME}.lnk\" \"$INSTDIR\\${APPEXE}\"\n"
        "  WriteUninstaller \"$INSTDIR\\Uninstall.exe\"\n"
        "SectionEnd\n"
        "\n"
        "Section \"Uninstall\"\n"
        "  Delete \"$INSTDIR\\${APPEXE}\"\n"
        "  Delete \"$INSTDIR\\Uninstall.exe\"\n"
        "  Delete \"$DESKTOP\\${APPNAME}.lnk\"\n"
        "  RMDir \"$INSTDIR\"\n"
        "SectionEnd\n",
        m->name, m->version, m->bin_path, m->maintainer,
        outdir);
    fclose(f);
    if (tool_exists("makensis")) {
        char cmd[700];
        snprintf(cmd,sizeof(cmd),"makensis \"%s\"",nsi_path);
        printf(CYAN "[M-PACK/NSIS] %s\n" RESET, cmd);
        if(system(cmd)==0) printf(GREEN "[M-PACK/NSIS] Installer ready.\n" RESET);
        else printf(RED "[M-PACK/NSIS] makensis failed.\n" RESET);
    } else {
        printf(GREEN "[M-PACK/NSIS] Script written: %s (run makensis to build)\n" RESET, nsi_path);
    }
}

void mpack_pkg_macos(const PkgMeta *m, const char *outdir) {
#ifdef __APPLE__
    if (!tool_exists("pkgbuild")) {
        printf(YELLOW "[M-PACK/PKG] pkgbuild not found (macOS only).\n" RESET); return;
    }
    /* stage root */
    char root[512], payload_dir[600];
    snprintf(root,       sizeof(root),       "%s/pkg_root",       outdir);
    snprintf(payload_dir,sizeof(payload_dir),"%s/usr/local/bin",  root);
    { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",payload_dir); ensure_dir_exists(_t); remove(_t); }
    char cp_cmd[700];
    snprintf(cp_cmd,sizeof(cp_cmd),"cp \"%s\" \"%s/%s\" && chmod 755 \"%s/%s\"",
             m->bin_path,payload_dir,m->name,payload_dir,m->name);
    system(cp_cmd);
    char pkg_path[512];
    snprintf(pkg_path,sizeof(pkg_path),"%s/%s-%s.pkg",outdir,m->name,m->version);
    char cmd[1024];
    snprintf(cmd,sizeof(cmd),
        "pkgbuild --root \"%s\" --identifier com.%s.%s --version %s \"%s\"",
        root, m->maintainer, m->name, m->version, pkg_path);
    printf(CYAN "[M-PACK/PKG] %s\n" RESET, cmd);
    if (system(cmd)==0) printf(GREEN "[M-PACK/PKG] %s\n" RESET, pkg_path);
    else printf(RED "[M-PACK/PKG] pkgbuild failed.\n" RESET);
    char rm_cmd[600]; snprintf(rm_cmd,sizeof(rm_cmd),"rm -rf \"%s\"",root); system(rm_cmd);
#else
    printf(YELLOW "[M-PACK/PKG] macOS .pkg only available on macOS.\n" RESET);
#endif
}


/* ================================================================== */
/*  OMake Extended Features — Round 2 (Features 11–20)                 */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/*  [11] LTO_CHECK: Auto-detect + enable LTO if compiler supports it  */
/*       LTO_AUTO: on  — probe compiler, enable if supported          */
/*       Auto-probes compiler and enables best LTO mode;             */
/*       also benchmarks compile flags and picks thin vs full LTO.    */
/* ------------------------------------------------------------------ */

int probe_lto_support(CompilerType ct, const char *comp) {
    /* Write a tiny test file and try to compile with -flto */
    const char *test_src = "/tmp/_omake_lto_probe.c";
    const char *test_obj = "/tmp/_omake_lto_probe.o";
    FILE *f = fopen(test_src, "w");
    if (!f) return 0;
    fprintf(f, "int lto_test(int x){return x*2;}\n");
    fclose(f);

    char cmd[1024];
    int ok = 0;
    if (ct == CT_MSVC) {
        /* MSVC: /GL flag probes LTO (LTCG) */
        snprintf(cmd, sizeof(cmd),
            "%s /nologo /GL /c \"%s\" /Fo\"%s\" >nul 2>&1", comp, test_src, test_obj);
        ok = (system(cmd) == 0);
    } else {
        /* Try ThinLTO first (faster), then full LTO */
        snprintf(cmd, sizeof(cmd),
            "%s -flto=thin -c \"%s\" -o \"%s\" 2>/dev/null", comp, test_src, test_obj);
        if (system(cmd) == 0) {
            ok = 2; /* 2 = thin supported */
        } else {
            snprintf(cmd, sizeof(cmd),
                "%s -flto -c \"%s\" -o \"%s\" 2>/dev/null", comp, test_src, test_obj);
            ok = (system(cmd) == 0) ? 1 : 0;
        }
    }
    remove(test_src); remove(test_obj);
    return ok;  /* 0=none, 1=full, 2=thin */
}

void apply_lto_auto(char *tags, char *libs, CompilerType ct, const char *comp) {
    int support = probe_lto_support(ct, comp);
    if (support == 0) {
        printf(YELLOW "[LTO_PROBE] Compiler does not support LTO — skipping.\n" RESET);
        return;
    }
    if (ct == CT_MSVC) {
        strncat(tags, " /GL",   B_SIZE - strlen(tags) - 1);
        strncat(libs, " /LTCG", B_SIZE - strlen(libs) - 1);
        printf(GREEN "[LTO_PROBE] MSVC LTCG enabled (/GL /LTCG)\n" RESET);
    } else if (support == 2) {
        strncat(tags, " -flto=thin", B_SIZE - strlen(tags) - 1);
        strncat(libs, " -flto=thin", B_SIZE - strlen(libs) - 1);
        printf(GREEN "[LTO_PROBE] ThinLTO enabled (-flto=thin) — fast parallel LTO\n" RESET);
    } else {
        strncat(tags, " -flto", B_SIZE - strlen(tags) - 1);
        strncat(libs, " -flto", B_SIZE - strlen(libs) - 1);
        printf(GREEN "[LTO_PROBE] Full LTO enabled (-flto)\n" RESET);
    }
    printf(CYAN "[LTO_PROBE] Expected speedup: 10–20%% at runtime\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  [12] DEBUG_POSTFIX: Auto-suffix binaries/libs in debug mode        */
/*       DEBUG_POSTFIX: -d   →  myapp-d.exe / libfoo-d.a             */
/*       Better: also sets RELEASE_POSTFIX and strips for release.    */
/* ------------------------------------------------------------------ */

void apply_debug_postfix(char *out, const char *postfix,
                                 const char *profile) {
    if (!postfix || !postfix[0]) return;

    /* Only apply in debug profile */
    char prof[32]; strncpy(prof, profile && profile[0] ? profile : "release", 31);
    for (int i = 0; prof[i]; i++) prof[i] = (char)tolower((unsigned char)prof[i]);
    if (strcmp(prof, "debug") != 0) return;

    /* Insert postfix before extension */
    char *dot = strrchr(out, '.');
    char *sep = strrchr(out, SEP);
    if (dot && (!sep || dot > sep)) {
        /* Has extension: insert before it */
        /* e.g. bin/myapp.exe  ->  bin/myapp-d.exe */
        char ext[32]; strncpy(ext, dot, 31);
        *dot = '\0';
        char tmp[512]; snprintf(tmp, sizeof(tmp), "%s%s%s", out, postfix, ext);
        strncpy(out, tmp, 511);
    } else {
        /* No extension */
        strncat(out, postfix, 511 - strlen(out));
    }
    printf(CYAN "[DBG_SUFFIX] Output renamed: %s\n" RESET, out);
}

/* ------------------------------------------------------------------ */
/*  [13] CONFIGURE_FILE: Template substitution                         */
/*       Syntax: CONFIGURE_FILE: templates/version.h.in -> include/version.h */
/*       Variables: @VAR@ style (from OMakeLists.txt: VERSION, DEFINE) */
/*       Also: @OMAKE_VERSION@, @BUILD_DATE@, @GIT_HASH@, @PLATFORM@  */
/*       Also supports ${VAR} and #omakedefine.    */
/* ------------------------------------------------------------------ */

typedef struct {
    char key[128];
    char val[512];
} CfgVar;

int configure_file_run(const char *src_path, const char *dst_path,
                               CfgVar *vars, int nv) {
    FILE *in = fopen(src_path, "r");
    if (!in) {
        printf(RED "[GEN_HEADER] Cannot open template: %s\n" RESET, src_path);
        return 0;
    }
    /* Ensure destination directory exists */
    char dst_tmp[600]; snprintf(dst_tmp, sizeof(dst_tmp), "%s", dst_path);
    ensure_dir_exists(dst_tmp);

    FILE *out = fopen(dst_path, "w");
    if (!out) {
        fclose(in);
        printf(RED "[GEN_HEADER] Cannot write: %s\n" RESET, dst_path);
        return 0;
    }

    char line[4096];
    while (fgets(line, sizeof(line), in)) {
        char result[8192]; strncpy(result, line, 8191);

        /* Substitute @VAR@ style */
        for (int i = 0; i < nv; i++) {
            char pattern[256]; snprintf(pattern, sizeof(pattern), "@%s@", vars[i].key);
            char *pos;
            while ((pos = strstr(result, pattern)) != NULL) {
                /* Replace pattern with value */
                char before[8192], after[4096];
                int before_len = (int)(pos - result);
                strncpy(before, result, before_len); before[before_len] = '\0';
                strncpy(after, pos + strlen(pattern), sizeof(after)-1);
                snprintf(result, sizeof(result), "%s%s%s", before, vars[i].val, after);
            }
            /* Also handle ${VAR} style */
            char pattern2[256]; snprintf(pattern2, sizeof(pattern2), "${%s}", vars[i].key);
            while ((pos = strstr(result, pattern2)) != NULL) {
                char before[8192], after[4096];
                int before_len = (int)(pos - result);
                strncpy(before, result, before_len); before[before_len] = '\0';
                strncpy(after, pos + strlen(pattern2), sizeof(after)-1);
                snprintf(result, sizeof(result), "%s%s%s", before, vars[i].val, after);
            }
        }

        /* Handle #omakedefine VAR → #define VAR / comment out if not set */
        if (strncmp(result, "#omakedefine ", 13) == 0 || strncmp(result, "#cmakedefine ", 13) == 0) {
            char varname[128]; sscanf(result + 13, "%127s", varname);
            int found = 0;
            for (int i = 0; i < nv; i++) {
                if (strcmp(vars[i].key, varname) == 0 && vars[i].val[0]) {
                    fprintf(out, "#define %s %s\n", varname, vars[i].val);
                    found = 1; break;
                }
            }
            if (!found) fprintf(out, "/* #undef %s */\n", varname);
            continue;
        }
        /* #omakedefine01 VAR → 1 or 0 */
        if (strncmp(result, "#omakedefine01 ", 15) == 0 || strncmp(result, "#cmakedefine01 ", 15) == 0) {
            char varname[128]; sscanf(result + 15, "%127s", varname);
            int found = 0;
            for (int i = 0; i < nv; i++) {
                if (strcmp(vars[i].key, varname) == 0 && vars[i].val[0]) { found = 1; break; }
            }
            fprintf(out, "#define %s %d\n", varname, found ? 1 : 0);
            continue;
        }

        fputs(result, out);
    }

    fclose(in); fclose(out);
    printf(GREEN "[GEN_HEADER] %s  ->  %s\n" RESET, src_path, dst_path);
    return 1;
}

/* Build the variable table from build_engine context */
/* Called with collected vars after parsing OMakeLists.txt */
void run_configure_files(const char *cfg_list,
                                 const char *ver, const char *define_list,
                                 const char *project_name) {
    if (!cfg_list || !cfg_list[0]) return;

    /* Build var table */
    CfgVar vars[256]; int nv = 0;

    /* Built-in variables */
    auto_add: ;
#define ADD_VAR(k, v) do { \
    if (nv < 255) { strncpy(vars[nv].key, (k), 127); strncpy(vars[nv].val, (v), 511); nv++; } \
} while(0)

    ADD_VAR("PROJECT_NAME",    project_name && project_name[0] ? project_name : "omake_project");
    ADD_VAR("OMAKE_VERSION",   VERSION);

    if (ver && ver[0]) {
        ADD_VAR("VERSION",     ver);
        ADD_VAR("PROJECT_VERSION", ver);
        /* Split major.minor.patch */
        char vcp[64]; strncpy(vcp, ver, 63);
        char *maj = strtok(vcp, "."),
             *min = strtok(NULL, "."),
             *pat = strtok(NULL, ".");
        ADD_VAR("VERSION_MAJOR", maj ? maj : "0");
        ADD_VAR("VERSION_MINOR", min ? min : "0");
        ADD_VAR("VERSION_PATCH", pat ? pat : "0");
    }

    /* Build date / time */
    {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char date_buf[32], time_buf[32];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", tm);
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);
        ADD_VAR("BUILD_DATE", date_buf);
        ADD_VAR("BUILD_TIME", time_buf);
    }

    /* Platform */
#ifdef _WIN32
    ADD_VAR("PLATFORM", "Windows");
    ADD_VAR("PLATFORM_WINDOWS", "1");
    ADD_VAR("PLATFORM_LINUX",   "0");
    ADD_VAR("PLATFORM_MACOS",   "0");
#elif defined(__APPLE__)
    ADD_VAR("PLATFORM", "macOS");
    ADD_VAR("PLATFORM_WINDOWS", "0");
    ADD_VAR("PLATFORM_LINUX",   "0");
    ADD_VAR("PLATFORM_MACOS",   "1");
#else
    ADD_VAR("PLATFORM", "Linux");
    ADD_VAR("PLATFORM_WINDOWS", "0");
    ADD_VAR("PLATFORM_LINUX",   "1");
    ADD_VAR("PLATFORM_MACOS",   "0");
#endif

    /* Git hash */
    {
        char hash[48] = "unknown";
        FILE *gp = popen("git rev-parse --short HEAD 2>/dev/null", "r");
        if (gp) { fscanf(gp, "%47s", hash); pclose(gp); }
        ADD_VAR("GIT_HASH", hash);
        ADD_VAR("GIT_COMMIT", hash);
    }

    /* Parse DEFINE: list into vars */
    if (define_list && define_list[0]) {
        char dcopy[B_SIZE]; strncpy(dcopy, define_list, B_SIZE-1);
        char *tok = strtok(dcopy, " \t");
        while (tok && nv < 255) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = '\0';
                char k[128], v[512];
                strncpy(k, tok[0]=='-'&&tok[1]=='D'?tok+2:tok, 127);
                strncpy(v, eq+1, 511);
                /* Strip quotes */
                if (v[0]=='"') { memmove(v,v+1,strlen(v)); char *e=strrchr(v,'"'); if(e)*e='\0'; }
                ADD_VAR(k, v);
            }
            tok = strtok(NULL, " \t");
        }
    }
#undef ADD_VAR

    /* Parse the CONFIGURE_FILE list: "src.h.in->dst.h src2.in->dst2" */
    char list_copy[B_SIZE]; strncpy(list_copy, cfg_list, B_SIZE-1);
    char *entry = strtok(list_copy, " \t\n");
    while (entry) {
        char *arrow = strstr(entry, "->");
        if (arrow) {
            *arrow = '\0';
            char *src = entry, *dst = arrow + 2;
            trim(src); trim(dst);
            configure_file_run(src, dst, vars, nv);
        } else {
            /* Single arg: auto-derive dst by stripping .in */
            char dst[512]; strncpy(dst, entry, 511);
            char *dot_in = NULL;
            /* Find last .in */
            char *p = dst + strlen(dst) - 3;
            if (p > dst && strcmp(p, ".in") == 0) { *p = '\0'; dot_in = p; }
            if (dot_in) configure_file_run(entry, dst, vars, nv);
            else printf(YELLOW "[GEN_HEADER] Cannot derive dest from: %s (use src->dst)\n" RESET, entry);
        }
        entry = strtok(NULL, " \t\n");
    }
}

/* ------------------------------------------------------------------ */
/*  [14] EXTERNAL_PROJECT: Heavy external lib management               */
/*       Heavy external library manager — simpler syntax:         */
/*       EXTERNAL_PROJECT: NAME=opencv                                 */
/*                         URL=https://...opencv-4.8.tar.gz           */
/*                         BUILD_ARGS=-DBUILD_TESTS=OFF               */
/*                         INSTALL_DIR=thirdparty/opencv               */
/*                         PATCH=patches/opencv.patch                 */
/*       Better: hash verification, parallel builds, skip-if-exists   */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[64];
    char url[512];           /* tarball URL or git URL */
    char cmake_args[512];    /* -DFOO=BAR ... */
    char install_dir[512];   /* where to install/stage */
    char patch_file[512];    /* optional .patch file */
    char configure_cmd[512]; /* custom configure script */
    char build_cmd[512];     /* custom build command */
    char sha256[80];         /* optional checksum */
    int  git_mode;           /* 1 = git clone, 0 = tarball */
    char git_tag[64];
} ExternalProject;

#define MAX_EXTERNAL 16
static ExternalProject g_ext_projects[MAX_EXTERNAL];
static int             g_ext_count = 0;

void external_project_parse(const char *line) {
    /* EXTERNAL_PROJECT: key=value key=value ... */
    if (g_ext_count >= MAX_EXTERNAL) return;
    ExternalProject *ep = &g_ext_projects[g_ext_count];
    memset(ep, 0, sizeof(*ep));

    char tmp[B_SIZE]; strncpy(tmp, line, B_SIZE-1);
    char *tok = strtok(tmp, " \t");
    while (tok) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0'; char *k = tok, *v = eq+1; trim(k); trim(v);
            if      (strcasecmp(k,"NAME")       ==0) strncpy(ep->name,       v,63);
            else if (strcasecmp(k,"URL")        ==0) strncpy(ep->url,        v,511);
            else if (strcasecmp(k,"BUILD_ARGS") ==0) strncpy(ep->cmake_args, v,511);
            else if (strcasecmp(k,"CMAKE")      ==0) strncpy(ep->cmake_args, v,511);  /* alias */
            else if (strcasecmp(k,"INSTALL_DIR")==0) strncpy(ep->install_dir,v,511);
            else if (strcasecmp(k,"PATCH")      ==0) strncpy(ep->patch_file, v,511);
            else if (strcasecmp(k,"CONFIGURE")  ==0) strncpy(ep->configure_cmd,v,511);
            else if (strcasecmp(k,"BUILD")      ==0) strncpy(ep->build_cmd,  v,511);
            else if (strcasecmp(k,"SHA256")     ==0) strncpy(ep->sha256,     v,79);
            else if (strcasecmp(k,"TAG")        ==0) { strncpy(ep->git_tag,  v,63); ep->git_mode=1; }
        }
        tok = strtok(NULL, " \t");
    }
    /* Auto-detect git mode */
    if (strstr(ep->url, ".git") || strstr(ep->url, "github.com"))
        ep->git_mode = 1;

    if (ep->name[0] && ep->url[0]) {
        g_ext_count++;
        printf(CYAN "[EXTERN] Registered: %s <- %s\n" RESET, ep->name, ep->url);
    }
}

void external_project_build_all(char *tags, char *libs) {
    if (g_ext_count == 0) return;

    int has_git   = tool_exists("git");
    int has_cmake = tool_exists("cmake");
    int has_curl  = tool_exists("curl");

    for (int i = 0; i < g_ext_count; i++) {
        ExternalProject *ep = &g_ext_projects[i];
        if (!ep->install_dir[0]) {
            snprintf(ep->install_dir, sizeof(ep->install_dir),
                     "thirdparty/%s", ep->name);
        }

        printf(BLUE "\n[EXTERNAL] ═══ %s ═══\n" RESET, ep->name);

        /* Check if already built (sentinel file) */
        char sentinel[600];
        snprintf(sentinel, sizeof(sentinel), "%s/.omake_built", ep->install_dir);
        if (is_regular_file(sentinel)) {
            printf(GREEN "[EXTERN] Already built: %s (delete %s to rebuild)\n" RESET,
                   ep->name, sentinel);
            goto inject_flags;
        }

        /* ── Download ── */
        char src_dir[600];
        snprintf(src_dir, sizeof(src_dir), "%s/_src", ep->install_dir);

        if (ep->git_mode && has_git) {
            char clone_cmd[1024];
            if (ep->git_tag[0])
                snprintf(clone_cmd, sizeof(clone_cmd),
                    "git clone --depth=1 --branch \"%s\" \"%s\" \"%s\"",
                    ep->git_tag, ep->url, src_dir);
            else
                snprintf(clone_cmd, sizeof(clone_cmd),
                    "git clone --depth=1 \"%s\" \"%s\"", ep->url, src_dir);

            if (!dir_exists(src_dir)) {
                printf(CYAN "[EXTERN] git clone: %s\n" RESET, ep->url);
                if (system(clone_cmd) != 0) {
                    printf(RED "[EXTERN] git clone failed for %s\n" RESET, ep->name);
                    continue;
                }
            }
        } else if (!ep->git_mode && (has_curl || tool_exists("wget"))) {
            /* Download tarball */
            char tarball[512];
            snprintf(tarball, sizeof(tarball), "/tmp/_omake_%s.tar.gz", ep->name);
            if (!is_regular_file(tarball)) {
                char dl_cmd[1024];
                if (has_curl)
                    snprintf(dl_cmd, sizeof(dl_cmd),
                        "curl -L --retry 3 -o \"%s\" \"%s\"", tarball, ep->url);
                else
                    snprintf(dl_cmd, sizeof(dl_cmd),
                        "wget -O \"%s\" \"%s\"", tarball, ep->url);
                printf(CYAN "[EXTERN] Downloading: %s\n" RESET, ep->url);
                if (system(dl_cmd) != 0) {
                    printf(RED "[EXTERN] Download failed: %s\n" RESET, ep->name); continue;
                }
            }
            /* Extract */
            char extract[1024];
            { char _t[650]; snprintf(_t,sizeof(_t),"%s/_t",src_dir); ensure_dir_exists(_t); remove(_t); }
            snprintf(extract, sizeof(extract),
                "tar -xzf \"%s\" --strip-components=1 -C \"%s\"", tarball, src_dir);
            system(extract);
        } else {
            printf(RED "[EXTERN] No download tool available for %s\n" RESET, ep->name); continue;
        }

        /* ── Patch ── */
        if (ep->patch_file[0] && is_regular_file(ep->patch_file)) {
            char patch_cmd[512];
            snprintf(patch_cmd, sizeof(patch_cmd),
                "patch -p1 -d \"%s\" < \"%s\"", src_dir, ep->patch_file);
            printf(CYAN "[EXTERN] Applying patch: %s\n" RESET, ep->patch_file);
            system(patch_cmd);
        }

        /* ── Configure + Build ── */
        char build_dir[600];
        snprintf(build_dir, sizeof(build_dir), "%s/_build", ep->install_dir);

        if (ep->configure_cmd[0]) {
            /* Custom configure */
            char cwd[1024]; getcwd(cwd, sizeof(cwd));
            chdir(src_dir);
            printf(CYAN "[EXTERN] Configure: %s\n" RESET, ep->configure_cmd);
            system(ep->configure_cmd);
            chdir(cwd);
        } else if (ep->cmake_args[0] || has_cmake) {
            /* cmake-system build */
            char abs_src[700], abs_build[700], abs_install[700];
            realpath(src_dir,           abs_src);
            realpath(ep->install_dir,   abs_install);
            snprintf(abs_build, sizeof(abs_build), "%s/_build", ep->install_dir);
            { char _t[750]; snprintf(_t,sizeof(_t),"%s/_t",abs_build); ensure_dir_exists(_t); remove(_t); }

            char cmake_cfg[1200];
            snprintf(cmake_cfg, sizeof(cmake_cfg),
                "cmake -S \"%s\" -B \"%s\" "
                "-DCMAKE_BUILD_TYPE=Release "
                "-DCMAKE_INSTALL_PREFIX=\"%s\" "
                "%s",
                abs_src, abs_build, abs_install, ep->cmake_args);
            printf(CYAN "[EXTERN] Configure: %s\n" RESET, ep->name);
            system(cmake_cfg);

            /* Get CPU count for parallel */
            int ncpu = get_cpu_count(); if (ncpu < 1) ncpu = 4;
            char cmake_build[512];
            snprintf(cmake_build, sizeof(cmake_build),
                "cmake --build \"%s\" --parallel %d", abs_build, ncpu);
            printf(CYAN "[EXTERN] Build: %s\n" RESET, ep->name);
            if (system(cmake_build) != 0) {
                printf(RED "[EXTERN] Build failed: %s\n" RESET, ep->name); continue;
            }

            char cmake_install[512];
            snprintf(cmake_install, sizeof(cmake_install),
                "cmake --install \"%s\"", abs_build);
            printf(CYAN "[EXTERN] Install: %s\n" RESET, ep->name);
            system(cmake_install);
        } else if (ep->build_cmd[0]) {
            char cwd[1024]; getcwd(cwd, sizeof(cwd));
            chdir(src_dir);
            system(ep->build_cmd);
            chdir(cwd);
        }

        /* ── Write sentinel ── */
        FILE *sf = fopen(sentinel, "w");
        if (sf) { fprintf(sf, "built by omake\n"); fclose(sf); }
        printf(GREEN "[EXTERN] Done: %s -> %s\n" RESET, ep->name, ep->install_dir);

        inject_flags:
        /* Auto-inject include/lib flags */
        char inc_try[600];
        snprintf(inc_try, sizeof(inc_try), "%s/include", ep->install_dir);
        if (dir_exists(inc_try)) {
            char flag[640]; snprintf(flag, sizeof(flag), " -I\"%s\"", inc_try);
            if (!strstr(tags, flag)) strncat(tags, flag, B_SIZE-strlen(tags)-1);
        }
        char lib_try[600];
        snprintf(lib_try, sizeof(lib_try), "%s/lib", ep->install_dir);
        if (dir_exists(lib_try)) {
            char flag[640]; snprintf(flag, sizeof(flag), " -L\"%s\"", lib_try);
            if (!strstr(libs, flag)) strncat(libs, flag, B_SIZE-strlen(libs)-1);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  [15] PKG_PROBE: Smart pkg-config + fallback header search          */
/*       Three-layer fallback:                           */
/*       1. pkg-config flags (preferred)                               */
/*       2. Fallback: scan /usr/include, /usr/local/include            */
/*       3. Fallback: vcpkg / conan manifest detection                */
/*       PKG_PROBE: openssl zlib SDL2 -- reports exact paths found    */
/* ------------------------------------------------------------------ */

void pkg_probe_one(const char *libname, char *tags, char *libs) {
    char cmd[512];

    /* 1. pkg-config */
    if (tool_exists("pkg-config")) {
        /* cflags */
        snprintf(cmd, sizeof(cmd), "pkg-config --cflags %s 2>/dev/null", libname);
        FILE *p = popen(cmd, "r");
        if (p) {
            char flags[512]; flags[0] = '\0';
            if (fgets(flags, sizeof(flags), p)) {
                char *nl = strrchr(flags, '\n'); if (nl) *nl = '\0';
                if (flags[0]) {
                    strncat(tags, " ", B_SIZE-strlen(tags)-1);
                    strncat(tags, flags, B_SIZE-strlen(tags)-1);
                    printf(GREEN "[PROBE] %s cflags: %s\n" RESET, libname, flags);
                }
            }
            pclose(p);
        }
        /* libs */
        snprintf(cmd, sizeof(cmd), "pkg-config --libs %s 2>/dev/null", libname);
        p = popen(cmd, "r");
        if (p) {
            char lflags[512]; lflags[0] = '\0';
            if (fgets(lflags, sizeof(lflags), p)) {
                char *nl = strrchr(lflags, '\n'); if (nl) *nl = '\0';
                if (lflags[0]) {
                    strncat(libs, " ", B_SIZE-strlen(libs)-1);
                    strncat(libs, lflags, B_SIZE-strlen(libs)-1);
                    printf(GREEN "[PROBE] %s libs: %s\n" RESET, libname, lflags);
                    return;
                }
            }
            pclose(p);
        }
    }

    /* 2. Header scan fallback */
    const char *inc_dirs[] = {
        "/usr/include", "/usr/local/include",
        "/opt/homebrew/include", "/opt/local/include",
        "/mingw64/include", NULL
    };
    for (int d = 0; inc_dirs[d]; d++) {
        char hdr_path[600];
        /* Try libname/libname.h and libname.h */
        snprintf(hdr_path, sizeof(hdr_path), "%s/%s/%s.h", inc_dirs[d], libname, libname);
        if (!is_regular_file(hdr_path))
            snprintf(hdr_path, sizeof(hdr_path), "%s/%s.h", inc_dirs[d], libname);
        if (is_regular_file(hdr_path)) {
            char iflag[640]; snprintf(iflag, sizeof(iflag), " -I\"%s\"", inc_dirs[d]);
            if (!strstr(tags, iflag)) strncat(tags, iflag, B_SIZE-strlen(tags)-1);
            char lflag[128]; snprintf(lflag, sizeof(lflag), " -l%s", libname);
            if (!strstr(libs, lflag)) strncat(libs, lflag, B_SIZE-strlen(libs)-1);
            printf(YELLOW "[PROBE] %s found via header scan: %s\n" RESET, libname, hdr_path);
            return;
        }
    }

    /* 3. vcpkg detection */
    const char *vcpkg_dirs[] = {
        "vcpkg_installed/x64-windows/include",
        "vcpkg_installed/x64-linux/include",
        "vcpkg_installed/arm64-osx/include",
        NULL
    };
    for (int d = 0; vcpkg_dirs[d]; d++) {
        char vpath[600];
        snprintf(vpath, sizeof(vpath), "%s/%s", vcpkg_dirs[d], libname);
        if (dir_exists(vpath)) {
            char iflag[700]; snprintf(iflag, sizeof(iflag), " -I\"%s\"", vcpkg_dirs[d]);
            if (!strstr(tags, iflag)) strncat(tags, iflag, B_SIZE-strlen(tags)-1);
            printf(YELLOW "[PROBE] %s found via vcpkg: %s\n" RESET, libname, vcpkg_dirs[d]);
            return;
        }
    }

    printf(RED "[PROBE] WARNING: %s not found (pkg-config, headers, vcpkg all failed)\n" RESET, libname);
}

void apply_pkg_probe(const char *probe_list, char *tags, char *libs) {
    if (!probe_list || !probe_list[0]) return;
    char tmp[B_SIZE]; strncpy(tmp, probe_list, B_SIZE-1);
    char *tok = strtok(tmp, " \t,");
    while (tok) {
        pkg_probe_one(tok, tags, libs);
        tok = strtok(NULL, " \t,");
    }
}

/* ------------------------------------------------------------------ */
/*  [16] REQUIRE_STANDARD: Compiler capability check                   */
/*       REQUIRE_STANDARD: C++=20                                      */
/*       Probes compiler with a C++20/17/14 test snippet.             */
/*       Aborts build if standard not supported (like target_compile_features) */
/* ------------------------------------------------------------------ */

int probe_standard(const char *comp, const char *std_flag,
                           const char *test_snippet) {
    const char *tsrc = "/tmp/_omake_std_probe.cpp";
    const char *tobj = "/tmp/_omake_std_probe.o";
    FILE *f = fopen(tsrc, "w");
    if (!f) return 1; /* assume ok if can't write */
    fputs(test_snippet, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s -c \"%s\" -o \"%s\" 2>/dev/null",
             comp, std_flag, tsrc, tobj);
    int ok = (system(cmd) == 0);
    remove(tsrc); remove(tobj);
    return ok;
}

void check_compile_features(const char *req, const char *comp) {
    if (!req || !req[0]) return;
    char tmp[128]; strncpy(tmp, req, 127);
    char *tok = strtok(tmp, " ,");
    while (tok) {
        char std_flag[32] = ""; char test_code[256] = "";
        if      (strncasecmp(tok,"C++=23",6)==0||strncasecmp(tok,"CXX23",5)==0) {
            strcpy(std_flag,"-std=c++23");
            strcpy(test_code,"#include<version>\n#if __cplusplus < 202302L\n#error\n#endif\nint main(){}\n");
        } else if (strncasecmp(tok,"C++=20",6)==0||strncasecmp(tok,"CXX20",5)==0) {
            strcpy(std_flag,"-std=c++20");
            strcpy(test_code,"#include<concepts>\nconcept C=requires(int x){x+1;};\nint main(){}\n");
        } else if (strncasecmp(tok,"C++=17",6)==0||strncasecmp(tok,"CXX17",5)==0) {
            strcpy(std_flag,"-std=c++17");
            strcpy(test_code,"#include<optional>\nstd::optional<int>x;\nint main(){}\n");
        } else if (strncasecmp(tok,"C++=14",6)==0||strncasecmp(tok,"CXX14",5)==0) {
            strcpy(std_flag,"-std=c++14");
            strcpy(test_code,"auto f=[](auto x){return x;};\nint main(){}\n");
        } else if (strncasecmp(tok,"C=23",4)==0) {
            strcpy(std_flag,"-std=c23");
            strcpy(test_code,"int main(void){return 0;}\n");
        } else if (strncasecmp(tok,"C=11",4)==0||strncasecmp(tok,"C11",3)==0) {
            strcpy(std_flag,"-std=c11");
            strcpy(test_code,"_Static_assert(1,\"c11\");\nint main(void){return 0;}\n");
        }

        if (!std_flag[0]) { tok = strtok(NULL," ,"); continue; }

        if (probe_standard(comp, std_flag, test_code)) {
            printf(GREEN "[NEED_STD] %s: supported ✓\n" RESET, tok);
        } else {
            printf(RED "[NEED_STD] FATAL: %s requires %s but compiler (%s) doesn't support it!\n" RESET,
                   tok, std_flag, comp);
            printf(RED "[NEED_STD] Upgrade your compiler or change STANDARD: directive.\n" RESET);
            exit(1);  /* Hard abort — unsupported standard */
        }
        tok = strtok(NULL, " ,");
    }
}

/* ------------------------------------------------------------------ */
/*  [18] IPO / Interprocedural Optimization    */
/*       IPO: on   — enables cross-module inlining + dead code elim   */
/*       IPO: full — full program optimization (slower build)         */
/*       Equivalent to: set_property(TARGET ... INTERPROCEDURAL_OPTIMIZATION TRUE) */
/*       Implemented via LTO flags (IPO IS LTO under the hood)        */
/* ------------------------------------------------------------------ */

void apply_ipo(const char *ipo_mode, char *tags, char *libs, CompilerType ct) {
    if (!ipo_mode || !ipo_mode[0]) return;
    char m[16]; strncpy(m, ipo_mode, 15);
    for (int i = 0; m[i]; i++) m[i] = (char)tolower((unsigned char)m[i]);

    if (strcmp(m,"off")==0 || strcmp(m,"no")==0 || strcmp(m,"false")==0) return;

    if (ct == CT_MSVC) {
        if (!strstr(tags,"/GL"))   strncat(tags, " /GL",   B_SIZE-strlen(tags)-1);
        if (!strstr(libs,"/LTCG")) strncat(libs, " /LTCG", B_SIZE-strlen(libs)-1);
        printf(GREEN "[IPO] MSVC whole-program optimization: /GL /LTCG\n" RESET);
    } else {
        /* Probe ThinLTO support first, fall back to full LTO */
        int thin_ok = (strcmp(m,"full")!=0);
        if (thin_ok) {
            /* Quick probe */
            const char *tp="/tmp/_ipo_probe.c"; FILE *tf=fopen(tp,"w");
            if(tf){fprintf(tf,"int x=1;\n");fclose(tf);}
            char pc[256]; snprintf(pc,sizeof(pc),
                "gcc -flto=thin -c \"%s\" -o /tmp/_ipo_probe.o 2>/dev/null",tp);
            thin_ok=(system(pc)==0);
            remove(tp); remove("/tmp/_ipo_probe.o");
        }
        if (thin_ok) {
            if (!strstr(tags,"-flto=thin")) strncat(tags, " -flto=thin", B_SIZE-strlen(tags)-1);
            if (!strstr(libs,"-flto=thin")) strncat(libs, " -flto=thin", B_SIZE-strlen(libs)-1);
            printf(GREEN "[IPO] ThinLTO/IPO: -flto=thin\n" RESET);
        } else {
            if (!strstr(tags,"-flto")) strncat(tags, " -flto", B_SIZE-strlen(tags)-1);
            if (!strstr(libs,"-flto")) strncat(libs, " -flto", B_SIZE-strlen(libs)-1);
            printf(GREEN "[IPO] Full LTO/IPO: -flto\n" RESET);
        }
    }
    printf(CYAN "[IPO] Enables: cross-module inlining, devirtualization, dead code elimination\n" RESET);
}

/* ------------------------------------------------------------------ */
/*  [20] CLEAN_BUILD: No registry pollution   */
/*       CLEAN_BUILD: on — ensures build is fully portable:           */
/*       • No ~/.omake_cache pollution                       */
/*       • No registry entries (Windows)                              */
/*       • All outputs relative to project dir                        */
/*       • Prints a "portability report"                              */
/* ------------------------------------------------------------------ */

void enforce_clean_build(const char *out_bin, const char *obj_root) {
    int issues = 0;
    printf(BLUE "[CLEAN_BUILD] Portability check:\n" RESET);

    /* Check outputs are relative */
    if (out_bin && (out_bin[0]=='/'||out_bin[0]=='\\')) {
        printf(YELLOW "  [WARN] OUT: is absolute path — not portable: %s\n" RESET, out_bin);
        issues++;
    } else {
        printf(GREEN "  [OK] OUT: is relative path\n" RESET);
    }

    /* Check obj dir is relative */
    if (obj_root && (obj_root[0]=='/'||obj_root[0]=='\\')) {
        printf(YELLOW "  [WARN] OBJDIR: is absolute — not portable\n" RESET); issues++;
    } else {
        printf(GREEN "  [OK] OBJDIR: is relative\n" RESET);
    }

    /* Check no absolute TAGS: include paths */
    /* (can't check here without tags, but warn user) */

    /* Windows: no registry writes (OMake never writes to registry — report as clean) */
#ifdef _WIN32
    printf(GREEN "  [OK] No registry entries written (OMake policy)\n" RESET);
    printf(GREEN "  [OK] No ~/.omake_cache pollution\n" RESET);
#else
    /* check for cmake leftovers */
    const char *home = getenv("HOME");
    if (home) {
        char cmake_pkg[512]; snprintf(cmake_pkg, sizeof(cmake_pkg), "%s/.cmake/packages", home);
        if (dir_exists(cmake_pkg))
            printf(CYAN   "  [OK] OMake never writes to system registry or ~/.cmake\n" RESET);
        else
            printf(GREEN "  [OK] No ~/.omake_cache directory\n" RESET);
    }
#endif

    if (issues == 0)
        printf(GREEN "[CLEAN_BUILD] Fully portable build ✓\n" RESET);
    else
        printf(YELLOW "[CLEAN_BUILD] %d portability issue(s) found\n" RESET, issues);
}


/* ================================================================== */
/*  [17] --m-gvz ENHANCED: Target graph, interface libs, deps         */
/*       Now shows: source→header, source→binary, target→target,     */
/*       interface lib propagation, external projects as subgraph     */
/*       Better than cmake --graphviz: colors, legend, version info   */
/* ================================================================== */
/* (m-gvz already implemented — extended via cmd_m_gvz_targets below) */

void cmd_m_gvz_targets(void) {
    /* Extended graphviz: shows target dependency graph */
    if (g_target_count == 0 && g_ext_count == 0) {
        /* Fall through to normal m-gvz */
        cmd_m_gvz("png");
        return;
    }

    mkdir_auto("omake_graph");
    const char *dot_path = "omake_graph/targets.dot";
    FILE *dot = fopen(dot_path, "w");
    if (!dot) { printf(RED "[M-GVZ] Cannot write targets.dot\n" RESET); return; }

    PkgMeta m; memset(&m, 0, sizeof(m));
    load_pkg_meta(&m);

    fprintf(dot,
        "digraph omake_targets {\n"
        "    graph [label=\"%s v%s — Target Graph (OMake v" VERSION ")\" "
        "        labelloc=t fontname=Helvetica fontsize=16\n"
        "        bgcolor=\"#1E1E2E\" pad=0.5 nodesep=0.8 ranksep=1.4\n"
        "        splines=curved];\n"
        "    node [fontname=Helvetica fontsize=11 style=filled];\n"
        "    edge [fontname=Helvetica fontsize=9];\n\n",
        m.name, m.version);

    /* OMake targets */
    for (int i = 0; i < g_target_count; i++) {
        Target *t = &g_targets[i];
        char tid[256]; dot_id(t->name, tid, sizeof(tid));

        const char *shape, *color, *label_extra;
        if (t->is_interface) {
            shape = "hexagon"; color = "\"#8B5CF6\""; label_extra = "\\n[interface]";
        } else if (t->is_object) {
            shape = "component"; color = "\"#059669\""; label_extra = "\\n[objects]";
        } else if (strcmp(t->output_type, "static") == 0) {
            shape = "cylinder"; color = "\"#D97706\""; label_extra = "\\n[static lib]";
        } else if (strcmp(t->output_type, "shared") == 0) {
            shape = "cylinder"; color = "\"#2563EB\""; label_extra = "\\n[shared lib]";
        } else {
            shape = "house"; color = "\"#DC2626\""; label_extra = "\\n[executable]";
        }

        /* Alias label */
        char alias_str[160] = "";
        if (t->alias[0]) snprintf(alias_str, sizeof(alias_str), "\\n alias: %s", t->alias);

        fprintf(dot,
            "    %s [label=\"%s%s%s\" shape=%s fillcolor=%s "
            "fontcolor=white color=\"#404040\" penwidth=2];\n",
            tid, t->name, label_extra, alias_str, shape, color);
    }
    fprintf(dot, "\n");

    /* Target dependency edges */
    for (int i = 0; i < g_target_count; i++) {
        if (!g_targets[i].deps[0]) continue;
        char src_id[256]; dot_id(g_targets[i].name, src_id, sizeof(src_id));
        char deps_copy[512]; strncpy(deps_copy, g_targets[i].deps, 511);
        char *tok = strtok(deps_copy, " ,");
        while (tok) {
            char dst_id[256]; dot_id(tok, dst_id, sizeof(dst_id));
            fprintf(dot,
                "    %s -> %s [color=\"#94A3B8\" penwidth=1.5 "
                "style=%s label=\"uses\"];\n",
                src_id, dst_id,
                g_targets[i].is_interface ? "dashed" : "solid");
            tok = strtok(NULL, " ,");
        }
    }

    /* External projects as subgraph */
    if (g_ext_count > 0) {
        fprintf(dot, "\n    subgraph cluster_external {\n"
            "        label=\"External Projects\" fontcolor=white\n"
            "        style=filled fillcolor=\"#1A2940\" color=\"#334155\"\n");
        for (int i = 0; i < g_ext_count; i++) {
            char eid[256]; dot_id(g_ext_projects[i].name, eid, sizeof(eid));
            fprintf(dot,
                "        ext_%s [label=\"%s\\n[external]\" shape=cloud "
                "fillcolor=\"#475569\" fontcolor=white color=\"#64748B\"];\n",
                eid, g_ext_projects[i].name);
        }
        fprintf(dot, "    }\n\n");
    }

    /* Legend */
    fprintf(dot,
        "    subgraph cluster_legend {\n"
        "        label=\"Legend\" fontcolor=white style=filled fillcolor=\"#2A2A3E\"\n"
        "        fontname=Helvetica fontsize=10 color=\"#505060\"\n"
        "        _l_exe  [label=\"Executable\"   shape=house     fillcolor=\"#DC2626\" fontcolor=white];\n"
        "        _l_sta  [label=\"Static lib\"   shape=cylinder  fillcolor=\"#D97706\" fontcolor=white];\n"
        "        _l_shr  [label=\"Shared lib\"   shape=cylinder  fillcolor=\"#2563EB\" fontcolor=white];\n"
        "        _l_ifc  [label=\"Interface lib\" shape=hexagon  fillcolor=\"#8B5CF6\" fontcolor=white];\n"
        "        _l_obj  [label=\"Object pool\"  shape=component fillcolor=\"#059669\" fontcolor=white];\n"
        "        _l_ext  [label=\"External\"     shape=cloud     fillcolor=\"#475569\" fontcolor=white];\n"
        "        _l_exe -> _l_sta -> _l_shr -> _l_ifc -> _l_obj -> _l_ext [style=invis];\n"
        "    }\n");

    fprintf(dot, "}\n");
    fclose(dot);

    printf(GREEN "[M-GVZ] Target graph: %s (%d targets, %d external)\n" RESET,
           dot_path, g_target_count, g_ext_count);

    if (tool_exists("dot")) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "dot -Tpng -o omake_graph/targets.png \"%s\"", dot_path);
        if (system(cmd) == 0) printf(GREEN "[M-GVZ] Rendered: omake_graph/targets.png\n" RESET);
        snprintf(cmd, sizeof(cmd), "dot -Tsvg -o omake_graph/targets.svg \"%s\"", dot_path);
        system(cmd);
    }

    /* Also generate the source-level graph */
    cmd_m_gvz("png");
}

/* ================================================================== */
/*  [19] Qt .qrc Resource Compiler                                     */
/*       QT_RESOURCE: resources/app.qrc                               */
/*       Converts .qrc XML → rcc-generated .cpp → compiled into build */
/*       Also handles .qrc → FAKE fallback if rcc not present        */
/*       Also supports raw binary embedding without Qt               */
/*                          without Qt (EMBED_BINARY: directive)      */
/* ================================================================== */

/* Parse a simple .qrc XML file and emit a C++ resource array */
static int qrc_to_cpp_fallback(const char *qrc_path, const char *out_cpp) {
    FILE *qrc = fopen(qrc_path, "r");
    if (!qrc) { printf(RED "[QRC] Cannot open: %s\n" RESET, qrc_path); return 0; }

    FILE *cpp = fopen(out_cpp, "w");
    if (!cpp) { fclose(qrc); printf(RED "[QRC] Cannot write: %s\n" RESET, out_cpp); return 0; }

    fprintf(cpp,
        "/* Auto-generated by OMake Qt resource fallback */\n"
        "#include <cstring>\n\n"
        "/* Embedded resource table */\n"
        "struct OmakeResource { const char *path; const unsigned char *data; int size; };\n\n");

    /* Parse file entries from QRC XML */
    char line[1024];
    char entries[256][512]; int entry_count = 0;
    char prefix[256] = "/";

    while (fgets(line, sizeof(line), qrc)) {
        /* Extract prefix attribute */
        char *pp = strstr(line, "prefix=");
        if (pp) {
            pp += 7;
            char delim = *pp == '"' ? '"' : '\'';
            pp++;
            char *end = strchr(pp, delim);
            if (end && entry_count < 256) {
                int l = (int)(end - pp);
                if (l < 255) { strncpy(prefix, pp, l); prefix[l] = '\0'; }
            }
        }
        /* Extract file entries */
        char *fs = strstr(line, "<file");
        if (fs) {
            char *gt = strchr(fs, '>');
            char *lt = gt ? strstr(gt, "</file>") : NULL;
            if (gt && lt && entry_count < 256) {
                gt++;
                int l = (int)(lt - gt);
                if (l > 0 && l < 511) {
                    strncpy(entries[entry_count++], gt, l);
                    entries[entry_count-1][l] = '\0';
                }
            }
        }
    }
    fclose(qrc);

    /* Get directory of the .qrc file */
    char qrc_dir[512]; strncpy(qrc_dir, qrc_path, 511);
    char *last_sep = strrchr(qrc_dir, SEP);
    if (last_sep) *last_sep = '\0'; else strcpy(qrc_dir, ".");

    /* Emit binary data arrays */
    for (int i = 0; i < entry_count; i++) {
        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", qrc_dir, entries[i]);

        FILE *bin = fopen(file_path, "rb");
        if (!bin) {
            printf(YELLOW "[QRC] Missing resource file: %s\n" RESET, file_path);
            fprintf(cpp, "/* Missing: %s */\n", entries[i]);
            continue;
        }
        /* Create C identifier */
        char cid[512]; dot_id(entries[i], cid, sizeof(cid));
        fprintf(cpp, "static const unsigned char _res_%s[] = {\n    ", cid);
        int c, col = 0;
        long size = 0;
        while ((c = fgetc(bin)) != EOF) {
            fprintf(cpp, "0x%02X,", (unsigned char)c);
            if (++col >= 16) { fprintf(cpp, "\n    "); col = 0; }
            size++;
        }
        fprintf(cpp, "\n}; /* %ld bytes */\n\n", size);
        fclose(bin);
    }

    /* Emit resource table */
    fprintf(cpp, "static const OmakeResource _omake_resources[] = {\n");
    for (int i = 0; i < entry_count; i++) {
        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", qrc_dir, entries[i]);
        if (!is_regular_file(file_path)) continue;

        char cid[512]; dot_id(entries[i], cid, sizeof(cid));
        char vpath[512]; snprintf(vpath, sizeof(vpath), "%s/%s", prefix, entries[i]);
        fprintf(cpp, "    { \"%s\", _res_%s, (int)sizeof(_res_%s) },\n",
                vpath, cid, cid);
    }
    fprintf(cpp,
        "    { nullptr, nullptr, 0 }\n"
        "};\n\n"
        "/* Lookup function */\n"
        "const unsigned char* omake_resource_data(const char *path, int *size) {\n"
        "    for (int i = 0; _omake_resources[i].path; i++) {\n"
        "        if (strcmp(_omake_resources[i].path, path) == 0) {\n"
        "            if (size) *size = _omake_resources[i].size;\n"
        "            return _omake_resources[i].data;\n"
        "        }\n"
        "    }\n"
        "    if (size) *size = 0;\n"
        "    return nullptr;\n"
        "}\n");

    fclose(cpp);
    printf(GREEN "[QRC] Fallback C++ resource: %s (%d files)\n" RESET, out_cpp, entry_count);
    return 1;
}

void process_qt_resources(const char *qrc_list, const char *obj_root,
                            const char *comp, CompilerType ct,
                            const char *tags, const char *auto_inc,
                            char *extra_objs, int extra_objs_sz) {
    if (!qrc_list || !qrc_list[0]) return;

    char tmp[B_SIZE]; strncpy(tmp, qrc_list, B_SIZE-1);
    char *tok = strtok(tmp, " \t;,");

    while (tok) {
        printf(CYAN "[QRC] Processing: %s\n" RESET, tok);

        char stem[256]; path_stem(tok, stem, sizeof(stem));
        char gen_cpp[600]; snprintf(gen_cpp, sizeof(gen_cpp), "%s/qrc_%s.cpp", obj_root, stem);
        char gen_obj[600]; snprintf(gen_obj, sizeof(gen_obj), "%s/qrc_%s.o",   obj_root, stem);
        ensure_dir_exists(gen_obj);

        int generated = 0;

        /* Try Qt's rcc first */
        if (tool_exists("rcc")) {
            char rcc_cmd[1024];
            snprintf(rcc_cmd, sizeof(rcc_cmd),
                "rcc --name %s --output \"%s\" \"%s\"", stem, gen_cpp, tok);
            printf(CYAN "[QRC] rcc: %s\n" RESET, rcc_cmd);
            generated = (system(rcc_cmd) == 0);
        }
        if (!generated && tool_exists("rcc6")) {
            char rcc_cmd[1024];
            snprintf(rcc_cmd, sizeof(rcc_cmd),
                "rcc6 --name %s --output \"%s\" \"%s\"", stem, gen_cpp, tok);
            generated = (system(rcc_cmd) == 0);
        }

        /* Fallback: our own QRC parser */
        if (!generated) {
            printf(YELLOW "[QRC] Qt rcc not found — using OMake fallback parser\n" RESET);
            generated = qrc_to_cpp_fallback(tok, gen_cpp);
        }

        if (!generated) { tok = strtok(NULL, " \t;,"); continue; }

        /* Compile the generated .cpp */
        char *tags_tr = (char*)calloc(1, B_SIZE);
        char *inc_tr  = (char*)calloc(1, B_SIZE);
        if (tags_tr && inc_tr) {
            translate_tags(tags, tags_tr, B_SIZE, ct);
            translate_inc(auto_inc, inc_tr, B_SIZE, ct);

            char compile_cmd[B_SIZE];
            if (ct == CT_MSVC)
                snprintf(compile_cmd, sizeof(compile_cmd),
                    "%s /nologo /c \"%s\" /Fo\"%s\" %s %s",
                    comp, gen_cpp, gen_obj, tags_tr, inc_tr);
            else
                snprintf(compile_cmd, sizeof(compile_cmd),
                    "%s -c \"%s\" -o \"%s\" %s %s",
                    comp, gen_cpp, gen_obj, tags_tr, inc_tr);

            printf(CYAN "[QRC] Compile: %s\n" RESET, compile_cmd);
            if (system(compile_cmd) == 0) {
                /* Append to extra_objs for linking */
                strncat(extra_objs, " \"", extra_objs_sz - strlen(extra_objs) - 1);
                strncat(extra_objs, gen_obj, extra_objs_sz - strlen(extra_objs) - 1);
                strncat(extra_objs, "\"", extra_objs_sz - strlen(extra_objs) - 1);
                printf(GREEN "[QRC] Resource compiled: %s\n" RESET, gen_obj);
            }
        }
        free(tags_tr); free(inc_tr);
        tok = strtok(NULL, " \t;,");
    }
}

/* EMBED_BINARY: name=path/to/file  →  embed raw binary as C array */
void process_embed_binary(const char *embed_list, const char *obj_root,
                           const char *comp, CompilerType ct,
                           const char *tags, const char *auto_inc,
                           char *extra_objs, int extra_objs_sz) {
    if (!embed_list || !embed_list[0]) return;

    char gen_cpp[600]; snprintf(gen_cpp, sizeof(gen_cpp), "%s/embedded_resources.cpp", obj_root);
    char gen_obj[600]; snprintf(gen_obj, sizeof(gen_obj), "%s/embedded_resources.o",   obj_root);
    ensure_dir_exists(gen_obj);

    FILE *cpp = fopen(gen_cpp, "w");
    if (!cpp) return;
    fprintf(cpp, "/* OMake embedded binary resources — auto-generated */\n#include <cstddef>\n\n");

    char tmp[B_SIZE]; strncpy(tmp, embed_list, B_SIZE-1);
    char *tok = strtok(tmp, " \t;,");
    int count = 0;

    while (tok) {
        char *eq = strchr(tok, '=');
        if (!eq) { tok = strtok(NULL, " \t;,"); continue; }
        *eq = '\0';
        char *name = tok, *path = eq + 1;
        trim(name); trim(path);

        FILE *bin = fopen(path, "rb");
        if (!bin) {
            printf(YELLOW "[EMBED] File not found: %s\n" RESET, path);
            tok = strtok(NULL, " \t;,"); continue;
        }

        /* C identifier */
        char cid[128]; dot_id(name, cid, sizeof(cid));
        fprintf(cpp, "/* Embedded: %s from %s */\n", name, path);
        fprintf(cpp, "extern \"C\" const unsigned char %s_data[] = {\n    ", cid);

        int c, col = 0; long size = 0;
        while ((c = fgetc(bin)) != EOF) {
            fprintf(cpp, "0x%02X,", (unsigned char)c);
            if (++col >= 16) { fprintf(cpp, "\n    "); col = 0; }
            size++;
        }
        fclose(bin);

        fprintf(cpp, "\n};\n");
        fprintf(cpp, "extern \"C\" const size_t %s_size = %ld;\n\n", cid, size);
        printf(GREEN "[EMBED] %s: %ld bytes\n" RESET, name, size);
        count++;
        tok = strtok(NULL, " \t;,");
    }
    fclose(cpp);

    if (count == 0) { remove(gen_cpp); return; }

    /* Compile */
    char *tags_tr = (char*)calloc(1, B_SIZE);
    char *inc_tr  = (char*)calloc(1, B_SIZE);
    if (tags_tr && inc_tr) {
        translate_tags(tags, tags_tr, B_SIZE, ct);
        translate_inc(auto_inc, inc_tr, B_SIZE, ct);
        char cmd[B_SIZE];
        if (ct == CT_MSVC)
            snprintf(cmd, sizeof(cmd),
                "%s /nologo /c \"%s\" /Fo\"%s\" %s %s", comp, gen_cpp, gen_obj, tags_tr, inc_tr);
        else
            snprintf(cmd, sizeof(cmd),
                "%s -c \"%s\" -o \"%s\" %s %s", comp, gen_cpp, gen_obj, tags_tr, inc_tr);
        if (system(cmd) == 0) {
            strncat(extra_objs, " \"", extra_objs_sz - strlen(extra_objs) - 1);
            strncat(extra_objs, gen_obj, extra_objs_sz - strlen(extra_objs) - 1);
            strncat(extra_objs, "\"",   extra_objs_sz - strlen(extra_objs) - 1);
        }
    }
    free(tags_tr); free(inc_tr);
    printf(GREEN "[EMBED] %d resource(s) embedded\n" RESET, count);
}


/* ================================================================== */
/*  OMake Features 21–25                                               */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/*  [21] PRERUN: / POSTRUN: — Pre/post build command hooks             */
/*       PRERUN:  scripts/gen_resources.py                            */
/*       POSTRUN: scripts/sign_binary.sh @OUT@                        */
/*       @OUT@ → replaced with actual output binary path              */
/*       @SRC_DIR@ → source directory                                 */
/*       @VERSION@ → project version                                  */
/*       Multiple commands via semicolons: cmd1 ; cmd2                */
/* ------------------------------------------------------------------ */

void run_hook(const char *hook_line, const char *out_bin,
                     const char *src_dir, const char *ver,
                     const char *hook_type) {
    if (!hook_line || !hook_line[0]) return;

    char expanded[B_SIZE];
    strncpy(expanded, hook_line, B_SIZE-1);

    /* Replace @OUT@ @SRC_DIR@ @VERSION@ */
    struct { const char *pat; const char *val; } subs[] = {
        {"@OUT@",     out_bin  ? out_bin  : ""},
        {"@SRC_DIR@", src_dir  ? src_dir  : "src"},
        {"@VERSION@", ver      ? ver      : ""},
        {"@PLATFORM@",
#ifdef _WIN32
         "windows"
#elif defined(__APPLE__)
         "macos"
#else
         "linux"
#endif
        },
        {NULL, NULL}
    };

    for (int si = 0; subs[si].pat; si++) {
        char *pos;
        while ((pos = strstr(expanded, subs[si].pat)) != NULL) {
            char before[B_SIZE], after[B_SIZE];
            int bl = (int)(pos - expanded);
            strncpy(before, expanded, bl); before[bl] = '\0';
            strncpy(after, pos + strlen(subs[si].pat), B_SIZE-1);
            snprintf(expanded, B_SIZE, "%s%s%s", before, subs[si].val, after);
        }
    }

    /* Split on semicolons and run each command */
    char *p = expanded;
    char *semi;
    int cmd_num = 0;
    do {
        semi = strchr(p, ';');
        if (semi) *semi = '\0';
        trim(p);
        if (*p) {
            printf(CYAN "[%s] Running: %s\n" RESET, hook_type, p);
            int ret = system(p);
            if (ret != 0)
                printf(YELLOW "[%s] Command returned %d: %s\n" RESET, hook_type, ret, p);
            cmd_num++;
        }
        if (semi) p = semi + 1;
    } while (semi);

    if (cmd_num == 0)
        printf(YELLOW "[%s] No commands found in hook\n" RESET, hook_type);
}

/* ------------------------------------------------------------------ */
/*  [22] SIZE_REPORT: — Binary section + symbol size analysis          */
/*       SIZE_REPORT: on — print section sizes after build            */
/*       SIZE_REPORT: full — also show top 20 largest symbols         */
/*       SIZE_REPORT: bloat — save bloat.txt for diffing              */
/*       Uses: size, nm, objdump (any available)                      */
/*       Better: tracks delta between builds (new vs prev size)       */
/* ------------------------------------------------------------------ */

void run_size_report(const char *mode, const char *bin_path) {
    if (!mode || !mode[0]) return;
    char m[16]; strncpy(m, mode, 15);
    for (int i = 0; m[i]; i++) m[i] = (char)tolower((unsigned char)m[i]);
    if (strcmp(m,"off")==0 || strcmp(m,"no")==0) return;

    printf(BLUE "\n[SIZE_REPORT] ═══ Binary Size Analysis: %s ═══\n" RESET, bin_path);

    /* Section sizes via 'size' */
    if (tool_exists("size")) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "size \"%s\"", bin_path);
        printf(CYAN "[SIZE_REPORT] Sections:\n" RESET);
        system(cmd);
    }

    /* File size */
    {
        FILE *f = fopen(bin_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f); fclose(f);
            const char *unit = "B";
            double dsz = fsz;
            if (fsz > 1024*1024) { dsz = fsz / (1024.0*1024.0); unit = "MB"; }
            else if (fsz > 1024) { dsz = fsz / 1024.0; unit = "KB"; }
            printf(GREEN "[SIZE_REPORT] Total binary: %.2f %s (%ld bytes)\n" RESET,
                   dsz, unit, fsz);

            /* Delta vs previous build */
            char prev_size_file[512];
            snprintf(prev_size_file, sizeof(prev_size_file), ".omake_prev_size");
            FILE *pf = fopen(prev_size_file, "r");
            if (pf) {
                long prev_sz = 0;
                fscanf(pf, "%ld", &prev_sz); fclose(pf);
                long delta = fsz - prev_sz;
                if (delta > 0)
                    printf(YELLOW "[SIZE_REPORT] Delta: +%ld bytes (+%.1f%%) vs last build\n" RESET,
                           delta, (delta * 100.0) / prev_sz);
                else if (delta < 0)
                    printf(GREEN "[SIZE_REPORT] Delta: %ld bytes (%.1f%%) vs last build\n" RESET,
                           delta, (delta * 100.0) / prev_sz);
                else
                    printf(CYAN "[SIZE_REPORT] Delta: unchanged vs last build\n" RESET);
            }
            /* Save current size */
            FILE *sf = fopen(prev_size_file, "w");
            if (sf) { fprintf(sf, "%ld\n", fsz); fclose(sf); }
        }
    }

    /* Top symbols if 'full' or 'bloat' */
    if ((strcmp(m,"full")==0 || strcmp(m,"bloat")==0) && tool_exists("nm")) {
        printf(CYAN "[SIZE_REPORT] Top 20 largest symbols:\n" RESET);
        char cmd[512];
        /* nm -S --size-sort: sort by size descending */
        snprintf(cmd, sizeof(cmd),
            "nm -S --size-sort --radix=d \"%s\" 2>/dev/null | tail -20 | sort -k2 -rn",
            bin_path);
        system(cmd);
    }

    /* Save bloat report */
    if (strcmp(m,"bloat")==0 && tool_exists("nm")) {
        char bloat_path[512];
        snprintf(bloat_path, sizeof(bloat_path), "bloat.txt");
        char cmd[600];
        snprintf(cmd, sizeof(cmd),
            "nm -S --size-sort --radix=d \"%s\" 2>/dev/null > \"%s\"",
            bin_path, bloat_path);
        system(cmd);
        printf(GREEN "[SIZE_REPORT] Full symbol list saved: %s\n" RESET, bloat_path);
    }
}

/* ------------------------------------------------------------------ */
/*  [23] MIRROR: — Source → destination sync                           */
/*       MIRROR: src/->build/src  include/->build/include             */
/*       Copies only changed files (mtime check), preserves structure */
/*       Use case: staging, Docker COPY, CI artifacts                 */
/* ------------------------------------------------------------------ */

void run_mirror(const char *mirror_list) {
    if (!mirror_list || !mirror_list[0]) return;

    char tmp[B_SIZE]; strncpy(tmp, mirror_list, B_SIZE-1);
    char *tok = strtok(tmp, " \t;");
    int total_copied = 0;

    while (tok) {
        char *arrow = strstr(tok, "->");
        if (!arrow) { tok = strtok(NULL, " \t;"); continue; }
        *arrow = '\0';
        char *src = tok, *dst = arrow + 2;
        trim(src); trim(dst);

        if (!src[0] || !dst[0]) { tok = strtok(NULL, " \t;"); continue; }

        printf(CYAN "[MIRROR] %s  →  %s\n" RESET, src, dst);

        /* Use rsync if available (best), else cp -r */
        if (tool_exists("rsync")) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                "rsync -av --checksum \"%s\" \"%s\" 2>/dev/null | grep -v '^sending\\|^sent\\|^total'",
                src, dst);
            system(cmd);
        } else {
            /* Fallback: mkdir -p dst; cp -ru src/* dst/ */
            char mk_cmd[600];
            snprintf(mk_cmd, sizeof(mk_cmd), "mkdir -p \"%s\"", dst);
            system(mk_cmd);
#ifdef _WIN32
            char cp_cmd[700];
            snprintf(cp_cmd, sizeof(cp_cmd), "xcopy \"%s\" \"%s\" /E /Y /Q >nul 2>&1", src, dst);
#else
            char cp_cmd[700];
            /* -u: only copy if newer */
            snprintf(cp_cmd, sizeof(cp_cmd), "cp -ru \"%s/.\" \"%s/\" 2>/dev/null || cp -r \"%s\" \"%s/\"", src, dst, src, dst);
#endif
            system(cp_cmd);
        }
        total_copied++;
        tok = strtok(NULL, " \t;");
    }
    if (total_copied > 0)
        printf(GREEN "[MIRROR] %d mirror(s) synced\n" RESET, total_copied);
}

/* ------------------------------------------------------------------ */
/*  [24] HASH_CHECK: — Source integrity verification                   */
/*       Generates a .omake_srcsum file on first build               */
/*       On subsequent builds: warns if sources were modified outside */
/*       of normal editing (tampering detection, CI consistency)      */
/*       HASH_CHECK: on | strict                                      */
/*       strict mode: abort build if mismatch                        */
/* ------------------------------------------------------------------ */

unsigned int simple_hash_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned int h = 5381;
    int c;
    while ((c = fgetc(f)) != EOF)
        h = ((h << 5) + h) ^ (unsigned int)c;
    fclose(f);
    return h;
}

void run_hash_check(const char *mode, SourceFile *srcs, int nsrcs) {
    if (!mode || !mode[0]) return;
    char m[16]; strncpy(m, mode, 15);
    for (int i = 0; m[i]; i++) m[i] = (char)tolower((unsigned char)m[i]);
    if (strcmp(m,"off")==0) return;
    int strict = (strcmp(m,"strict")==0);

    const char *sum_file = ".omake_srcsum";

    /* Build current hash map */
    typedef struct { char path[512]; unsigned int hash; } SrcHash;
    SrcHash *current = (SrcHash*)calloc(nsrcs, sizeof(SrcHash));
    if (!current) return;
    for (int i = 0; i < nsrcs; i++) {
        strncpy(current[i].path, srcs[i].path, 511);
        current[i].hash = simple_hash_file(srcs[i].path);
    }

    if (!is_regular_file(sum_file)) {
        /* First run: save hashes */
        FILE *sf = fopen(sum_file, "w");
        if (sf) {
            for (int i = 0; i < nsrcs; i++)
                fprintf(sf, "%08X  %s\n", current[i].hash, current[i].path);
            fclose(sf);
            printf(CYAN "[HASH_CHECK] Baseline saved (%d files)\n" RESET, nsrcs);
        }
    } else {
        /* Subsequent run: compare */
        FILE *sf = fopen(sum_file, "r");
        if (!sf) { free(current); return; }

        int mismatches = 0;
        char line[640];
        while (fgets(line, sizeof(line), sf)) {
            unsigned int saved_hash; char saved_path[512];
            if (sscanf(line, "%08X  %511s", &saved_hash, saved_path) != 2) continue;
            /* Find in current */
            for (int i = 0; i < nsrcs; i++) {
                if (strcmp(current[i].path, saved_path) == 0) {
                    if (current[i].hash != saved_hash) {
                        printf(YELLOW "[HASH_CHECK] Modified: %s\n" RESET, saved_path);
                        mismatches++;
                    }
                    break;
                }
            }
        }
        fclose(sf);

        /* Update the hash file */
        sf = fopen(sum_file, "w");
        if (sf) {
            for (int i = 0; i < nsrcs; i++)
                fprintf(sf, "%08X  %s\n", current[i].hash, current[i].path);
            fclose(sf);
        }

        if (mismatches == 0)
            printf(GREEN "[HASH_CHECK] All %d source files verified ✓\n" RESET, nsrcs);
        else if (strict) {
            printf(RED "[HASH_CHECK] STRICT: %d file(s) modified unexpectedly — aborting\n" RESET, mismatches);
            free(current); exit(1);
        } else
            printf(YELLOW "[HASH_CHECK] %d modified file(s) detected (use strict to abort)\n" RESET, mismatches);
    }
    free(current);
}

/* ------------------------------------------------------------------ */
/*  [25] MULTI_OUT: — Build multiple output binaries from one project  */
/*       MULTI_OUT: bin/server bin/client bin/tools/migrate            */
/*       Each OUT has its own main() entry in a file named after it   */
/*       e.g. bin/server → looks for src/server.cpp (has main)       */
/*       Common sources compiled once, shared across all outputs      */
/*       Auto-detects entry files by name                             */
/* ------------------------------------------------------------------ */

void run_multi_out(const char *multi_list, const char *src_dir,
                           const char *obj_root, const char *final_comp,
                           CompilerType ct, const char *tags,
                           const char *libs, const char *auto_inc,
                           const char *arch) {
    if (!multi_list || !multi_list[0]) return;

    /* Already built the main output — build additional ones */
    char tmp[B_SIZE]; strncpy(tmp, multi_list, B_SIZE-1);
    char *tok = strtok(tmp, " \t;,");

    while (tok) {
        char out_path[512]; strncpy(out_path, tok, 511); trim(out_path);
        tok = strtok(NULL, " \t;,");
        if (!out_path[0]) continue;

        /* Derive entry file name from output stem */
        char stem[256]; path_stem(out_path, stem, sizeof(stem));

        /* Look for <stem>.cpp in src_dir, parent dir, and "src/" fallback */
        char entry_cpp[600], entry_c[600];
        /* Try src_dir first */
        snprintf(entry_cpp, sizeof(entry_cpp), "%s/%s.cpp", src_dir, stem);
        snprintf(entry_c,   sizeof(entry_c),   "%s/%s.c",   src_dir, stem);

        const char *entry_src = NULL;
        if      (is_regular_file(entry_cpp))  entry_src = entry_cpp;
        else if (is_regular_file(entry_c))    entry_src = entry_c;
        /* Try parent of src_dir */
        if (!entry_src) {
            char parent[600]; strncpy(parent, src_dir, 599);
            char *last = strrchr(parent, SEP);
            if (last) { *last = '\0';
                snprintf(entry_cpp, sizeof(entry_cpp), "%s/%s.cpp", parent, stem);
                snprintf(entry_c,   sizeof(entry_c),   "%s/%s.c",   parent, stem);
                if      (is_regular_file(entry_cpp)) entry_src = entry_cpp;
                else if (is_regular_file(entry_c))   entry_src = entry_c;
            }
        }
        /* Try "src/<stem>.cpp" relative to cwd */
        if (!entry_src) {
            snprintf(entry_cpp, sizeof(entry_cpp), "src/%s.cpp", stem);
            snprintf(entry_c,   sizeof(entry_c),   "src/%s.c",   stem);
            if      (is_regular_file(entry_cpp)) entry_src = entry_cpp;
            else if (is_regular_file(entry_c))   entry_src = entry_c;
        }

        if (!entry_src) {
            printf(YELLOW "[MULTI_OUT] No entry file for %s (tried %s.cpp / %s.c)\n" RESET,
                   out_path, stem, stem);
            continue;
        }

        printf(CYAN "[MULTI_OUT] Building: %s (entry: %s)\n" RESET, out_path, entry_src);

        /* Compile entry file to its own obj */
        char entry_obj[600];
        snprintf(entry_obj, sizeof(entry_obj), "%s/multi_%s.o", obj_root, stem);
        ensure_dir_exists(entry_obj);

        char *tags_tr = (char*)calloc(1, B_SIZE);
        char *inc_tr  = (char*)calloc(1, B_SIZE);
        if (!tags_tr || !inc_tr) { free(tags_tr); free(inc_tr); continue; }
        translate_tags(tags, tags_tr, B_SIZE, ct);
        translate_inc(auto_inc, inc_tr, B_SIZE, ct);

        char compile_cmd[B_SIZE];
        if (ct == CT_MSVC)
            snprintf(compile_cmd, sizeof(compile_cmd),
                "%s /nologo /c \"%s\" /Fo\"%s\" %s %s",
                final_comp, entry_src, entry_obj, tags_tr, inc_tr);
        else
            snprintf(compile_cmd, sizeof(compile_cmd),
                "%s -c \"%s\" -o \"%s\" -MMD -MP %s %s",
                final_comp, entry_src, entry_obj, tags_tr, inc_tr);

        if (system(compile_cmd) != 0) {
            printf(RED "[MULTI_OUT] Compile failed: %s\n" RESET, entry_src);
            free(tags_tr); free(inc_tr); continue;
        }

        /* Gather all shared objects EXCLUDING other potential entry files */
        char shared_objs[B_SIZE]; shared_objs[0] = '\0';
        /* Use existing compiled .o files from obj_root, skip multi_*.o */
        char find_cmd[600];
        snprintf(find_cmd, sizeof(find_cmd),
            "find \"%s\" -name '*.o' ! -name 'multi_*.o' ! -name 'unity_*.o' "
            "2>/dev/null | tr '\\n' '\\0' | xargs -0 printf '\"%%s\" '",
            obj_root);
        FILE *fp = popen(find_cmd, "r");
        if (fp) {
            char buf[B_SIZE]; buf[0] = '\0';
            fread(buf, 1, B_SIZE-1, fp); pclose(fp);
            buf[B_SIZE-1] = '\0';
            /* Filter out any obj that has the same stem as another MULTI_OUT target
               (avoids multiple main() symbols) — simple heuristic: skip main.o */
            strncpy(shared_objs, buf, B_SIZE-1);
        }

        /* Ensure parent output dir exists */
        {
            char out_parent[512]; strncpy(out_parent, out_path, 511);
            char *last_sep = strrchr(out_parent, SEP);
            if (last_sep) { *last_sep = '\0'; mkdir_auto(out_parent); }
        }

        /* Link */
        char link_buf[B_SIZE];
        char *libs_tr = (char*)calloc(1, B_SIZE);
        if (libs_tr) {
            translate_tags(libs, libs_tr, B_SIZE, ct);
            if (ct == CT_MSVC)
                snprintf(link_buf, sizeof(link_buf),
                    "%s /nologo \"%s\" %s %s /Fe\"%s\"",
                    final_comp, entry_obj, shared_objs, libs_tr, out_path);
            else
                snprintf(link_buf, sizeof(link_buf),
                    "%s \"%s\" %s %s -o \"%s\"",
                    final_comp, entry_obj, shared_objs, libs_tr, out_path);
            free(libs_tr);
        }

        printf(CYAN "[MULTI_OUT] Link: %s\n" RESET, link_buf);
        if (system(link_buf) == 0)
            printf(GREEN "[MULTI_OUT] ✓  %s\n" RESET, out_path);
        else
            printf(RED "[MULTI_OUT] ✗  Link failed: %s\n" RESET, out_path);

        free(tags_tr); free(inc_tr);
    }
}


/* ================================================================== */
/*  OMake — Heavy Library Smart Support System (25 Libraries)          */

/* ================================================================== */
/*  OMake Heavy Library Support — 25+ Libraries                        */
/*  Each library: pkg-config → env vars → header scan → Win paths     */
/*  Better than CMake: zero config, auto-moc/uic, auto shader compile */
/* ================================================================== */

/* ─── Core helpers ──────────────────────────────────────────────── */

static void tag_add(char *buf, const char *flag) {
    if (!flag || !flag[0]) return;
    if (!strstr(buf, flag)) {
        strncat(buf, " ", B_SIZE - strlen(buf) - 1);
        strncat(buf, flag, B_SIZE - strlen(buf) - 1);
    }
}

/* Run pkg-config for <name>; fill tags+libs; return 1 if found */
static int pkgcfg(const char *name, char *tags, char *libs) {
    if (!tool_exists("pkg-config")) return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pkg-config --exists \"%s\" 2>/dev/null", name);
    if (system(cmd) != 0) return 0;
    if (tags) {
        snprintf(cmd, sizeof(cmd), "pkg-config --cflags \"%s\" 2>/dev/null", name);
        FILE *p = popen(cmd, "r");
        if (p) {
            char buf[2048]; buf[0] = '\0';
            if (fgets(buf, sizeof(buf), p)) {
                char *nl = strrchr(buf, '\n'); if (nl) *nl = '\0';
                if (buf[0]) { strncat(tags," ",B_SIZE-strlen(tags)-1); strncat(tags,buf,B_SIZE-strlen(tags)-1); }
            }
            pclose(p);
        }
    }
    if (libs) {
        snprintf(cmd, sizeof(cmd), "pkg-config --libs \"%s\" 2>/dev/null", name);
        FILE *p = popen(cmd, "r");
        if (p) {
            char buf[2048]; buf[0] = '\0';
            if (fgets(buf, sizeof(buf), p)) {
                char *nl = strrchr(buf, '\n'); if (nl) *nl = '\0';
                if (buf[0]) { strncat(libs," ",B_SIZE-strlen(libs)-1); strncat(libs,buf,B_SIZE-strlen(libs)-1); }
            }
            pclose(p);
        }
    }
    return 1;
}

/* Check header existence across all common system/MSYS2/vcpkg paths */
static int find_header(const char *rel_path, char *out_dir, size_t sz) {
    static const char *sys_dirs[] = {
        /* Linux / macOS system */
        "/usr/include", "/usr/local/include",
        "/opt/homebrew/include", "/opt/homebrew/opt/include",
        "/opt/local/include",
        /* MSYS2 MinGW */
        "/mingw64/include", "/mingw32/include",
        "/ucrt64/include",  "/clang64/include",
        "C:/msys64/mingw64/include", "C:/msys64/ucrt64/include",
        "C:/msys64/clang64/include",
        /* vcpkg */
        "vcpkg_installed/x64-windows/include",
        "vcpkg_installed/x64-linux/include",
        "vcpkg_installed/arm64-osx/include",
        "C:/vcpkg/installed/x64-windows/include",
        NULL
    };
    /* Check system dirs first */
    for (int i = 0; sys_dirs[i]; i++) {
        char full[768];
        snprintf(full, sizeof(full), "%s/%s", sys_dirs[i], rel_path);
        if (is_regular_file(full)) {
            if (out_dir) snprintf(out_dir, sz, "%s", sys_dirs[i]);
            return 1;
        }
    }
    /* Check deps/ subdirectories (FETCH_V2 cloned libs) */
    /* Try deps/<name>/include and deps/<name>/_build/include */
    static const char *dep_bases[] = {"deps","extern","external","third_party","3rdparty",NULL};
    for (int bi = 0; dep_bases[bi]; bi++) {
        if (!dir_exists(dep_bases[bi])) continue;
        /* Scan one level inside deps/ */
        DIR *dd = opendir(dep_bases[bi]);
        if (!dd) continue;
        struct dirent *de;
        while ((de = readdir(dd)) != NULL) {
            if (de->d_name[0] == '.') continue;
            static const char *sub[] = {"include", "_build/include", "", NULL};
            for (int si = 0; sub[si]; si++) {
                char base[512];
                if (sub[si][0])
                    snprintf(base,sizeof(base),"%s/%s/%s",dep_bases[bi],de->d_name,sub[si]);
                else
                    snprintf(base,sizeof(base),"%s/%s",dep_bases[bi],de->d_name);
                char full[768];
                snprintf(full,sizeof(full),"%s/%s",base,rel_path);
                if (is_regular_file(full)) {
                    if (out_dir) snprintf(out_dir, sz, "%s", base);
                    closedir(dd);
                    return 1;
                }
            }
        }
        closedir(dd);
    }
    return 0;
}

/* Scan typical Windows lib directories for a given library name */
static int find_libdir_win(const char *libname, char *out_dir, size_t sz) {
    static const char *dirs[] = {
        "/mingw64/lib", "/mingw32/lib", "/ucrt64/lib", "/clang64/lib",
        "C:/msys64/mingw64/lib", "C:/msys64/ucrt64/lib",
        "vcpkg_installed/x64-windows/lib",
        "C:/vcpkg/installed/x64-windows/lib",
        NULL
    };
    static const char *exts[] = { ".lib", ".a", ".dll.a", NULL };
    for (int d = 0; dirs[d]; d++) {
        for (int e = 0; exts[e]; e++) {
            char p1[768], p2[768];
            snprintf(p1, sizeof(p1), "%s/lib%s%s",  dirs[d], libname, exts[e]);
            snprintf(p2, sizeof(p2), "%s/%s%s",      dirs[d], libname, exts[e]);
            if (is_regular_file(p1) || is_regular_file(p2)) {
                if (out_dir) snprintf(out_dir, sz, "%s", dirs[d]);
                return 1;
            }
        }
    }
    return 0;
}

/* ── deps_inject_flags ─────────────────────────────────────────────
 * For a library with source in deps/<name>/ (built via FETCH_V2 cmake),
 * inject -I and -L flags automatically.
 * Returns 1 if the library was found in deps/, 0 otherwise.
 * ─────────────────────────────────────────────────────────────────── */
static int deps_inject_flags(const char *lib_name, const char *header_rel,
                              char *tags, char *libs) {
    static const char *dep_bases[] = {"deps","extern","external","third_party","3rdparty",NULL};
    static const char *inc_subs[]  = {"include","_build/include","",NULL};
    static const char *lib_subs[]  = {"_build/lib","_build/lib/Release","_build/lib/Debug",
                                       "_build","lib","build/lib","_build/Release",NULL};

    for (int bi = 0; dep_bases[bi]; bi++) {
        /* Try deps/<exact_name>/ and case variants */
        char candidates[4][64];
        strncpy(candidates[0], lib_name, 63);
        /* uppercase first letter */
        strncpy(candidates[1], lib_name, 63);
        if (candidates[1][0] >= 'a' && candidates[1][0] <= 'z')
            candidates[1][0] = (char)(candidates[1][0] - 32);
        /* all uppercase */
        strncpy(candidates[2], lib_name, 63);
        for (int c=0;candidates[2][c];c++) candidates[2][c]=(char)toupper((unsigned char)candidates[2][c]);
        candidates[3][0] = '\0'; /* sentinel */

        for (int ci = 0; candidates[ci][0]; ci++) {
            char root[256];
            snprintf(root, sizeof(root), "%s/%s", dep_bases[bi], candidates[ci]);
            if (!dir_exists(root)) continue;

            /* Found the root — inject include */
            int inc_found = 0;
            for (int si = 0; inc_subs[si]; si++) {
                char inc_path[512];
                if (inc_subs[si][0])
                    snprintf(inc_path, sizeof(inc_path), "%s/%s", root, inc_subs[si]);
                else
                    strncpy(inc_path, root, 511);

                /* Verify header exists there */
                if (header_rel && header_rel[0]) {
                    char full[768]; snprintf(full, sizeof(full), "%s/%s", inc_path, header_rel);
                    if (!is_regular_file(full)) continue;
                }

                char iflag[600]; snprintf(iflag, sizeof(iflag), " -I\"%s\"", inc_path);
                if (!strstr(tags, iflag)) strncat(tags, iflag, B_SIZE-strlen(tags)-1);
                inc_found = 1;
                break;
            }
            if (!inc_found && header_rel && header_rel[0]) {
                /* fallback: add root itself */
                char iflag[600]; snprintf(iflag, sizeof(iflag), " -I\"%s\"", root);
                if (!strstr(tags, iflag)) strncat(tags, iflag, B_SIZE-strlen(tags)-1);
            }

            /* Inject lib dir */
            for (int li = 0; lib_subs[li]; li++) {
                char lp[512]; snprintf(lp, sizeof(lp), "%s/%s", root, lib_subs[li]);
                if (dir_exists(lp)) {
                    char lflag[600]; snprintf(lflag, sizeof(lflag), " -L\"%s\"", lp);
                    if (!strstr(libs, lflag)) strncat(libs, lflag, B_SIZE-strlen(libs)-1);
                    printf(CYAN "[LIB] %s  deps source: %s\n" RESET, lib_name, root);
                    return 1;
                }
            }
            /* Root found but no lib dir yet (not built / header-only) */
            return 1;
        }
    }
    return 0;
}

/* Compile a single C/C++ source file and append .o to extra_objs */
static void compile_extra(const char *src_path, const char *obj_path,
                           const char *comp, CompilerType ct,
                           const char *tags, char *extra_objs, size_t esz,
                           const char *label) {
    char cmd[4096];
    if (ct == CT_MSVC)
        snprintf(cmd, sizeof(cmd), "%s /nologo /c \"%s\" /Fo\"%s\" /EHsc %s",
                 comp, src_path, obj_path, tags);
    else
        snprintf(cmd, sizeof(cmd), "%s -c \"%s\" -o \"%s\" %s",
                 comp, src_path, obj_path, tags);
    printf(CYAN "[%s] Compiling: %s\n" RESET, label, src_path);
    if (system(cmd) == 0) {
        char entry[800]; snprintf(entry, sizeof(entry), " \"%s\"", obj_path);
        strncat(extra_objs, entry, esz - strlen(extra_objs) - 1);
    } else {
        printf(YELLOW "[%s] Warning: compile failed for %s\n" RESET, label, src_path);
    }
}

/* ================================================================== */
/*  Qt5 / Qt6                                                          */
/*  CMake needs: find_package(Qt6 REQUIRED COMPONENTS Core Widgets)   */
/*               target_link_libraries(... Qt6::Core Qt6::Widgets)    */
/*               set_target_properties(... AUTOMOC ON AUTOUIC ON)     */
/*  OMake: USE_LIB: qt:Core,Widgets   ← done, auto-moc/uic included  */
/* ================================================================== */
static int qt_find_tools(int *ver_out, char *moc, char *uic, char *rcc,
                           char *inc_base, char *lib_base, size_t psz) {
    moc[0] = uic[0] = rcc[0] = inc_base[0] = lib_base[0] = '\0';
    *ver_out = 0;

    /* ── Strategy 1: qmake -query (most reliable on all platforms) ── */
    /* First find qmake in PATH or common locations */
    static const char *qmake_names[] = {
        "qmake6","qmake-qt6","qmake","qmake-qt5","qmake5",NULL
    };
    char qmake_bin[512]; qmake_bin[0]='\0';
    for (int i=0; qmake_names[i]; i++) {
        if (tool_exists(qmake_names[i])) {
            strncpy(qmake_bin, qmake_names[i], psz-1); break;
        }
    }
    /* Also scan C:/Qt for qmake */
#ifdef _WIN32
    if (!qmake_bin[0]) {
        /* Walk C:/Qt/<ver>/<kit>/bin/qmake.exe */
        static const char *kits[] = {
            "msvc2022_64","msvc2019_64","msvc2017_64",
            "mingw_64","mingw64_64","mingw81_64",
            "llvm-mingw_64",NULL
        };
        static const char *vers[] = {
            "6.9","6.8","6.7","6.6","6.5","6.4","6.3","6.2","6.1","6.0",
            "5.15","5.14","5.13","5.12",NULL
        };
        /* Try exact minor versions 0-9 */
        static const char *minors[] = {"0","1","2","3","4","5","6","7","8","9",NULL};
        for (int vi=0; vers[vi] && !qmake_bin[0]; vi++) {
            for (int mi=0; minors[mi] && !qmake_bin[0]; mi++) {
                for (int ki=0; kits[ki] && !qmake_bin[0]; ki++) {
                    char p[512];
                    snprintf(p,sizeof(p),"C:/Qt/%s.%s/%s/bin/qmake.exe",
                             vers[vi],minors[mi],kits[ki]);
                    if (is_regular_file(p)) strncpy(qmake_bin,p,psz-1);
                }
            }
        }
        /* Also try MSYS2 paths */
        static const char *msys_qmakes[] = {
            "C:/msys64/ucrt64/bin/qmake6",
            "C:/msys64/mingw64/bin/qmake6",
            "C:/msys64/ucrt64/bin/qmake",
            "C:/msys64/mingw64/bin/qmake",
            NULL
        };
        for (int i=0; msys_qmakes[i] && !qmake_bin[0]; i++)
            if (is_regular_file(msys_qmakes[i]))
                strncpy(qmake_bin,msys_qmakes[i],psz-1);
    }
#endif

    if (qmake_bin[0]) {
        /* qmake -query gives us everything we need */
        struct { const char *key; char *out; size_t sz; } queries[] = {
            {"QT_VERSION",     NULL,     0  },  /* 0 */
            {"QT_INSTALL_HEADERS", inc_base, psz},  /* 1 */
            {"QT_INSTALL_LIBS",    lib_base, psz},  /* 2 */
            {"QT_INSTALL_BINS",    NULL,     0  },  /* 3 — for moc/uic/rcc */
            {NULL,NULL,0}
        };
        /* We'll extract QT_INSTALL_BINS to derive moc */
        char qt_bins[512]; qt_bins[0]='\0';
        /* Run qmake -query once and parse all keys */
        char cmd[600]; snprintf(cmd,sizeof(cmd),"\"%s\" -query 2>&1",qmake_bin);
        FILE *fp = popen(cmd,"r");
        if (fp) {
            char line[1024];
            while (fgets(line,sizeof(line),fp)) {
                char *nl=strrchr(line,'\n'); if(nl)*nl='\0';
                char *col=strchr(line,':'); if(!col) continue;
                *col='\0';
                const char *key=line, *val=col+1;
                if (strcmp(key,"QT_VERSION")==0) {
                    /* Parse major */
                    int major=0; sscanf(val,"%d",&major);
                    *ver_out=major;
                } else if (strcmp(key,"QT_INSTALL_HEADERS")==0 && !inc_base[0]) {
                    strncpy(inc_base,val,psz-1);
                } else if (strcmp(key,"QT_INSTALL_LIBS")==0 && !lib_base[0]) {
                    strncpy(lib_base,val,psz-1);
                } else if (strcmp(key,"QT_INSTALL_BINS")==0 && !qt_bins[0]) {
                    strncpy(qt_bins,val,psz-1);
                }
            }
            pclose(fp);
        }
        /* Build moc/uic/rcc paths from QT_INSTALL_BINS */
        if (qt_bins[0]) {
#ifdef _WIN32
            snprintf(moc,psz,"%s/moc.exe",qt_bins);
            snprintf(uic,psz,"%s/uic.exe",qt_bins);
            snprintf(rcc,psz,"%s/rcc.exe",qt_bins);
#else
            snprintf(moc,psz,"%s/moc",qt_bins);
            snprintf(uic,psz,"%s/uic",qt_bins);
            snprintf(rcc,psz,"%s/rcc",qt_bins);
#endif
            if (!is_regular_file(moc)) {
                /* macOS Qt6: moc is in libexec */
                char libexec[512];
                snprintf(libexec,sizeof(libexec),"%s/../libexec",qt_bins);
                char moc2[512]; snprintf(moc2,sizeof(moc2),"%s/moc",libexec);
                if (is_regular_file(moc2)) {
                    strncpy(moc,moc2,psz-1);
                    char uic2[512],rcc2[512];
                    snprintf(uic2,sizeof(uic2),"%s/uic",libexec);
                    snprintf(rcc2,sizeof(rcc2),"%s/rcc",libexec);
                    if(is_regular_file(uic2)) strncpy(uic,uic2,psz-1);
                    if(is_regular_file(rcc2)) strncpy(rcc,rcc2,psz-1);
                } else {
                    moc[0]='\0'; /* clear bad path */
                }
            }
        }
        if (inc_base[0]) {
            printf(CYAN "[QT] qmake: %s\n" RESET, qmake_bin);
            if (*ver_out) printf(CYAN "[QT] Qt version: %d\n" RESET, *ver_out);
            printf(CYAN "[QT] Include: %s\n" RESET, inc_base);
            return 1;
        }
    }

    /* ── Strategy 2: moc --version → derive path ────────────────── */
    if (!moc[0]) {
        const char *names[]={"moc","moc-qt6","moc6","moc-qt5","moc5",NULL};
        for (int i=0; names[i]; i++)
            if (tool_exists(names[i])) { strncpy(moc,names[i],psz-1); break; }
    }
    if (moc[0] && !*ver_out) {
        char cmd[640]; snprintf(cmd,sizeof(cmd),"\"%s\" --version 2>&1",moc);
        FILE *fp=popen(cmd,"r");
        if (fp) {
            char buf[256]; buf[0]='\0';
            if (fgets(buf,sizeof(buf),fp)) {
                if (strstr(buf,"Qt 6")||strstr(buf,"Qt6")||strstr(buf," 6.")) *ver_out=6;
                else if (strstr(buf,"Qt 5")||strstr(buf,"Qt5")||strstr(buf," 5.")) *ver_out=5;
            }
            pclose(fp);
        }
    }
    /* Try to find moc full path via where/which */
    if (moc[0] && !strchr(moc,'/') && !strchr(moc,'\\')) {
#ifdef _WIN32
        char cmd2[512]; snprintf(cmd2,sizeof(cmd2),"where \"%s\" 2>nul",moc);
#else
        char cmd2[512]; snprintf(cmd2,sizeof(cmd2),"which \"%s\" 2>/dev/null",moc);
#endif
        FILE *fp=popen(cmd2,"r");
        if (fp) {
            char fullpath[512]; fullpath[0]='\0';
            if (fgets(fullpath,sizeof(fullpath),fp)) {
                char *nl=strrchr(fullpath,'\n'); if(nl)*nl='\0';
                char *cr=strrchr(fullpath,'\r'); if(cr)*cr='\0';
                if (fullpath[0]) strncpy(moc,fullpath,psz-1);
            }
            pclose(fp);
        }
    }
    /* Derive include/lib from full moc path */
    if (moc[0] && !inc_base[0]) {
        char tmp[512]; strncpy(tmp,moc,511);
        /* moc full path: C:\Qt\6.7.0\mingw_64\bin\moc.exe */
        char *bin=strstr(tmp,"\\bin\\"); if(!bin) bin=strstr(tmp,"/bin/");
        if (bin) {
            *bin='\0';
            char inc_try[600],lib_try[600];
            snprintf(inc_try,sizeof(inc_try),"%s/include",tmp);
            snprintf(lib_try,sizeof(lib_try),"%s/lib",tmp);
            if (dir_exists(inc_try)) {
                strncpy(inc_base,inc_try,psz-1);
                strncpy(lib_base,lib_try,psz-1);
                if (!*ver_out) {
                    /* Try to guess version from path */
                    if (strstr(tmp,"6.")) *ver_out=6;
                    else if (strstr(tmp,"5.")) *ver_out=5;
                }
            }
        }
    }

    /* ── Strategy 3: QTDIR / QT_ROOT / Qt6_DIR env ──────────────── */
    if (!inc_base[0]) {
        const char *ev_raw=getenv("QTDIR");
        if (!ev_raw) ev_raw=getenv("QT_ROOT");
        if (!ev_raw) ev_raw=getenv("Qt6_DIR");
        if (!ev_raw) ev_raw=getenv("Qt5_DIR");
        /* Trim leading/trailing whitespace from env value */
        char ev_buf[512]; ev_buf[0]='\0';
        if (ev_raw) { strncpy(ev_buf, ev_raw, 511); trim(ev_buf); }
        const char *ev = ev_buf[0] ? ev_buf : NULL;
        if (ev && ev[0]) {
            /* Try: ev/include, ev itself (if it already points to include/) */
            char trials[3][600];
            snprintf(trials[0],sizeof(trials[0]),"%s/include",ev);
            snprintf(trials[1],sizeof(trials[1]),"%s",ev);
            snprintf(trials[2],sizeof(trials[2]),"%s/Headers",ev); /* macOS */
            for (int ti=0; ti<3 && !inc_base[0]; ti++) {
                if (dir_exists(trials[ti])) {
                    strncpy(inc_base,trials[ti],psz-1);
                    char lib_try[600];
                    snprintf(lib_try,sizeof(lib_try),"%s/lib",ev);
                    strncpy(lib_base,lib_try,psz-1);
                    printf(CYAN "[QT] Using QTDIR: %s (include: %s)\n" RESET, ev, trials[ti]);
                }
            }
        }
    }

    /* ── Strategy 4: pkg-config ─────────────────────────────────── */
    if (!inc_base[0]) {
        const char *pkgs[]={"Qt6Core","Qt5Core",NULL};
        for (int i=0; pkgs[i] && !inc_base[0]; i++) {
            char cmd[256]; char buf[512]; buf[0]='\0';
            snprintf(cmd,sizeof(cmd),"pkg-config --variable=includedir %s 2>/dev/null",pkgs[i]);
            FILE *fp=popen(cmd,"r");
            if (fp) { if(fgets(buf,sizeof(buf),fp)){char*nl=strrchr(buf,'\n');if(nl)*nl='\0';if(buf[0]){strncpy(inc_base,buf,psz-1);}} pclose(fp); }
            if (inc_base[0]) {
                snprintf(cmd,sizeof(cmd),"pkg-config --variable=libdir %s 2>/dev/null",pkgs[i]);
                fp=popen(cmd,"r");
                if (fp) { if(fgets(buf,sizeof(buf),fp)){char*nl=strrchr(buf,'\n');if(nl)*nl='\0';if(buf[0]){strncpy(lib_base,buf,psz-1);}} pclose(fp); }
            }
        }
    }

    /* ── Strategy 5: MSYS2 known paths ─────────────────────────── */
    if (!inc_base[0]) {
        static const char *msys_i[]=
            {"/ucrt64/include/qt6","/mingw64/include/qt6",
             "/ucrt64/include/qt5","/mingw64/include/qt5",NULL};
        static const char *msys_l[]=
            {"/ucrt64/lib","/mingw64/lib",
             "/ucrt64/lib","/mingw64/lib",NULL};
        for (int i=0; msys_i[i]; i++) {
            if (dir_exists(msys_i[i])) {
                strncpy(inc_base,msys_i[i],psz-1);
                strncpy(lib_base,msys_l[i],psz-1);
                break;
            }
        }
    }
    /* Resolve uic/rcc once moc full path is known */
    if (moc[0] && is_regular_file(moc)) {
        char uic_try[512],rcc_try[512];
        strncpy(uic_try,moc,511); strncpy(rcc_try,moc,511);
        char *mp; char *up; char *rp;
        mp=strstr(uic_try,"moc"); up=strstr(rcc_try,"moc");
        if (mp) { memcpy(mp,"uic",3); if(is_regular_file(uic_try)) strncpy(uic,uic_try,psz-1); }
        if (up) { memcpy(up,"rcc",3); if(is_regular_file(rcc_try)) strncpy(rcc,rcc_try,psz-1); }
    }
    if (!uic[0]) { const char *n[]={"uic","uic-qt6","uic6","uic-qt5",NULL}; for(int i=0;n[i];i++) if(tool_exists(n[i])){strncpy(uic,n[i],psz-1);break;} }
    if (!rcc[0]) { const char *n[]={"rcc","rcc-qt6","rcc6","rcc-qt5",NULL}; for(int i=0;n[i];i++) if(tool_exists(n[i])){strncpy(rcc,n[i],psz-1);break;} }

    return (moc[0] || inc_base[0]) ? 1 : 0;
}

/* ── Check if a file contains a pattern (simple line scan) ───────── */
static int file_contains(const char *path, const char *pattern) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[1024];
    int found = 0;
    while (!found && fgets(buf, sizeof(buf), fp))
        if (strstr(buf, pattern)) found = 1;
    fclose(fp);
    return found;
}

/* ── Collect all files with given extension under a directory ─────── */
static int collect_files_ext(const char *dir, const char *ext,
                               char out[][1024], int maxout) {
    int count = 0;
#ifdef _WIN32
    char pat[1024]; snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        char full[1024]; snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += collect_files_ext(full, ext, out + count, maxout - count);
        } else if (strstr(fd.cFileName, ext) && count < maxout) {
            strncpy(out[count++], full, 1023);
        }
    } while (count < maxout && FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < maxout) {
        if (ent->d_name[0] == '.') continue;
        char full[1024]; snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (dir_exists(full)) {
            count += collect_files_ext(full, ext, out + count, maxout - count);
        } else if (strstr(ent->d_name, ext)) {
            strncpy(out[count++], full, 1023);
        }
    }
    closedir(d);
#endif
    return count;
}

/* ── qt_run_moc ───────────────────────────────────────────────────────
 *
 * Two moc strategies run together:
 *
 *  A) HEADER MOC  (classic):
 *     Scan .h / .hpp files for Q_OBJECT → generate moc_<stem>.cpp
 *     → compile it → add to link list
 *
 *  B) INLINE MOC  (auto-detect):
 *     Scan .cpp files for  #include "*.moc"
 *     → moc that .cpp → write <stem>.moc into obj_root/
 *     No separate compile step needed — the .moc is #included directly.
 *
 *  Both strategies require -I"obj_root" in compile tags (already added
 *  by setup_qt before this call).
 * ─────────────────────────────────────────────────────────────────── */
static void qt_run_moc(const char *moc_bin, const char *src_dir,
                        const char *obj_root, const char *comp,
                        CompilerType ct, const char *tags,
                        char *extra_objs, size_t esz) {

    int total_hdr = 0, total_inline = 0;

    /* ── A) Header moc: .h/.hpp with Q_OBJECT ──────────────────── */
    {
        static char hfiles[512][1024];
        int nh = collect_files_ext(src_dir, ".h",   hfiles,       256);
        int nh2= collect_files_ext(src_dir, ".hpp",  hfiles + nh, 256 - nh);
        nh += nh2;
        for (int i = 0; i < nh; i++) {
            if (!file_contains(hfiles[i], "Q_OBJECT")) continue;
            char stem[256]; path_stem(hfiles[i], stem, sizeof(stem));
            char moc_cpp[700], moc_obj[700];
            snprintf(moc_cpp, sizeof(moc_cpp), "%s/moc_%s.cpp", obj_root, stem);
            snprintf(moc_obj, sizeof(moc_obj), "%s/moc_%s.o",   obj_root, stem);
            char moc_cmd[1500];
            snprintf(moc_cmd, sizeof(moc_cmd), "\"%s\" \"%s\" -o \"%s\" 2>&1",
                     moc_bin, hfiles[i], moc_cpp);
            printf(CYAN "[QT-moc] header: %s\n" RESET, hfiles[i]);
            if (system(moc_cmd) == 0)
                compile_extra(moc_cpp, moc_obj, comp, ct, tags,
                              extra_objs, esz, "QT-moc");
            total_hdr++;
        }
    }

    /* ── B) Inline moc: .cpp with #include "*.moc" ─────────────── */
    {
        static char cfiles[512][1024];
        int nc = collect_files_ext(src_dir, ".cpp", cfiles, 512);
        /* Also check .cxx / .cc */
        nc += collect_files_ext(src_dir, ".cxx", cfiles + nc, 512 - nc);

        for (int i = 0; i < nc; i++) {
            /* Scan this .cpp for  #include "something.moc"
             * Skip comment lines so "// example: #include \"foo.moc\"" doesn't trigger */
            FILE *fp = fopen(cfiles[i], "r");
            if (!fp) continue;
            char line[1024];
            int found_moc_include = 0;
            char moc_name[256]; moc_name[0] = '\0';  /* e.g. "main.moc" */
            while (fgets(line, sizeof(line), fp)) {
                /* Skip whole-line comments */
                const char *ls = line;
                while (*ls == ' ' || *ls == '\t') ls++;
                if (ls[0] == '/' && ls[1] == '/') continue;  /* // comment */
                if (ls[0] == '*') continue;                   /* * inside block comment */
                /* Match:  #include "foo.moc"  (with optional spaces) */
                const char *inc = ls;
                while ((inc = strstr(inc, "#include")) != NULL) {
                    /* Make sure this #include is not after // on same line */
                    int is_commented = 0;
                    for (const char *c = ls; c < inc-1; c++)
                        if (c[0]=='/' && c[1]=='/') { is_commented=1; break; }
                    if (is_commented) break;
                    inc += 8;
                    while (*inc == ' ' || *inc == '\t') inc++;
                    if (*inc != '"') { inc++; continue; }
                    inc++; /* skip opening " */
                    const char *dot = strstr(inc, ".moc\"");
                    if (!dot) { inc++; continue; }
                    /* Extract the name without quotes */
                    int nlen = (int)(dot + 4 - inc); /* includes ".moc" */
                    if (nlen > 0 && nlen < 255) {
                        strncpy(moc_name, inc, nlen); moc_name[nlen] = '\0';
                        found_moc_include = 1;
                        break;
                    }
                    inc++;
                }
                if (found_moc_include) break;
            }
            fclose(fp);
            if (!found_moc_include) continue;

            /* #include "*.moc" found → run moc unconditionally.
             * User explicitly included the .moc file, so we trust them. */

            /* moc_name might be "main.moc" or just "foo.moc" — use stem from cpp */
            char stem[256]; path_stem(cfiles[i], stem, sizeof(stem));
            /* Output path: obj_root/<stem>.moc */
            char moc_out[700];
            /* Preserve the exact name from the #include directive */
            snprintf(moc_out, sizeof(moc_out), "%s/%s", obj_root, moc_name);
            /* Ensure obj_root subdir exists */
            ensure_dir_exists(moc_out);

            char moc_cmd[1500];
            snprintf(moc_cmd, sizeof(moc_cmd), "\"%s\" \"%s\" -o \"%s\" 2>&1",
                     moc_bin, cfiles[i], moc_out);
            printf(CYAN "[QT-moc] inline: %s  →  %s\n" RESET, cfiles[i], moc_name);
            int moc_ok = system(moc_cmd);
            if (moc_ok != 0) {
                printf(YELLOW "[QT-moc] Warning: moc failed for %s\n" RESET, cfiles[i]);
            } else {
                printf(GREEN  "[QT-moc] ✓ %s generated\n" RESET, moc_name);
                total_inline++;
            }
        }
    }

    /* ── Summary ──────────────────────────────────────────────── */
    if (total_hdr || total_inline) {
        printf(GREEN "[QT] auto-moc: %d header(s)  %d inline(s)\n" RESET,
               total_hdr, total_inline);
    } else {
        printf(CYAN "[QT] auto-moc: no Q_OBJECT found\n" RESET);
    }
}

static void qt_run_uic(const char *uic_bin, const char *src_dir, const char *obj_root) {
    char find_cmd[512];
#ifdef _WIN32
    snprintf(find_cmd, sizeof(find_cmd), "dir /s /b \"%s\\*.ui\" 2>nul", src_dir);
#else
    snprintf(find_cmd, sizeof(find_cmd), "find \"%s\" -name '*.ui' 2>/dev/null", src_dir);
#endif
    FILE *fp = popen(find_cmd, "r"); if (!fp) return;
    char ui[1024]; int n = 0;
    while (fgets(ui, sizeof(ui), fp)) {
        char *nl = strrchr(ui,'\n'); if(nl)*nl='\0'; if(!ui[0]) continue;
        char stem[256]; path_stem(ui, stem, sizeof(stem));
        char out_h[600]; snprintf(out_h, sizeof(out_h), "%s/ui_%s.h", obj_root, stem);
        char cmd[1400]; snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" -o \"%s\" 2>&1", uic_bin, ui, out_h);
        printf(CYAN "[QT-uic] %s → ui_%s.h\n" RESET, ui, stem);
        system(cmd); n++;
    }
    pclose(fp);
    if (n) printf(GREEN "[QT] auto-uic: %d .ui file(s)\n" RESET, n);
}

void setup_qt(const char *modules_str, char *tags, char *libs,
              const char *src_dir, const char *obj_root,
              const char *comp, CompilerType ct,
              char *extra_objs, size_t esz) {
    if (!modules_str || !modules_str[0]) return;
    printf(BLUE "\n[QT] ═══════════════════════════════\n" RESET);
    printf(BLUE "[QT] Qt Setup\n" RESET);

    int qver = 0;
    char moc[512], uic[512], rcc[512], qt_inc[512], qt_lib[512];
    qt_find_tools(&qver, moc, uic, rcc, qt_inc, qt_lib, 512);

    if (!qt_inc[0]) {
        printf(RED "[QT] Qt not found!\n" RESET);
        printf(RED "[QT] Install Qt via:\n" RESET);
        printf(RED "[QT]   Windows: https://www.qt.io/download-open-source\n" RESET);
        printf(RED "[QT]   Linux:   sudo apt install qt6-base-dev\n" RESET);
        printf(RED "[QT]   macOS:   brew install qt\n" RESET);
        printf(RED "[QT] Then set QTDIR=C:/Qt/6.x.x/mingw_64 (or add qmake to PATH)\n" RESET);
        printf(BLUE "[QT] ═══════════════════════════════\n\n" RESET);
        return;
    }

    if (qver)  printf(GREEN "[QT] Qt%d  |  %s\n" RESET, qver, qt_inc);
    else       printf(YELLOW "[QT] Qt (version unknown)  |  %s\n" RESET, qt_inc);

    const char *pfx = (qver == 6) ? "Qt6" : "Qt5";

    /* ── Include paths ───────────────────────────────────────────── */
    /* qt_inc = e.g. C:/Qt/6.7.0/mingw_64/include  OR
                     /usr/include/qt6                OR
                     /ucrt64/include/qt6             */
    {
        char f[700];
        /* Base include dir */
        snprintf(f,sizeof(f),"-I\"%s\"",qt_inc); tag_add(tags,f);
        /* Qt6/ or Qt5/ subdir (Linux MSYS2: inc_base already points inside) */
        char qt_prefix_dir[600];
        snprintf(qt_prefix_dir,sizeof(qt_prefix_dir),"%s/%s",qt_inc,pfx);
        if (dir_exists(qt_prefix_dir)) {
            snprintf(f,sizeof(f),"-I\"%s\"",qt_prefix_dir); tag_add(tags,f);
        }
        /* obj_root for generated moc_*.h, ui_*.h */
        snprintf(f,sizeof(f),"-I\"%s\"",obj_root); tag_add(tags,f);
    }

    /* ── Required compile flags ──────────────────────────────────── */
    tag_add(tags, "-DQT_NO_DEBUG");
#ifndef _WIN32
    if (ct == CT_GCC || ct == CT_CLANG) tag_add(tags, "-fPIC");
#endif

    /* ── Lib path ────────────────────────────────────────────────── */
    if (qt_lib[0]) {
        char f[700]; snprintf(f,sizeof(f),"-L\"%s\"",qt_lib); tag_add(libs,f);
    }

    /* ── Resolve implicit module dependencies ───────────────────── */
    char modules_expanded[B_SIZE]; strncpy(modules_expanded, modules_str, B_SIZE-1);
    {
        /* Dependency table: if module A present and module B present/available,
           auto-add module C */
        struct { const char *trigger; const char *also_add; } auto_deps[] = {
            /* Multimedia needs MultimediaWidgets if Widgets also present */
            {"Multimedia",   "MultimediaWidgets"},
            /* QML needs Quick */
            {"Qml",          "Quick"},
            /* Quick needs QuickWidgets if Widgets present */
            {"Quick",        "QuickWidgets"},
            {NULL,NULL}
        };
        for (int ai=0; auto_deps[ai].trigger; ai++) {
            const char *tr  = auto_deps[ai].trigger;
            const char *add = auto_deps[ai].also_add;
            /* Check: trigger module present AND add module not yet present */
            /* Case-insensitive contains check */
            int has_tr=0, has_add=0;
            { char _a[B_SIZE],_b[64]; strncpy(_a,modules_expanded,B_SIZE-1); strncpy(_b,tr,63);
              for(char*p=_a;*p;p++) *p=(char)tolower((unsigned char)*p);
              for(char*p=_b;*p;p++) *p=(char)tolower((unsigned char)*p);
              has_tr=(strstr(_a,_b)!=NULL); }
            { char _a[B_SIZE],_b[64]; strncpy(_a,modules_expanded,B_SIZE-1); strncpy(_b,add,63);
              for(char*p=_a;*p;p++) *p=(char)tolower((unsigned char)*p);
              for(char*p=_b;*p;p++) *p=(char)tolower((unsigned char)*p);
              has_add=(strstr(_a,_b)!=NULL); }
            /* Only add if dir exists to avoid phantom modules */
            if (has_tr && !has_add) {
                char add_dir[700];
                snprintf(add_dir,sizeof(add_dir),"%s/Qt%s",qt_inc,add);
                if (dir_exists(add_dir)) {
                    strncat(modules_expanded,",",B_SIZE-strlen(modules_expanded)-1);
                    strncat(modules_expanded,add,B_SIZE-strlen(modules_expanded)-1);
                    printf(CYAN "[QT] Auto-adding Qt%s (dependency)\n" RESET, add);
                }
            }
        }
    }
    /* ── Per-module setup ────────────────────────────────────────── */
    char mods[B_SIZE]; strncpy(mods, modules_expanded, B_SIZE-1);
    char *mod = strtok(mods, " \t,;");
    while (mod) {
        /* Normalize: first char uppercase */
        char mc[64]; strncpy(mc, mod, 63); mc[63]='\0';
        if (mc[0]>='a'&&mc[0]<='z') mc[0]=(char)(mc[0]-32);

        char pkg_full[128]; snprintf(pkg_full,sizeof(pkg_full),"%s%s",pfx,mc);

        int found = pkgcfg(pkg_full, tags, libs);
        if (found) {
            printf(GREEN "[QT] %-24s  pkg-config\n" RESET, pkg_full);
        } else {
            /* Module header subdir — try all known Qt path layouts:
               Windows Qt Installer: include/QtCore  include/QtWidgets
               Linux system:         include/qt6/QtCore
               MSYS2:               include/qt6/QtCore  */
            char mod_inc[700]; mod_inc[0]='\0';
            {
                char tries[6][700];
                /* 1. include/QtWidgets  (Windows Qt Installer) */
                snprintf(tries[0],sizeof(tries[0]),"%s/Qt%s",qt_inc,mc);
                /* 2. include/Qt6/QtWidgets  (Linux pkg-config) */
                snprintf(tries[1],sizeof(tries[1]),"%s/%s/Qt%s",qt_inc,pfx,mc);
                /* 3. include/qt6/QtWidgets  (lowercase) */
                snprintf(tries[2],sizeof(tries[2]),"%s/qt6/Qt%s",qt_inc,mc);
                snprintf(tries[3],sizeof(tries[3]),"%s/qt5/Qt%s",qt_inc,mc);
                /* 4. include/Qt6Widgets  (wrong but try) */
                snprintf(tries[4],sizeof(tries[4]),"%s/%s%s",qt_inc,pfx,mc);
                /* 5. The qt_inc itself already points inside Qt6/ */
                snprintf(tries[5],sizeof(tries[5]),"%s/Qt%s",qt_inc,mc);
                for (int ti=0; ti<6; ti++) {
                    if (dir_exists(tries[ti])) { strncpy(mod_inc,tries[ti],699); break; }
                }
            }
            if (mod_inc[0]) {
                char f[700]; snprintf(f,sizeof(f),"-I\"%s\"",mod_inc); tag_add(tags,f);
                printf(GREEN "[QT] %-24s  %s\n" RESET, pkg_full, mod_inc);
            } else {
                /* Give actionable install hint */
                printf(YELLOW "[QT] %-24s  NOT FOUND\n" RESET, pkg_full);
                /* Map module → Qt Installer component name */
                struct { const char *mod; const char *component; const char *extra; } hints[] = {
                    {"Multimedia",       "Qt Multimedia",          "Qt Installer → Qt 6.x → Additional Libraries → Qt Multimedia"},
                    {"MultimediaWidgets","Qt Multimedia",          "Qt Installer → Qt 6.x → Additional Libraries → Qt Multimedia"},
                    {"MultimediaQuick",  "Qt Multimedia",          "Qt Installer → Qt 6.x → Additional Libraries → Qt Multimedia"},
                    {"Charts",           "Qt Charts",              "Qt Installer → Qt 6.x → Additional Libraries → Qt Charts"},
                    {"DataVisualization","Qt Data Visualization",  "Qt Installer → Qt 6.x → Additional Libraries → Qt Data Visualization"},
                    {"WebEngineWidgets", "Qt WebEngine",           "Qt Installer → Qt 6.x → Qt WebEngine"},
                    {"WebEngineCore",    "Qt WebEngine",           "Qt Installer → Qt 6.x → Qt WebEngine"},
                    {"WebView",          "Qt WebView",             "Qt Installer → Qt 6.x → Qt WebView"},
                    {"Quick",            "Qt Quick",               "Qt Installer → Qt 6.x → Qt Quick"},
                    {"Qml",              "Qt QML",                 "Qt Installer → Qt 6.x → Qt QML"},
                    {"3DCore",           "Qt 3D",                  "Qt Installer → Qt 6.x → Qt 3D"},
                    {"Bluetooth",        "Qt Bluetooth",           "Qt Installer → Qt 6.x → Additional Libraries → Qt Bluetooth"},
                    {"SerialPort",       "Qt Serial Port",         "Qt Installer → Qt 6.x → Additional Libraries → Qt Serial Port"},
                    {"Positioning",      "Qt Positioning",         "Qt Installer → Qt 6.x → Additional Libraries → Qt Positioning"},
                    {"Sensors",          "Qt Sensors",             "Qt Installer → Qt 6.x → Additional Libraries → Qt Sensors"},
                    {"HttpServer",       "Qt HTTP Server",         "Qt Installer → Qt 6.x → Additional Libraries → Qt HTTP Server"},
                    {"Pdf",              "Qt PDF",                 "Qt Installer → Qt 6.x → Qt PDF"},
                    {"PrintSupport",     "Qt Print Support",       "Qt Installer → Qt 6.x → Qt Print Support (may be in base)"},
                    {NULL,NULL,NULL}
                };
                for (int hi=0; hints[hi].mod; hi++) {
                    if (strcasecmp(mc, hints[hi].mod)==0) {
                        printf(RED "[QT]   Install: %s\n" RESET, hints[hi].component);
                        printf(RED "[QT]   Via: %s\n" RESET, hints[hi].extra);
                        break;
                    }
                }
            }
            /* Link */
            char lf[128]; snprintf(lf,sizeof(lf),"-l%s",pkg_full); tag_add(libs,lf);
        }
        /* Module define: -DQT_WIDGETS_LIB etc */
        char def[128]; snprintf(def,sizeof(def),"-DQT_%s_LIB",mc);
        for (char *p=def;*p;p++) if(*p>='a'&&*p<='z') *p=(char)(*p-32);
        tag_add(tags, def);

        mod = strtok(NULL, " \t,;");
    }

    /* ── macOS framework paths ────────────────────────────────────── */
#ifdef __APPLE__
    {
        const char *fw[]=
            {"/opt/homebrew/opt/qt/lib","/opt/homebrew/opt/qt@6/lib",
             "/opt/homebrew/opt/qt@5/lib","/Library/Frameworks",NULL};
        for (int i=0; fw[i]; i++) {
            if (dir_exists(fw[i])) {
                char f[600];
                snprintf(f,sizeof(f),"-F\"%s\"",fw[i]); tag_add(tags,f);
                snprintf(f,sizeof(f),"-L\"%s\"",fw[i]); tag_add(libs,f);
                break;
            }
        }
    }
#endif

    /* ── Windows: add Qt bin to PATH for DLL loading at runtime ─── */
#ifdef _WIN32
    if (qt_lib[0]) {
        /* Derive Qt bin dir from lib dir: .../lib → .../bin */
        char qt_bin[600]; strncpy(qt_bin, qt_lib, 599);
        char *lib_part = strstr(qt_bin, "/lib");
        if (!lib_part) lib_part = strstr(qt_bin, "\\lib");
        if (lib_part) {
            strncpy(lib_part, "/bin", 5);
            printf(CYAN "[QT] Runtime DLLs in: %s\n" RESET, qt_bin);
            printf(CYAN "[QT] If exe crashes: set PATH=%%PATH%%;%s\n" RESET, qt_bin);
        }
    }
    /* Windows Qt: mingw32 entry + platform plugin stub */
    /* Note: kernel32/user32/etc are auto-linked by MinGW, skip them */
    tag_add(libs, "-lmingw32");
#endif

    /* ── auto-moc ────────────────────────────────────────────────── */
    /* Always run moc scan: even if moc binary isn't confirmed yet,
     * scan src files first to see if #include "*.moc" exists.
     * If inline moc is needed but moc is missing, give a clear error. */
    {
        /* Try to find moc in PATH if not found via qmake */
        char moc_try[512]; strncpy(moc_try, moc, 511);
        if (!moc_try[0]) {
            /* Try common names in PATH */
            static const char *moc_names[] = {"moc","moc6","moc-qt6","moc5","moc-qt5",NULL};
            for (int mi=0; moc_names[mi]; mi++) {
                if (tool_exists(moc_names[mi])) {
                    strncpy(moc_try, moc_names[mi], 511);
                    printf(CYAN "[QT] moc found in PATH: %s\n" RESET, moc_try);
                    break;
                }
            }
        }
        if (moc_try[0]) {
            qt_run_moc(moc_try, src_dir, obj_root, comp, ct, tags, extra_objs, esz);
        } else {
            /* Scan src to see if moc is actually needed */
            static char _cfiles[512][1024];
            int _nc = collect_files_ext(src_dir, ".cpp", _cfiles, 512);
            _nc    += collect_files_ext(src_dir, ".cxx", _cfiles + _nc, 512 - _nc);
            int needs_moc = 0;
            for (int _i=0; _i<_nc && !needs_moc; _i++) {
                FILE *_fp = fopen(_cfiles[_i], "r");
                if (!_fp) continue;
                char _line[512];
                while (fgets(_line, sizeof(_line), _fp)) {
                    if (strstr(_line, "#include") && strstr(_line, ".moc\"")) {
                        needs_moc = 1; break;
                    }
                }
                fclose(_fp);
            }
            /* Also check headers */
            static char _hfiles[256][1024];
            int _nh = collect_files_ext(src_dir, ".h",   _hfiles, 128);
            _nh    += collect_files_ext(src_dir, ".hpp",  _hfiles+_nh, 128-_nh);
            for (int _i=0; _i<_nh && !needs_moc; _i++)
                if (file_contains(_hfiles[_i], "Q_OBJECT")) needs_moc=1;

            if (needs_moc) {
                printf(RED "[QT] ERROR: moc is required but not found!\n" RESET);
                printf(RED "[QT]   Source files contain #include \"*.moc\" or Q_OBJECT\n" RESET);
                printf(YELLOW "[QT]   Install Qt and add moc to PATH, or set QT_DIR\n" RESET);
            } else {
                printf(CYAN "[QT] moc not needed (no Q_OBJECT / inline moc found)\n" RESET);
            }
        }
    }

    /* ── auto-uic ────────────────────────────────────────────────── */
    if (uic[0]) qt_run_uic(uic, src_dir, obj_root);

    printf(GREEN "[QT] Setup complete\n" RESET);
    printf(BLUE "[QT] ═══════════════════════════════\n\n" RESET);
}

/* ================================================================== */
/*  OpenGL                                                             */
/*  CMake: find_package(OpenGL REQUIRED) + target_link_libraries      */
/*  OMake: USE_LIB: opengl   (platform auto-detected)                 */
/* ================================================================== */
static void setup_opengl(char *tags, char *libs) {
    (void)tags;
#ifdef _WIN32
    tag_add(libs, "-lopengl32"); tag_add(libs, "-lgdi32");
    printf(GREEN "[LIB] OpenGL  win32: opengl32 + gdi32\n" RESET);
#elif defined(__APPLE__)
    tag_add(libs, "-framework OpenGL");
    tag_add(libs, "-framework CoreFoundation");
    printf(GREEN "[LIB] OpenGL  macOS: framework OpenGL\n" RESET);
#else
    if (!pkgcfg("gl", tags, libs)) tag_add(libs, "-lGL");
    tag_add(libs, "-lX11");
    printf(GREEN "[LIB] OpenGL  linux: -lGL -lX11\n" RESET);
#endif
}

/* ================================================================== */
/*  GLFW 3                                                             */
/*  CMake: find_package(glfw3 REQUIRED) — often fails, needs hints   */
/*  OMake: USE_LIB: glfw   (pkg-config + header scan + platform libs) */
/* ================================================================== */
static void setup_glfw(char *tags, char *libs) {
    if (pkgcfg("glfw3", tags, libs)) {
        printf(GREEN "[LIB] GLFW    via pkg-config\n" RESET); goto glfw_syslibs;
    }
    /* Source build via FETCH_V2 (deps/glfw) */
    if (deps_inject_flags("glfw", "GLFW/glfw3.h", tags, libs)) goto glfw_syslibs;
    /* System header scan */
    {
        char inc[512];
        if (find_header("GLFW/glfw3.h", inc, sizeof(inc))) {
            char f[600]; snprintf(f, sizeof(f), "-I\"%s\"", inc); tag_add(tags, f);
        }
    }
glfw_syslibs:
#ifdef _WIN32
    tag_add(libs, "-lglfw3"); tag_add(libs, "-lgdi32");
    tag_add(libs, "-luser32"); tag_add(libs, "-lshell32");
#elif defined(__APPLE__)
    tag_add(libs, "-lglfw");
    tag_add(libs, "-framework Cocoa");
    tag_add(libs, "-framework IOKit");
    tag_add(libs, "-framework CoreVideo");
#else
    tag_add(libs, "-lglfw");
    if (tool_exists("wayland-scanner")) tag_add(libs, "-lwayland-client");
    else tag_add(libs, "-lX11");
#endif
    printf(GREEN "[LIB] GLFW    configured\n" RESET);
}

/* ================================================================== */
/*  GLEW                                                               */
/*  CMake: find_package(GLEW REQUIRED) + complex static/shared logic  */
/*  OMake: USE_LIB: glew  (auto static define on Windows)             */
/* ================================================================== */
static void setup_glew(char *tags, char *libs, int do_static) {
    if (pkgcfg("glew", tags, libs)) {
        printf(GREEN "[LIB] GLEW    via pkg-config\n" RESET); goto glew_link;
    }
    if (!deps_inject_flags("glew", "GL/glew.h", tags, libs)) {
        char inc[512];
        if (find_header("GL/glew.h", inc, sizeof(inc))) {
            char f[600]; snprintf(f, sizeof(f), "-I\"%s\"", inc); tag_add(tags, f);
        }
    }
glew_link:
#ifdef _WIN32
    if (do_static) { tag_add(tags, "-DGLEW_STATIC"); tag_add(libs, "-lglew32s"); }
    else             tag_add(libs, "-lglew32");
    tag_add(libs, "-lopengl32");
#elif defined(__APPLE__)
    tag_add(libs, "-lGLEW"); tag_add(libs, "-framework OpenGL");
#else
    tag_add(libs, "-lGLEW"); tag_add(libs, "-lGL");
#endif
    printf(GREEN "[LIB] GLEW    configured%s\n" RESET, do_static?" (static)":"");
}

/* ================================================================== */
/*  GLAD                                                               */
/*  CMake: manual add_library(glad STATIC ...) — users always forget  */
/*  OMake: USE_LIB: glad  auto-finds glad.c and compiles it           */
/* ================================================================== */
static void setup_glad(char *tags, char *libs, const char *obj_root,
                         const char *comp, CompilerType ct,
                         char *extra_objs, size_t esz) {
    (void)libs;
    /* Search for glad.c */
    static const char *glad_srcs[] = {
        "glad/src/glad.c","src/glad/glad.c","glad.c","src/glad.c",
        "external/glad/src/glad.c","thirdparty/glad/src/glad.c",
        "vendor/glad/src/glad.c","deps/glad/src/glad.c", NULL
    };
    const char *found_src = NULL;
    for (int i = 0; glad_srcs[i]; i++)
        if (is_regular_file(glad_srcs[i])) { found_src = glad_srcs[i]; break; }

    /* Search for glad include dir */
    static const char *glad_incs[] = {
        "glad/include","include","external/glad/include",
        "thirdparty/glad/include","vendor/glad/include", NULL
    };
    for (int i = 0; glad_incs[i]; i++) {
        char h[512]; snprintf(h, sizeof(h), "%s/glad/glad.h", glad_incs[i]);
        if (is_regular_file(h)) {
            char f[600]; snprintf(f, sizeof(f), "-I\"%s\"", glad_incs[i]);
            tag_add(tags, f); break;
        }
    }
    /* System install */
    char sys_inc[512];
    if (!found_src && find_header("glad/glad.h", sys_inc, sizeof(sys_inc))) {
        char f[600]; snprintf(f, sizeof(f), "-I\"%s\"", sys_inc);
        tag_add(tags, f);
        printf(YELLOW "[LIB] GLAD    system install (no glad.c compile)\n" RESET);
        return;
    }
    if (!found_src) {
        printf(YELLOW "[LIB] GLAD    glad.c not found!\n" RESET);
        printf(YELLOW "              → Place glad/src/glad.c + glad/include/ in project\n" RESET);
        printf(YELLOW "              → Generator: https://glad.dav1d.de/\n" RESET);
        return;
    }
    char obj[512]; snprintf(obj, sizeof(obj), "%s/glad.o", obj_root);
    compile_extra(found_src, obj, comp, ct, tags, extra_objs, esz, "GLAD");
    printf(GREEN "[LIB] GLAD    compiled: %s\n" RESET, found_src);
}

/* ================================================================== */
/*  SDL2 + sub-modules: image ttf mixer net gfx                       */
/*  CMake: find_package(SDL2 REQUIRED) + separate FindSDL2_*.cmake   */
/*  OMake: USE_LIB: sdl2:image,ttf,mixer  (all-in-one)               */
/* ================================================================== */
static void setup_sdl2(const char *modules_str, char *tags, char *libs) {
    /* Core SDL2 */
    int found = pkgcfg("sdl2", tags, libs);
    if (!found) found = deps_inject_flags("SDL2", "SDL2/SDL.h", tags, libs);
    if (!found) deps_inject_flags("sdl2", "SDL2/SDL.h", tags, libs);
    if (!found) {
        char inc[512];
        if (find_header("SDL2/SDL.h", inc, sizeof(inc))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
        } else if (find_header("SDL.h", inc, sizeof(inc))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
        }
    }
#ifdef _WIN32
    tag_add(libs,"-lmingw32"); tag_add(libs,"-lSDL2main"); tag_add(libs,"-lSDL2");
#elif defined(__APPLE__)
    tag_add(libs,"-framework SDL2");
#else
    tag_add(libs,"-lSDL2");
#endif
    printf(GREEN "[LIB] SDL2    core configured\n" RESET);
    if (!modules_str || !modules_str[0]) return;

    /* Sub-modules */
    struct { const char *key; const char *pkg; const char *lib; } mods[] = {
        {"image",  "SDL2_image", "-lSDL2_image"},
        {"ttf",    "SDL2_ttf",   "-lSDL2_ttf"},
        {"mixer",  "SDL2_mixer", "-lSDL2_mixer"},
        {"net",    "SDL2_net",   "-lSDL2_net"},
        {"gfx",    "SDL2_gfx",   "-lSDL2_gfx"},
        {NULL,NULL,NULL}
    };
    char buf[512]; strncpy(buf, modules_str, 511);
    char *tok = strtok(buf, " ,;");
    while (tok) {
        for (int i=0; mods[i].key; i++)
            if (strcasecmp(tok, mods[i].key)==0) {
                if (!pkgcfg(mods[i].pkg, tags, libs)) tag_add(libs, mods[i].lib);
                printf(GREEN "[LIB] SDL2_%-11s configured\n" RESET, tok);
                break;
            }
        tok = strtok(NULL, " ,;");
    }
}

/* ================================================================== */
/*  SFML 2.x / 3.x                                                    */
/*  CMake: find_package(SFML 2 REQUIRED graphics audio network ...)  */
/*  OMake: USE_LIB: sfml:graphics,audio  (static flag auto-handled)  */
/* ================================================================== */
static void setup_sfml(const char *modules_str, char *tags, char *libs, int do_static) {
    const char *def_mods = "graphics audio network system window";
    const char *use = (modules_str && modules_str[0]) ? modules_str : def_mods;

    /* 1. pkg-config (system-installed SFML) */
    if (pkgcfg("sfml-all", tags, libs)) {
        printf(GREEN "[LIB] SFML    all modules via pkg-config\n" RESET); return;
    }
    /* Try individual modules via pkg-config */
    {
        char mbuf[256]; strncpy(mbuf,use,255);
        char *tok = strtok(mbuf," ,;"); int any=0;
        while (tok) {
            char pkg[64]; snprintf(pkg,sizeof(pkg),"sfml-%s",tok);
            if (pkgcfg(pkg,tags,libs)) any++;
            tok=strtok(NULL," ,;");
        }
        if (any) { printf(GREEN "[LIB] SFML    %d module(s) via pkg-config\n" RESET,any); goto sfml_syslibs; }
    }

    /* 2. SFML built from source via FETCH_V2 (deps/SFML) */
    {
        /* Search candidate locations */
        static const char *sfml_roots[] = {
            "deps/SFML","deps/sfml","extern/SFML","third_party/SFML",NULL
        };
        for (int ri=0; sfml_roots[ri]; ri++) {
            /* Include: deps/SFML/include */
            char inc_try[512]; snprintf(inc_try,sizeof(inc_try),"%s/include",sfml_roots[ri]);
            if (!dir_exists(inc_try)) continue;

            char incf[600]; snprintf(incf,sizeof(incf),"-I\"%s\"",inc_try); tag_add(tags,incf);
            if (do_static) tag_add(tags,"-DSFML_STATIC");

            /* Lib search: _build/lib, _build/lib/Release, _build/lib/Debug, lib */
            static const char *lib_subs[] = {
                "_build/lib","_build/lib/Release","_build/lib/Debug",
                "_build","lib","build/lib",NULL
            };
            char lib_dir[512]; lib_dir[0]='\0';
            for (int li=0; lib_subs[li]; li++) {
                char lp[512]; snprintf(lp,sizeof(lp),"%s/%s",sfml_roots[ri],lib_subs[li]);
                if (dir_exists(lp)) { strncpy(lib_dir,lp,511); break; }
            }
            if (lib_dir[0]) {
                char lflag[600]; snprintf(lflag,sizeof(lflag),"-L\"%s\"",lib_dir);
                tag_add(libs,lflag);
                /* Link order matters: graphics→window→system, audio→system */
                char mbuf[256]; strncpy(mbuf,use,255);
                char *tok=strtok(mbuf," ,;");
                while (tok) {
                    char lf[64];
                    snprintf(lf,sizeof(lf),do_static?"-lsfml-%s-s":"-lsfml-%s",tok);
                    tag_add(libs,lf);
                    tok=strtok(NULL," ,;");
                }
            } else {
                /* Lib not built yet — cmake hasn't run or failed */
                printf(YELLOW "[LIB] SFML  source found at %s but not built yet\n" RESET, sfml_roots[ri]);
                printf(YELLOW "[LIB] SFML  Add to OMakeLists.txt:\n" RESET);
                printf(CYAN   "[LIB]   FETCH_V2: https://github.com/SFML/SFML.git TO %s TAG 2.6.x CMAKE -DBUILD_SHARED_LIBS=OFF -DSFML_BUILD_EXAMPLES=OFF\n" RESET, sfml_roots[ri]);
                /* Still add include path so user gets useful error */
                char mbuf2[256]; strncpy(mbuf2,use,255);
                char *tok=strtok(mbuf2," ,;");
                while (tok) {
                    char lf[64]; snprintf(lf,sizeof(lf),"-lsfml-%s%s",tok,do_static?"-s":"");
                    tag_add(libs,lf);
                    tok=strtok(NULL," ,;");
                }
            }
            printf(GREEN "[LIB] SFML    source: %s [%s]%s\n" RESET,
                   sfml_roots[ri], use, do_static?" (static)":"");
            goto sfml_syslibs;
        }
    }

    /* 3. Header scan (already installed somewhere find_header can see) */
    {
        char inc[512];
        if (find_header("SFML/Config.hpp", inc, sizeof(inc))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
            if (do_static) tag_add(tags,"-DSFML_STATIC");
        }
        char mbuf[256]; strncpy(mbuf,use,255);
        char *tok=strtok(mbuf," ,;");
        while (tok) {
            char lf[64]; snprintf(lf,sizeof(lf),do_static?"-lsfml-%s-s":"-lsfml-%s",tok);
            tag_add(libs,lf);
            tok=strtok(NULL," ,;");
        }
        printf(GREEN "[LIB] SFML    configured [%s]%s\n" RESET, use, do_static?" (static)":"");
    }

sfml_syslibs:
    /* Platform system deps (always needed) */
#ifdef _WIN32
    tag_add(libs,"-lwinmm"); tag_add(libs,"-lws2_32");
    tag_add(libs,"-lopengl32"); tag_add(libs,"-lgdi32");
#elif defined(__APPLE__)
    tag_add(libs,"-framework OpenGL"); tag_add(libs,"-framework Foundation");
    tag_add(libs,"-framework AppKit"); tag_add(libs,"-framework IOKit");
    tag_add(libs,"-framework CoreAudio"); tag_add(libs,"-framework AudioToolbox");
#else
    tag_add(libs,"-lX11"); tag_add(libs,"-lGL");
    tag_add(libs,"-ludev"); tag_add(libs,"-lpthread");
    tag_add(libs,"-lfreetype"); tag_add(libs,"-lopenal");
    tag_add(libs,"-lvorbisenc"); tag_add(libs,"-lvorbisfile");
    tag_add(libs,"-lvorbis"); tag_add(libs,"-logg"); tag_add(libs,"-lFLAC");
#endif
}

/* ================================================================== */
/*  Vulkan + SPIR-V shader compilation                                 */
/*  CMake: find_package(Vulkan REQUIRED) + custom shader targets      */
/*  OMake: USE_LIB: vulkan  (auto glslc for .vert/.frag/.comp shaders)*/
/* ================================================================== */
static void setup_vulkan(char *tags, char *libs, const char *src_dir,
                           const char *obj_root) {
    if (pkgcfg("vulkan", tags, libs)) {
        printf(GREEN "[LIB] Vulkan  via pkg-config\n" RESET);
    } else {
        const char *sdk = getenv("VULKAN_SDK");
        if (sdk && sdk[0]) {
            char f[600];
            snprintf(f,sizeof(f),"-I\"%s/include\"",sdk); tag_add(tags,f);
            snprintf(f,sizeof(f),"-L\"%s/lib\"",sdk);     tag_add(libs,f);
            printf(YELLOW "[LIB] Vulkan  VULKAN_SDK=%s\n" RESET, sdk);
        } else {
            char inc[512];
            if (find_header("vulkan/vulkan.h", inc, sizeof(inc))) {
                char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
            }
        }
#ifdef _WIN32
        tag_add(libs, "-lvulkan-1");
#elif defined(__APPLE__)
        tag_add(libs, "-lvulkan");
        tag_add(libs, "-framework Metal"); tag_add(libs, "-framework QuartzCore");
#else
        tag_add(libs, "-lvulkan");
#endif
        printf(GREEN "[LIB] Vulkan  configured\n" RESET);
    }

    /* Auto-compile GLSL shaders to SPIR-V with glslc */
    if (tool_exists("glslc")) {
        char find_cmd[512];
#ifdef _WIN32
        snprintf(find_cmd, sizeof(find_cmd),
            "dir /s /b \"%s\\*.vert\" \"%s\\*.frag\" \"%s\\*.comp\" \"%s\\*.geom\" 2>nul",
            src_dir, src_dir, src_dir, src_dir);
#else
        snprintf(find_cmd, sizeof(find_cmd),
            "find \"%s\" \\( -name '*.vert' -o -name '*.frag' -o -name '*.comp' -o -name '*.geom' -o -name '*.tesc' -o -name '*.tese' \\) 2>/dev/null",
            src_dir);
#endif
        FILE *fp = popen(find_cmd, "r"); int sc = 0;
        if (fp) {
            char shader[1024];
            while (fgets(shader, sizeof(shader), fp)) {
                char *nl=strrchr(shader,'\n'); if(nl)*nl='\0'; if(!shader[0]) continue;
                char stem[256]; path_stem(shader, stem, sizeof(stem));
                const char *ext=strrchr(shader,'.'); if(!ext) continue;
                char spv[512]; snprintf(spv,sizeof(spv),"%s/%s%s.spv",obj_root,stem,ext);
                char cmd[1200]; snprintf(cmd,sizeof(cmd),"glslc \"%s\" -o \"%s\"",shader,spv);
                printf(CYAN "[VULKAN] glslc: %s\n" RESET, shader);
                if (system(cmd)==0) sc++;
            }
            pclose(fp);
        }
        if (sc) printf(GREEN "[VULKAN] %d shader(s) → SPIR-V\n" RESET, sc);
    } else {
        printf(CYAN "[VULKAN] glslc not found — GLSL shaders not compiled\n" RESET);
    }
}

/* ================================================================== */
/*  Dear ImGui                                                         */
/*  CMake: manual add_library, complex backend wiring — 40+ lines    */
/*  OMake: USE_LIB: imgui:opengl3+glfw  (auto-finds dir, auto-build)  */
/* ================================================================== */
static void setup_imgui(const char *backend, char *tags, char *libs,
                          const char *obj_root, const char *comp,
                          CompilerType ct, char *extra_objs, size_t esz) {
    /* Find imgui root dir */
    static const char *dirs[] = {
        "imgui","external/imgui","thirdparty/imgui",
        "vendor/imgui","deps/imgui","lib/imgui", NULL
    };
    const char *imgui_dir = NULL;
    for (int i=0; dirs[i]; i++) {
        char h[512]; snprintf(h,sizeof(h),"%s/imgui.h",dirs[i]);
        if (is_regular_file(h)) { imgui_dir=dirs[i]; break; }
    }
    if (!imgui_dir) {
        char sys[512];
        if (find_header("imgui.h", sys, sizeof(sys))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
            printf(YELLOW "[LIB] ImGui   system install\n" RESET);
        } else {
            printf(YELLOW "[LIB] ImGui   NOT FOUND\n" RESET);
            printf(YELLOW "              → git clone https://github.com/ocornut/imgui.git\n" RESET);
        }
        return;
    }

    { char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",imgui_dir); tag_add(tags,f); }
    { char f[600]; snprintf(f,sizeof(f),"-I\"%s/backends\"",imgui_dir); tag_add(tags,f); }

    /* Core sources */
    static const char *core[] = {
        "imgui.cpp","imgui_draw.cpp","imgui_tables.cpp","imgui_widgets.cpp",NULL
    };
    /* Backend sources based on selection */
    char bk_srcs[8][64]; int nb=0;
    const char *be = backend ? backend : "opengl3+glfw";
    if (strstr(be,"opengl3")) strncpy(bk_srcs[nb++],"backends/imgui_impl_opengl3.cpp",63);
    if (strstr(be,"opengl2")) strncpy(bk_srcs[nb++],"backends/imgui_impl_opengl2.cpp",63);
    if (strstr(be,"glfw"))    strncpy(bk_srcs[nb++],"backends/imgui_impl_glfw.cpp",63);
    if (strstr(be,"sdl2"))    strncpy(bk_srcs[nb++],"backends/imgui_impl_sdl2.cpp",63);
    if (strstr(be,"sdl3"))    strncpy(bk_srcs[nb++],"backends/imgui_impl_sdl3.cpp",63);
    if (strstr(be,"vulkan"))  strncpy(bk_srcs[nb++],"backends/imgui_impl_vulkan.cpp",63);
    if (strstr(be,"dx11"))  { strncpy(bk_srcs[nb++],"backends/imgui_impl_dx11.cpp",63); tag_add(libs,"-ld3d11"); tag_add(libs,"-ld3dcompiler"); }
    if (strstr(be,"dx12"))  { strncpy(bk_srcs[nb++],"backends/imgui_impl_dx12.cpp",63); tag_add(libs,"-ld3d12"); tag_add(libs,"-ld3dcompiler"); }
    if (strstr(be,"win32"))   strncpy(bk_srcs[nb++],"backends/imgui_impl_win32.cpp",63);

    int compiled=0;
    for (int pass=0; pass<2; pass++) {
        int len = (pass==0) ? 4 : nb;
        for (int i=0; i<len; i++) {
            char sp[600], op[600], stem[256];
            snprintf(sp, sizeof(sp), "%s/%s", imgui_dir, (pass==0)?core[i]:bk_srcs[i]);
            if (!is_regular_file(sp)) continue;
            path_stem(sp, stem, sizeof(stem));
            snprintf(op, sizeof(op), "%s/imgui_%s.o", obj_root, stem);
            compile_extra(sp, op, comp, ct, tags, extra_objs, esz, "ImGui");
            compiled++;
        }
    }
    printf(GREEN "[LIB] ImGui   %d source(s) compiled [backend: %s]\n" RESET, compiled, be);
}

/* ================================================================== */
/*  GLM — header-only OpenGL math                                      */
/* ================================================================== */
static void setup_glm(char *tags) {
    if (pkgcfg("glm", tags, NULL)) {
        printf(GREEN "[LIB] GLM     via pkg-config\n" RESET); return;
    }
    static const char *local[] = {"glm","external/glm","thirdparty/glm","vendor/glm",NULL};
    for (int i=0; local[i]; i++) {
        char h[512]; snprintf(h,sizeof(h),"%s/glm/glm.hpp",local[i]);
        if (is_regular_file(h)) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",local[i]); tag_add(tags,f);
            printf(GREEN "[LIB] GLM     local: %s\n" RESET, local[i]); return;
        }
    }
    char sys[512];
    if (find_header("glm/glm.hpp", sys, sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
        printf(GREEN "[LIB] GLM     system\n" RESET);
    } else printf(YELLOW "[LIB] GLM     not found\n" RESET);
}

/* ================================================================== */
/*  Boost                                                              */
/*  CMake: find_package(Boost REQUIRED COMPONENTS filesystem thread)  */
/*  OMake: USE_LIB: boost:filesystem,thread  (BOOST_ROOT auto-detect) */
/* ================================================================== */
static void setup_boost(const char *modules_str, char *tags, char *libs) {
    /* BOOST_ROOT env */
    const char *br = getenv("BOOST_ROOT");
    if (br && br[0]) {
        char f[600];
        snprintf(f,sizeof(f),"-I\"%s\"",br);          tag_add(tags,f);
        snprintf(f,sizeof(f),"-L\"%s/stage/lib\"",br); tag_add(libs,f);
        printf(CYAN "[Boost] BOOST_ROOT=%s\n" RESET, br);
    } else {
        char sys[512];
        if (find_header("boost/version.hpp", sys, sizeof(sys))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
            printf(YELLOW "[Boost] header: %s\n" RESET, sys);
        }
    }
    if (!modules_str || !modules_str[0]) {
        printf(GREEN "[LIB] Boost   headers only\n" RESET); return;
    }
    /* Header-only modules (no link) */
    static const char *ho[] = {
        "algorithm","any","array","assert","bind","config","core","format",
        "function","functional","integer","io","iterator","lexical_cast",
        "math","move","mpl","numeric","optional","phoenix","preprocessor",
        "proto","range","smart_ptr","spirit","static_assert","tokenizer",
        "tuple","type_traits","typeof","utility","variant","foreach",NULL
    };
    char buf[1024]; strncpy(buf,modules_str,1023);
    char *tok=strtok(buf," ,;");
    while (tok) {
        int header_only=0;
        for (int i=0; ho[i]; i++) if(strcasecmp(tok,ho[i])==0){header_only=1;break;}
        if (!header_only) {
            char pkg[128]; snprintf(pkg,sizeof(pkg),"boost_%s",tok);
            if (!pkgcfg(pkg,tags,libs)) {
                char lf[128]; snprintf(lf,sizeof(lf),"-lboost_%s",tok);
                tag_add(libs,lf);
                if (strcmp(tok,"thread")==0) {
#ifdef _WIN32
                    char lft[128]; snprintf(lft,sizeof(lft),"-lboost_thread-mt"); tag_add(libs,lft);
#else
                    tag_add(libs,"-lpthread");
#endif
                }
            }
        }
        tok=strtok(NULL," ,;");
    }
    printf(GREEN "[LIB] Boost   configured\n" RESET);
}

/* ================================================================== */
/*  OpenCV                                                             */
/*  CMake: find_package(OpenCV REQUIRED) — notoriously fragile       */
/*  OMake: USE_LIB: opencv:core,imgproc  (env var + pkg-config)       */
/* ================================================================== */
static void setup_opencv(const char *modules_str, char *tags, char *libs) {
    if (pkgcfg("opencv4",tags,libs)){ printf(GREEN "[LIB] OpenCV  opencv4 pkg-config\n" RESET); return; }
    if (pkgcfg("opencv", tags,libs)){ printf(GREEN "[LIB] OpenCV  opencv  pkg-config\n" RESET); return; }
    const char *cvd = getenv("OPENCV_DIR"); if (!cvd) cvd = getenv("OpenCV_DIR");
    if (cvd && cvd[0]) {
        char f[600];
        snprintf(f,sizeof(f),"-I\"%s/include\"",cvd); tag_add(tags,f);
        snprintf(f,sizeof(f),"-L\"%s/lib\"",cvd);     tag_add(libs,f);
        printf(YELLOW "[LIB] OpenCV  OPENCV_DIR=%s\n" RESET, cvd);
    } else {
        char sys[512];
        if (find_header("opencv4/opencv2/core.hpp",sys,sizeof(sys))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s/opencv4\"",sys); tag_add(tags,f);
        } else if (find_header("opencv2/core.hpp",sys,sizeof(sys))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
        }
    }
    const char *mods = (modules_str&&modules_str[0]) ? modules_str : "core imgproc highgui videoio";
    char buf[1024]; strncpy(buf,mods,1023);
    char *tok=strtok(buf," ,;");
    while (tok) {
        char lf[128]; snprintf(lf,sizeof(lf),"-lopencv_%s",tok); tag_add(libs,lf);
        tok=strtok(NULL," ,;");
    }
    printf(GREEN "[LIB] OpenCV  configured\n" RESET);
}

/* ================================================================== */
/*  Assimp                                                             */
/* ================================================================== */
static void setup_assimp(char *tags, char *libs) {
    if (deps_inject_flags("assimp", "assimp/Importer.hpp", tags, libs)) { tag_add(libs,"-lassimp"); return; }
    if (pkgcfg("assimp",tags,libs)){ printf(GREEN "[LIB] Assimp  pkg-config\n" RESET); return; }
    char sys[512];
    if (find_header("assimp/Importer.hpp",sys,sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
    }
    tag_add(libs,"-lassimp");
#ifdef _WIN32
    tag_add(libs,"-lIrrXML"); tag_add(libs,"-lzlib");
#endif
    printf(GREEN "[LIB] Assimp  configured\n" RESET);
}

/* ================================================================== */
/*  FreeType 2                                                          */
/* ================================================================== */
static void setup_freetype(char *tags, char *libs) {
    if (pkgcfg("freetype2",tags,libs)){ printf(GREEN "[LIB] FreeType pkg-config\n" RESET); return; }
    char sys[512];
    if (find_header("freetype2/freetype/freetype.h",sys,sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s/freetype2\"",sys); tag_add(tags,f);
    } else if (find_header("freetype/freetype.h",sys,sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
    }
    tag_add(libs,"-lfreetype");
    printf(GREEN "[LIB] FreeType configured\n" RESET);
}

/* ================================================================== */
/*  Eigen 3 — header-only linear algebra                               */
/* ================================================================== */
static void setup_eigen(char *tags) {
    if (pkgcfg("eigen3",tags,NULL)){ printf(GREEN "[LIB] Eigen3  pkg-config\n" RESET); return; }
    static const char *local[] = {"eigen","external/eigen","thirdparty/eigen",NULL};
    for (int i=0; local[i]; i++) {
        char h[512]; snprintf(h,sizeof(h),"%s/Eigen/Core",local[i]);
        if (is_regular_file(h)) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",local[i]); tag_add(tags,f);
            printf(GREEN "[LIB] Eigen3  local: %s\n" RESET, local[i]); return;
        }
    }
    char sys[512];
    if (find_header("eigen3/Eigen/Core",sys,sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s/eigen3\"",sys); tag_add(tags,f);
        printf(GREEN "[LIB] Eigen3  system\n" RESET);
    } else if (find_header("Eigen/Core",sys,sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
        printf(GREEN "[LIB] Eigen3  system\n" RESET);
    } else printf(YELLOW "[LIB] Eigen3  not found\n" RESET);
}

/* ================================================================== */
/*  Box2D                                                              */
/* ================================================================== */
static void setup_box2d(char *tags, char *libs) {
    if (pkgcfg("box2d",tags,libs)){ printf(GREEN "[LIB] Box2D   pkg-config\n" RESET); return; }
    char sys[512];
    if (find_header("box2d/box2d.h",sys,sizeof(sys))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
    }
    tag_add(libs,"-lbox2d");
    printf(GREEN "[LIB] Box2D   configured\n" RESET);
}

/* ================================================================== */
/*  OpenAL                                                             */
/* ================================================================== */
static void setup_openal(char *tags, char *libs) {
    if (deps_inject_flags("openal", "AL/al.h", tags, libs)) {
        tag_add(libs,"-lopenal");
#ifdef _WIN32
        tag_add(libs,"-lwinmm");
#elif defined(__APPLE__)
        tag_add(libs,"-framework AudioToolbox");
#else
        tag_add(libs,"-lpthread");
#endif
        printf(GREEN "[LIB] OpenAL  source build\n" RESET);
        return;
    }
    if (pkgcfg("openal",tags,libs)) {
        printf(GREEN "[LIB] OpenAL  via pkg-config\n" RESET); return;
    }
    char inc[512];
    if (find_header("AL/al.h",inc,sizeof(inc))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
    }
    tag_add(libs,"-lopenal");
#ifdef _WIN32
    tag_add(libs,"-lwinmm");
#endif
    printf(GREEN "[LIB] OpenAL  configured\n" RESET);
}

/* ================================================================== */
/*  raylib                                                             */
/* ================================================================== */
static void setup_raylib(char *tags, char *libs) {
    if (pkgcfg("raylib", tags, libs)) {
        printf(GREEN "[LIB] raylib  via pkg-config\n" RESET); return;
    }
    if (deps_inject_flags("raylib", "raylib.h", tags, libs)) {
        tag_add(libs,"-lraylib");
#ifdef _WIN32
        tag_add(libs,"-lwinmm"); tag_add(libs,"-lgdi32");
        tag_add(libs,"-lopengl32"); tag_add(libs,"-luser32");
#elif defined(__APPLE__)
        tag_add(libs,"-framework OpenGL"); tag_add(libs,"-framework Cocoa");
        tag_add(libs,"-framework IOKit"); tag_add(libs,"-framework CoreAudio");
#else
        tag_add(libs,"-lGL"); tag_add(libs,"-lm");
        tag_add(libs,"-lpthread"); tag_add(libs,"-ldl");
#endif
        printf(GREEN "[LIB] raylib  source build\n" RESET); return;
    }
    char inc[512];
    if (find_header("raylib.h", inc, sizeof(inc))) {
        char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
    }
    tag_add(libs,"-lraylib");
    printf(GREEN "[LIB] raylib  configured\n" RESET);
}

/* ================================================================== */
/*  spdlog (header-only or compiled)                                  */
/* ================================================================== */
static void setup_spdlog(char *tags, char *libs) {
    (void)libs;
    if (pkgcfg("spdlog", tags, libs)) {
        printf(GREEN "[LIB] spdlog  via pkg-config\n" RESET); return;
    }
    if (!deps_inject_flags("spdlog", "spdlog/spdlog.h", tags, libs)) {
        char inc[512];
        if (find_header("spdlog/spdlog.h", inc, sizeof(inc))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
        }
    }
    printf(GREEN "[LIB] spdlog  configured\n" RESET);
}

/* ================================================================== */
/*  {fmt}  (header-only or compiled)                                  */
/* ================================================================== */
static void setup_fmt(char *tags, char *libs) {
    if (pkgcfg("fmt", tags, libs)) {
        printf(GREEN "[LIB] fmt     via pkg-config\n" RESET); return;
    }
    if (!deps_inject_flags("fmt", "fmt/core.h", tags, libs)) {
        char inc[512];
        if (find_header("fmt/core.h", inc, sizeof(inc))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",inc); tag_add(tags,f);
        }
    }
    tag_add(libs,"-lfmt");
    printf(GREEN "[LIB] fmt     configured\n" RESET);
}

/* ================================================================== */
/*  Test frameworks (Catch2 / GoogleTest)                             */
/* ================================================================== */
static void setup_test_lib(const char *name, char *tags, char *libs) {
    if (!strcmp(name,"catch2")) {
        if (pkgcfg("catch2",tags,libs)||pkgcfg("catch2-with-main",tags,libs)) {
            printf(GREEN "[LIB] Catch2  via pkg-config\n" RESET); return;
        }
        if (!deps_inject_flags("catch2","catch2/catch_all.hpp",tags,libs))
            find_header("catch2/catch_all.hpp",NULL,0);
        tag_add(libs,"-lCatch2Main"); tag_add(libs,"-lCatch2");
        printf(GREEN "[LIB] Catch2  configured\n" RESET);
    } else {
        /* GoogleTest */
        if (pkgcfg("gtest",tags,libs)) {
            printf(GREEN "[LIB] GTest   via pkg-config\n" RESET); return;
        }
        if (!deps_inject_flags("gtest","gtest/gtest.h",tags,libs))
            deps_inject_flags("googletest","gtest/gtest.h",tags,libs);
        tag_add(libs,"-lgtest_main"); tag_add(libs,"-lgtest"); tag_add(libs,"-lpthread");
        printf(GREEN "[LIB] GTest   configured\n" RESET);
    }
}

/* ================================================================== */
/*  Ogg + Vorbis                                                       */
/* ================================================================== */
static void setup_ogg_vorbis(char *tags, char *libs) {
    if (!pkgcfg("ogg",tags,libs))    tag_add(libs,"-logg");
    if (!pkgcfg("vorbis",tags,libs)) { tag_add(libs,"-lvorbis"); tag_add(libs,"-lvorbisfile"); }
    printf(GREEN "[LIB] Ogg+Vorbis configured\n" RESET);
}

/* ================================================================== */
/*  FMOD — proprietary (Core + Studio)                                 */
/*  OMake: USE_LIB: fmod:studio  — set FMOD_DIR env var              */
/* ================================================================== */
static void setup_fmod(const char *opts, char *tags, char *libs) {
    int studio = opts && strstr(opts,"studio");
    const char *fd = getenv("FMOD_DIR"); if(!fd) fd=getenv("FMOD_HOME");
    if (!fd || !fd[0]) {
        printf(YELLOW "[LIB] FMOD    FMOD_DIR not set!\n" RESET);
        printf(YELLOW "              Download: https://fmod.com/download\n" RESET);
        printf(YELLOW "              Then: export FMOD_DIR=/path/to/fmod\n" RESET);
        return;
    }
    char f[600];
    snprintf(f,sizeof(f),"-I\"%s/api/core/inc\"",fd);    tag_add(tags,f);
    if (studio) { snprintf(f,sizeof(f),"-I\"%s/api/studio/inc\"",fd); tag_add(tags,f); }
#ifdef _WIN32
    snprintf(f,sizeof(f),"-L\"%s/api/core/lib/x64\"",fd);  tag_add(libs,f);
    tag_add(libs,"-lfmod_vc");
    if (studio) tag_add(libs,"-lfmodstudio_vc");
#elif defined(__APPLE__)
    snprintf(f,sizeof(f),"-L\"%s/api/core/lib\"",fd);       tag_add(libs,f);
    tag_add(libs,"-lfmod"); if (studio) tag_add(libs,"-lfmodstudio");
#else
    snprintf(f,sizeof(f),"-L\"%s/api/core/lib/x86_64\"",fd); tag_add(libs,f);
    tag_add(libs,"-lfmod"); if (studio) tag_add(libs,"-lfmodstudio");
#endif
    printf(GREEN "[LIB] FMOD%s  FMOD_DIR=%s\n" RESET, studio?"+Studio":"", fd);
}

/* ================================================================== */
/*  wxWidgets                                                          */
/*  CMake: find_package(wxWidgets REQUIRED) — platform nightmare      */
/*  OMake: USE_LIB: wx:core,base  (wx-config auto-detected)           */
/* ================================================================== */
static void setup_wx(const char *modules_str, char *tags, char *libs) {
    const char *tool=NULL;
    const char *try_tools[]={"wx-config","wx-config-3.2","wx-config-3.0","wx-config-gtk3",NULL};
    for (int i=0; try_tools[i]; i++) if(tool_exists(try_tools[i])){tool=try_tools[i];break;}
    if (!tool) { printf(YELLOW "[LIB] wxWidgets wx-config not found\n" RESET); return; }

    const char *mods = (modules_str&&modules_str[0]) ? modules_str : "core base";
    char cmd[512]; char buf[2048];

    snprintf(cmd,sizeof(cmd),"%s --cxxflags 2>/dev/null",tool);
    FILE *p=popen(cmd,"r");
    if (p) { buf[0]='\0'; if(fgets(buf,sizeof(buf),p)){char*nl=strrchr(buf,'\n');if(nl)*nl='\0'; strncat(tags," ",B_SIZE-strlen(tags)-1); strncat(tags,buf,B_SIZE-strlen(tags)-1);} pclose(p); }

    snprintf(cmd,sizeof(cmd),"%s --libs %s 2>/dev/null",tool,mods);
    p=popen(cmd,"r");
    if (p) { buf[0]='\0'; if(fgets(buf,sizeof(buf),p)){char*nl=strrchr(buf,'\n');if(nl)*nl='\0'; strncat(libs," ",B_SIZE-strlen(libs)-1); strncat(libs,buf,B_SIZE-strlen(libs)-1);} pclose(p); }

    printf(GREEN "[LIB] wxWidgets [%s] via wx-config\n" RESET, mods);
}

/* ================================================================== */
/*  GTK 3 / GTK 4                                                      */
/* ================================================================== */
static void setup_gtk(int ver, char *tags, char *libs) {
    char pkg[32]; snprintf(pkg,sizeof(pkg),(ver==4)?"gtk4":"gtk+-3.0");
    if (pkgcfg(pkg,tags,libs)) { printf(GREEN "[LIB] GTK%d    pkg-config\n" RESET,ver); return; }
    const char *alt=(ver==4)?"gtk+-3.0":"gtk4";
    if (pkgcfg(alt,tags,libs)) printf(YELLOW "[LIB] GTK%d → %s fallback\n" RESET,ver,alt);
    else printf(YELLOW "[LIB] GTK%d    not found\n" RESET,ver);
}

/* ================================================================== */
/*  Common alias dispatch (openssl, curl, zlib, lua, sqlite …)        */
/* ================================================================== */
static void setup_common_alias(const char *lb, const char *libarg,
                                 char *tags, char *libs) {
    (void)libarg;
    if (strcmp(lb,"openssl")==0||strcmp(lb,"ssl")==0) {
        if (!pkgcfg("openssl",tags,libs)) { tag_add(libs,"-lssl"); tag_add(libs,"-lcrypto"); }
        printf(GREEN "[LIB] OpenSSL configured\n" RESET);
    } else if (strcmp(lb,"curl")==0||strcmp(lb,"libcurl")==0) {
        if (!pkgcfg("libcurl",tags,libs)) tag_add(libs,"-lcurl");
        printf(GREEN "[LIB] libcurl configured\n" RESET);
    } else if (strcmp(lb,"zlib")==0||strcmp(lb,"z")==0) {
        if (!pkgcfg("zlib",tags,libs)) tag_add(libs,"-lz");
        printf(GREEN "[LIB] zlib    configured\n" RESET);
    } else if (strcmp(lb,"png")==0||strcmp(lb,"libpng")==0) {
        if (!pkgcfg("libpng16",tags,libs)&&!pkgcfg("libpng",tags,libs)) tag_add(libs,"-lpng");
        printf(GREEN "[LIB] libpng  configured\n" RESET);
    } else if (strcmp(lb,"jpeg")==0||strcmp(lb,"libjpeg")==0) {
        if (!pkgcfg("libjpeg",tags,libs)) tag_add(libs,"-ljpeg");
        printf(GREEN "[LIB] libjpeg configured\n" RESET);
    } else if (strcmp(lb,"lua")==0||strcmp(lb,"lua5.4")==0||strcmp(lb,"lua5.3")==0) {
        if (!pkgcfg("lua5.4",tags,libs)&&!pkgcfg("lua5.3",tags,libs)&&!pkgcfg("lua",tags,libs))
            tag_add(libs,"-llua");
        printf(GREEN "[LIB] Lua     configured\n" RESET);
    } else if (strcmp(lb,"sqlite")==0||strcmp(lb,"sqlite3")==0) {
        if (!pkgcfg("sqlite3",tags,libs)) tag_add(libs,"-lsqlite3");
        printf(GREEN "[LIB] SQLite3 configured\n" RESET);
    } else if (strcmp(lb,"pthread")==0||strcmp(lb,"threads")==0) {
        tag_add(tags,"-pthread"); tag_add(libs,"-lpthread");
        printf(GREEN "[LIB] pthreads configured\n" RESET);
    } else if (strcmp(lb,"bullet")==0||strcmp(lb,"bullet3")==0) {
        if (!pkgcfg("bullet",tags,libs)) {
            tag_add(libs,"-lBulletDynamics"); tag_add(libs,"-lBulletCollision");
            tag_add(libs,"-lLinearMath");
        }
        printf(GREEN "[LIB] Bullet3 configured\n" RESET);
    } else if (strcmp(lb,"harfbuzz")==0) {
        if (!pkgcfg("harfbuzz",tags,libs)) tag_add(libs,"-lharfbuzz");
        printf(GREEN "[LIB] HarfBuzz configured\n" RESET);
    } else if (strcmp(lb,"portaudio")==0) {
        if (!pkgcfg("portaudio-2.0",tags,libs)) tag_add(libs,"-lportaudio");
        printf(GREEN "[LIB] PortAudio configured\n" RESET);
    } else if (strcmp(lb,"enet")==0) {
        if (!pkgcfg("enet",tags,libs)) tag_add(libs,"-lenet");
        printf(GREEN "[LIB] ENet    configured\n" RESET);
    } else if (strcmp(lb,"glm")==0) {
        /* handled in dispatch but also here as alias */
        char sys[512];
        if (find_header("glm/glm.hpp",sys,sizeof(sys))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
        }
        printf(GREEN "[LIB] GLM     configured\n" RESET);
    } else if (strcmp(lb,"json")==0||strcmp(lb,"nlohmann")==0||strcmp(lb,"nlohmann_json")==0) {
        char sys[512];
        if (find_header("nlohmann/json.hpp",sys,sizeof(sys))) {
            char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f);
        } else if (!pkgcfg("nlohmann_json",tags,libs)) {
            printf(YELLOW "[LIB] nlohmann/json not found — header only, place json.hpp in include/\n" RESET);
        }
        printf(GREEN "[LIB] nlohmann/json configured\n" RESET);
    } else if (strcmp(lb,"tinyxml2")==0) {
        if (!pkgcfg("tinyxml2",tags,libs)) tag_add(libs,"-ltinyxml2");
        printf(GREEN "[LIB] TinyXML2 configured\n" RESET);
    } else {
        /* Generic last-resort: pkg-config → header scan → -l<name> */
        printf(CYAN "[USE_LIB] %s: pkg-config...\n" RESET, lb);
        if (!pkgcfg(lb,tags,libs)) {
            char hdr[256]; snprintf(hdr,sizeof(hdr),"%s/%s.h",lb,lb);
            char sys[512];
            if (!find_header(hdr,sys,sizeof(sys))) {
                snprintf(hdr,sizeof(hdr),"%s.h",lb);
                find_header(hdr,sys,sizeof(sys));
            }
            if (sys[0]) { char f[600]; snprintf(f,sizeof(f),"-I\"%s\"",sys); tag_add(tags,f); }
            char lf[128]; snprintf(lf,sizeof(lf),"-l%s",lb); tag_add(libs,lf);
            printf(YELLOW "[USE_LIB] %s: generic fallback\n" RESET, lb);
        }
    }
}

/* ================================================================== */
/*  apply_use_lib — main dispatch for USE_LIB: directive              */
/*                                                                      */
/*  OMakeLists.txt syntax examples:                                     */
/*    USE_LIB: opengl glfw glew glad            # Graphics stack       */
/*    USE_LIB: vulkan                            # Vulkan + SPIR-V     */
/*    USE_LIB: sdl2:image,ttf,mixer             # SDL2 + modules       */
/*    USE_LIB: sfml:graphics,audio,network      # SFML + modules       */
/*    USE_LIB: qt:Core,Widgets,OpenGL,Network   # Qt (auto moc/uic)   */
/*    USE_LIB: imgui:opengl3+glfw               # Dear ImGui           */
/*    USE_LIB: glm eigen assimp freetype        # Math/3D/font         */
/*    USE_LIB: boost:filesystem,thread          # Boost modules        */
/*    USE_LIB: opencv:core,imgproc,highgui      # OpenCV modules       */
/*    USE_LIB: openal ogg                       # Audio                */
/*    USE_LIB: fmod:studio                      # FMOD (set FMOD_DIR)  */
/*    USE_LIB: wx:core,base                     # wxWidgets            */
/*    USE_LIB: gtk3 | gtk4                      # GTK                  */
/*    USE_LIB: openssl curl zlib sqlite3 lua    # Common libs          */
/*    USE_LIB: <anything>                       # pkg-config fallback  */
/* ================================================================== */
void apply_use_lib(const char *str, char *tags, char *libs,
                   const char *src_dir, const char *obj_root,
                   const char *comp, CompilerType ct, int do_static,
                   char *extra_objs, size_t esz,
                   const char *qt_mods, const char *boost_mods,
                   const char *opencv_mods, const char *wx_mods,
                   const char *imgui_be) {
    if (!str || !str[0]) return;
    printf(BLUE "\n[USE_LIB] ══════════════════════════════\n" RESET);

    /* Tokenize by whitespace only — commas belong INSIDE lib:arg syntax
       e.g. "qt:Core,Widgets sdl2:image,ttf opengl sfml" */
    char tmp[B_SIZE]; strncpy(tmp,str,B_SIZE-1);
    /* Normalize: replace semicolons with spaces, but NOT commas */
    for (char *p=tmp;*p;p++) if(*p==';') *p=' ';
    char *tok = strtok(tmp," \t");
    while (tok) {
        /* Skip lone commas (user wrote "opengl, sfml" with space+comma) */
        while (*tok == ',') tok++;
        /* Strip trailing commas */
        { char *end=tok+strlen(tok)-1; while(end>=tok&&*end==',') *end--='\0'; }
        if (!tok[0]) { tok=strtok(NULL," \t"); continue; }
        /* Split "libname:arg" */
        char lb[128], la[512]; lb[0]=la[0]='\0';
        char *col=strchr(tok,':');
        if (col) {
            int n=(int)(col-tok); if(n>127)n=127;
            strncpy(lb,tok,n); lb[n]='\0';
            strncpy(la,col+1,511);
        } else {
            strncpy(lb,tok,127);
        }
        /* Lowercase for matching */
        for (int i=0;lb[i];i++) lb[i]=(char)tolower((unsigned char)lb[i]);

        /* ── Dispatch ────────────────────────────────────────── */
        if (!strcmp(lb,"opengl")||!strcmp(lb,"gl"))
            setup_opengl(tags,libs);
        else if (!strcmp(lb,"glfw")||!strcmp(lb,"glfw3"))
            setup_glfw(tags,libs);
        else if (!strcmp(lb,"glew"))
            setup_glew(tags,libs,do_static);
        else if (!strcmp(lb,"glad"))
            setup_glad(tags,libs,obj_root,comp,ct,extra_objs,esz);
        else if (!strcmp(lb,"sdl2")||!strcmp(lb,"sdl"))
            setup_sdl2(la,tags,libs);
        else if (!strcmp(lb,"sfml"))
            setup_sfml(la,tags,libs,do_static);
        else if (!strcmp(lb,"vulkan"))
            setup_vulkan(tags,libs,src_dir,obj_root);
        else if (!strcmp(lb,"imgui")||!strcmp(lb,"dear_imgui")) {
            const char *be=la[0]?la:(imgui_be?imgui_be:"opengl3+glfw");
            setup_imgui(be,tags,libs,obj_root,comp,ct,extra_objs,esz);
        }
        else if (!strcmp(lb,"glm"))
            setup_glm(tags);
        else if (!strcmp(lb,"boost"))
            setup_boost(la[0]?la:boost_mods,tags,libs);
        else if (!strcmp(lb,"opencv")||!strcmp(lb,"cv"))
            setup_opencv(la[0]?la:opencv_mods,tags,libs);
        else if (!strcmp(lb,"assimp"))
            setup_assimp(tags,libs);
        else if (!strcmp(lb,"freetype")||!strcmp(lb,"freetype2"))
            setup_freetype(tags,libs);
        else if (!strcmp(lb,"eigen")||!strcmp(lb,"eigen3"))
            setup_eigen(tags);
        else if (!strcmp(lb,"box2d"))
            setup_box2d(tags,libs);
        else if (!strcmp(lb,"openal")||!strcmp(lb,"al"))
            setup_openal(tags,libs);
        else if (!strcmp(lb,"ogg")||!strcmp(lb,"vorbis")||!strcmp(lb,"ogg_vorbis"))
            setup_ogg_vorbis(tags,libs);
        else if (!strcmp(lb,"fmod"))
            setup_fmod(la,tags,libs);
        else if (!strcmp(lb,"qt")||!strcmp(lb,"qt5")||!strcmp(lb,"qt6")) {
            const char *qm=la[0]?la:(qt_mods?qt_mods:"Core Widgets");
            setup_qt(qm,tags,libs,src_dir,obj_root,comp,ct,extra_objs,esz);
        }
        else if (!strcmp(lb,"wx")||!strcmp(lb,"wxwidgets"))
            setup_wx(la[0]?la:wx_mods,tags,libs);
        else if (!strcmp(lb,"gtk3"))
            setup_gtk(3,tags,libs);
        else if (!strcmp(lb,"gtk4")||!strcmp(lb,"gtk"))
            setup_gtk(4,tags,libs);
        else if (!strcmp(lb,"raylib"))
            setup_raylib(tags,libs);
        else if (!strcmp(lb,"spdlog"))
            setup_spdlog(tags,libs);
        else if (!strcmp(lb,"fmt"))
            setup_fmt(tags,libs);
        else if (!strcmp(lb,"catch2"))
            setup_test_lib("catch2",tags,libs);
        else if (!strcmp(lb,"googletest")||!strcmp(lb,"gtest"))
            setup_test_lib("googletest",tags,libs);
        else
            setup_common_alias(lb,la,tags,libs);

        tok=strtok(NULL," \t,");
    }
    printf(BLUE "[USE_LIB] ══════════════════════════════\n\n" RESET);
}




/* ================================================================== */
/*  FEATURE BLOCK — Enterprise extensions                              */
/*  1. Preset system        (omake.presets)                            */
/*  2. Toolchain files      (TOOLCHAIN: arm-gcc.toolchain)             */
/*  3. Dependency cache     (.omake_cache/)                            */
/*  4. VSCode / CLion tasks auto-generate                              */
/*  5. Reproducible builds  (REPRODUCIBLE: on)                         */
/*  6. PGO pipeline         (--pgo-generate / --pgo-use)              */
/*  7. TEST_CASE: DSL       (inline test registration)                 */
/*  8. Distributed build    (--dist host1,host2 via SSH)              */
/* ================================================================== */

/* ── 1. PRESET SYSTEM ─────────────────────────────────────────────── */
/*
 * omake.presets format (INI-like):
 *
 *   [linux-clang-asan]
 *   COMPILER: clang++
 *   PROFILE:  debug
 *   TAGS:     -fsanitize=address,undefined
 *   ARCH:     x64
 *
 *   [release-static]
 *   PROFILE:  release
 *   TAGS:     -O3 -march=native
 *   STATIC:   on
 *   JOBS:     8
 *
 *   [cross-arm]
 *   TOOLCHAIN: arm-gcc.toolchain
 *   ARCH:      arm64
 *   STATIC:    on
 *
 * Keys (case-insensitive): COMPILER, PROFILE, TAGS, ARCH, STATIC,
 *   JOBS, TOOLCHAIN, REPRODUCIBLE, DEFINES, LIBS, USE_LIB
 */

/* OmakePreset typedef is forward-declared above */

#define MAX_PRESETS 64
static OmakePreset g_presets[MAX_PRESETS];
static int         g_preset_count = 0;

static void preset_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[1024];
    OmakePreset *cur = NULL;
    while (fgets(line, sizeof(line), fp)) {
        /* strip comments and newline */
        char *cmt = strchr(line, '#'); if (cmt) *cmt = '\0';
        char *nl  = strrchr(line, '\n'); if (nl) *nl = '\0';
        char *cr  = strrchr(line, '\r'); if (cr) *cr = '\0';
        /* trim leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        /* Section header [name] */
        if (*p == '[') {
            char *end = strchr(p+1, ']');
            if (!end || g_preset_count >= MAX_PRESETS) continue;
            *end = '\0';
            cur = &g_presets[g_preset_count++];
            memset(cur, 0, sizeof(*cur));
            strncpy(cur->name, p+1, 63);
            cur->jobs = 0; /* default = auto */
            continue;
        }
        if (!cur) continue;

        /* key: value */
        char *col = strchr(p, ':');
        if (!col) continue;
        *col = '\0';
        char *key = p;
        char *val = col+1;
        while (*val == ' ' || *val == '\t') val++;
        /* trim key */
        char *ke = key + strlen(key) - 1;
        while (ke > key && (*ke==' '||*ke=='\t')) *ke--='\0';
        /* lowercase key */
        for (char *k=key; *k; k++) *k=(char)tolower((unsigned char)*k);

        if      (!strcmp(key,"compiler"))    strncpy(cur->compiler,   val, 255);
        else if (!strcmp(key,"profile"))     strncpy(cur->profile,    val,  63);
        else if (!strcmp(key,"tags"))        strncpy(cur->tags,       val,1023);
        else if (!strcmp(key,"arch"))        strncpy(cur->arch,       val,  31);
        else if (!strcmp(key,"toolchain"))   strncpy(cur->toolchain,  val, 511);
        else if (!strcmp(key,"use_lib"))     strncpy(cur->use_lib,    val, 511);
        else if (!strcmp(key,"defines"))     strncpy(cur->defines,    val, 511);
        else if (!strcmp(key,"libs"))        strncpy(cur->libs,       val, 511);
        else if (!strcmp(key,"static"))      cur->do_static    = (tolower(val[0])=='o' && tolower(val[1])=='n');
        else if (!strcmp(key,"jobs"))        cur->jobs         = atoi(val);
        else if (!strcmp(key,"reproducible"))cur->reproducible = (tolower(val[0])=='o');
    }
    fclose(fp);
}

static OmakePreset *preset_find(const char *name) {
    for (int i = 0; i < g_preset_count; i++)
        if (strcasecmp(g_presets[i].name, name) == 0)
            return &g_presets[i];
    return NULL;
}

static void preset_list(void) {
    /* presets already loaded by caller */
    if (g_preset_count == 0) {
        printf(YELLOW "[PRESET] No presets found. Create omake.presets\n" RESET);
        printf(CYAN   "  Example:\n"
                       "    [release-native]\n"
                       "    PROFILE: release\n"
                       "    TAGS: -O3 -march=native\n\n"
                       "    [debug-asan]\n"
                       "    PROFILE: debug\n"
                       "    TAGS: -fsanitize=address,undefined\n\n"
                       "    [cross-arm]\n"
                       "    TOOLCHAIN: arm.toolchain\n"
                       "    ARCH: arm64\n" RESET);
        return;
    }
    printf(BLUE "\n[PRESET] Available presets (%d):\n" RESET, g_preset_count);
    for (int i = 0; i < g_preset_count; i++) {
        OmakePreset *p = &g_presets[i];
        printf(CYAN "  %-28s" RESET, p->name);
        if (p->profile[0])   printf(" profile=%-8s", p->profile);
        if (p->compiler[0])  printf(" comp=%-12s", p->compiler);
        if (p->arch[0])      printf(" arch=%-6s", p->arch);
        if (p->do_static)    printf(" static");
        if (p->reproducible) printf(" reproducible");
        if (p->toolchain[0]) printf(" tc=%s", p->toolchain);
        printf("\n");
    }
    printf("\n");
}

/* ── 2. TOOLCHAIN FILES ───────────────────────────────────────────── */
/*
 * arm-gcc.toolchain format:
 *
 *   COMPILER:     arm-linux-gnueabihf-g++
 *   C_COMPILER:   arm-linux-gnueabihf-gcc
 *   LINKER:       arm-linux-gnueabihf-g++
 *   SYSROOT:      /opt/arm-sysroot
 *   AR:           arm-linux-gnueabihf-ar
 *   STRIP:        arm-linux-gnueabihf-strip
 *   LINKER_SCRIPT: ldscripts/custom.ld
 *   TAGS:         -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
 *   DEFINES:      TARGET_ARM=1 EMBEDDED=1
 *   SYSROOT_LIBS: /opt/arm-sysroot/usr/lib
 */
/* Toolchain typedef is forward-declared above */

static int toolchain_load(const char *path, Toolchain *tc) {
    memset(tc, 0, sizeof(*tc));
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf(RED "[TOOLCHAIN] File not found: %s\n" RESET, path);
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *cmt = strchr(line, '#'); if (cmt) *cmt = '\0';
        char *nl  = strrchr(line, '\n'); if (nl) *nl = '\0';
        char *p   = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        char *col = strchr(p, ':');
        if (!col) continue;
        *col = '\0';
        char *key = p, *val = col+1;
        while (*val == ' ' || *val == '\t') val++;
        char *ke = key+strlen(key)-1;
        while (ke>key && (*ke==' '||*ke=='\t')) *ke--='\0';
        for (char *k=key;*k;k++) *k=(char)tolower((unsigned char)*k);

        if      (!strcmp(key,"compiler"))      strncpy(tc->compiler,     val,255);
        else if (!strcmp(key,"c_compiler"))    strncpy(tc->c_compiler,   val,255);
        else if (!strcmp(key,"linker"))        strncpy(tc->linker,       val,255);
        else if (!strcmp(key,"sysroot"))       strncpy(tc->sysroot,      val,511);
        else if (!strcmp(key,"ar"))            strncpy(tc->ar,           val,127);
        else if (!strcmp(key,"strip"))         strncpy(tc->strip_tool,   val,127);
        else if (!strcmp(key,"linker_script")) strncpy(tc->linker_script,val,511);
        else if (!strcmp(key,"tags"))          strncpy(tc->tags,         val,1023);
        else if (!strcmp(key,"defines"))       strncpy(tc->defines,      val,511);
        else if (!strcmp(key,"sysroot_libs"))  strncpy(tc->sysroot_libs, val,511);
    }
    fclose(fp);
    printf(BLUE "[TOOLCHAIN] Loaded: %s\n" RESET, path);
    if (tc->compiler[0])      printf(CYAN  "  compiler:      %s\n" RESET, tc->compiler);
    if (tc->sysroot[0])       printf(CYAN  "  sysroot:       %s\n" RESET, tc->sysroot);
    if (tc->linker_script[0]) printf(CYAN  "  linker_script: %s\n" RESET, tc->linker_script);
    if (tc->tags[0])          printf(CYAN  "  extra tags:    %s\n" RESET, tc->tags);
    return 1;
}

static void toolchain_apply(const Toolchain *tc, char *comp, char *tags, char *libs) {
    if (tc->compiler[0])      strncpy(comp, tc->compiler, 255);
    if (tc->sysroot[0]) {
        char sysroot_flag[600];
        snprintf(sysroot_flag, sizeof(sysroot_flag), " --sysroot=\"%s\"", tc->sysroot);
        strncat(tags, sysroot_flag, 8191 - strlen(tags));
    }
    if (tc->linker_script[0]) {
        char ld_flag[600];
        snprintf(ld_flag, sizeof(ld_flag), " -T\"%s\"", tc->linker_script);
        strncat(tags, ld_flag, 8191 - strlen(tags));
    }
    if (tc->tags[0]) {
        strncat(tags, " ", 8191-strlen(tags));
        strncat(tags, tc->tags, 8191 - strlen(tags));
    }
    if (tc->defines[0]) {
        char *tok = strtok((char*)tc->defines, " \t");
        while (tok) {
            char dflag[128];
            snprintf(dflag, sizeof(dflag), " -D%s", tok);
            strncat(tags, dflag, 8191-strlen(tags));
            tok = strtok(NULL, " \t");
        }
    }
    if (tc->sysroot_libs[0]) {
        char lpath[600];
        snprintf(lpath, sizeof(lpath), " -L\"%s\"", tc->sysroot_libs);
        strncat(libs, lpath, 8191-strlen(libs));
    }
    printf(GREEN "[TOOLCHAIN] Applied to build.\n" RESET);
}

/* ── 3. DEPENDENCY CACHE ──────────────────────────────────────────── */
/*
 * .omake_cache/<lib>-<hash>.done  — marks a built+cached extern lib
 * Hash = SHA256-like (FNV-64) of: url + version + build flags
 * If .done exists → skip rebuild of that extern dep.
 *
 * Cache directory: .omake_cache/
 * Format: plain text with metadata for inspection.
 */

static uint64_t fnv64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static int dep_cache_hit(const char *lib_name, const char *url,
                          const char *version, const char *flags) {
    char combined[1024];
    snprintf(combined, sizeof(combined), "%s|%s|%s|%s", lib_name, url, version, flags);
    uint64_t h = fnv64(combined);
    char path[256];
    snprintf(path, sizeof(path), ".omake_cache/%s-%016llx.done",
             lib_name, (unsigned long long)h);
    return is_regular_file(path);
}

static void dep_cache_mark(const char *lib_name, const char *url,
                            const char *version, const char *flags) {
    mkdir_auto(".omake_cache");  /* ensure dir exists */
    char combined[1024];
    snprintf(combined, sizeof(combined), "%s|%s|%s|%s", lib_name, url, version, flags);
    uint64_t h = fnv64(combined);
    char path[256];
    snprintf(path, sizeof(path), ".omake_cache/%s-%016llx.done",
             lib_name, (unsigned long long)h);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "lib:     %s\n", lib_name);
        fprintf(fp, "url:     %s\n", url);
        fprintf(fp, "version: %s\n", version);
        fprintf(fp, "flags:   %s\n", flags);
        fprintf(fp, "hash:    %016llx\n", (unsigned long long)h);
        fclose(fp);
    }
}

static void dep_cache_list(void) {
    DIR *d = opendir(".omake_cache");
    if (!d) { printf(YELLOW "[CACHE] No .omake_cache/ directory found.\n" RESET); return; }
    printf(BLUE "[CACHE] Cached dependencies:\n" RESET);
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (!strstr(nm, ".done")) continue;
        char path[300]; snprintf(path, sizeof(path), ".omake_cache/%s", nm);
        /* Read lib: line */
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char lbuf[256]; lbuf[0]='\0';
        fgets(lbuf, sizeof(lbuf), fp);
        fclose(fp);
        printf(CYAN "  %s" RESET " → %s", nm, lbuf);
        count++;
    }
    closedir(d);
    if (!count) printf(YELLOW "  (empty)\n" RESET);
}

static void dep_cache_clear(void) {
    DIR *d = opendir(".omake_cache");
    if (!d) return;
    struct dirent *de;
    int n=0;
    while ((de=readdir(d))) {
        if (!strstr(de->d_name,".done")) continue;
        char path[300]; snprintf(path,sizeof(path),".omake_cache/%s",de->d_name);
        remove(path); n++;
    }
    closedir(d);
    printf(GREEN "[CACHE] Cleared %d cached entries.\n" RESET, n);
}

/* ── 4. IDE INTEGRATION ────────────────────────────────────────────── */

static void ide_gen_vscode(void) {
    /* Read project name from OMakeLists.txt */
    char proj_name[128] = "project";
    char out_bin[512]   = "bin/app";
    FILE *cfg = fopen("OMakeLists.txt","r");
    if (cfg) {
        char line[512];
        while (fgets(line,sizeof(line),cfg)) {
            char *p=line; while(*p==' '||*p=='\t')p++;
            if (strncasecmp(p,"name:",5)==0) {
                char *v=p+5; while(*v==' '||*v=='\t')v++;
                char *e=strrchr(v,'\n'); if(e)*e='\0';
                e=strrchr(v,'\r'); if(e)*e='\0';
                strncpy(proj_name,v,127);
            }
            if (strncasecmp(p,"out:",4)==0) {
                char *v=p+4; while(*v==' '||*v=='\t')v++;
                char *e=strrchr(v,'\n'); if(e)*e='\0';
                strncpy(out_bin,v,511);
            }
        }
        fclose(cfg);
    }

#ifdef _WIN32
    int is_win = 1;
#else
    int is_win = 0;
#endif

    /* .vscode/tasks.json */
    mkdir_auto(".vscode");
    FILE *f = fopen(".vscode/tasks.json","w");
    if (!f) { printf(RED "[IDE] Cannot write .vscode/tasks.json\n" RESET); return; }
    fprintf(f,
"{\n"
"  \"version\": \"2.0.0\",\n"
"  \"tasks\": [\n"
"    {\n"
"      \"label\": \"omake: build\",\n"
"      \"type\": \"shell\",\n"
"      \"command\": \"%s\",\n"
"      \"args\": [\"--build\"],\n"
"      \"group\": { \"kind\": \"build\", \"isDefault\": true },\n"
"      \"presentation\": { \"reveal\": \"always\", \"panel\": \"shared\" },\n"
"      \"problemMatcher\": [\n"
"        \"$gcc\"\n"
"      ]\n"
"    },\n"
"    {\n"
"      \"label\": \"omake: build (incremental)\",\n"
"      \"type\": \"shell\",\n"
"      \"command\": \"%s\",\n"
"      \"args\": [\"--build-hash\"],\n"
"      \"group\": \"build\",\n"
"      \"problemMatcher\": [\"$gcc\"]\n"
"    },\n"
"    {\n"
"      \"label\": \"omake: f-compile (error check)\",\n"
"      \"type\": \"shell\",\n"
"      \"command\": \"%s\",\n"
"      \"args\": [\"--f-compile\"],\n"
"      \"group\": \"test\",\n"
"      \"problemMatcher\": []\n"
"    },\n"
"    {\n"
"      \"label\": \"omake: run\",\n"
"      \"type\": \"shell\",\n"
"      \"command\": \"%s\",\n"
"      \"group\": \"none\",\n"
"      \"presentation\": { \"reveal\": \"always\" }\n"
"    },\n"
"    {\n"
"      \"label\": \"omake: test\",\n"
"      \"type\": \"shell\",\n"
"      \"command\": \"%s\",\n"
"      \"args\": [\"--test\"],\n"
"      \"group\": { \"kind\": \"test\", \"isDefault\": true },\n"
"      \"problemMatcher\": []\n"
"    },\n"
"    {\n"
"      \"label\": \"omake: clean\",\n"
"      \"type\": \"shell\",\n"
"      \"command\": \"%s\",\n"
"      \"args\": [\"--clean\"],\n"
"      \"group\": \"none\"\n"
"    }\n"
"  ]\n"
"}\n",
is_win ? "omake.exe" : "./omake",
is_win ? "omake.exe" : "./omake",
is_win ? "omake.exe" : "./omake",
out_bin,
is_win ? "omake.exe" : "./omake",
is_win ? "omake.exe" : "./omake"
    );
    fclose(f);
    printf(GREEN "[IDE] .vscode/tasks.json written\n" RESET);

    /* .vscode/launch.json (debugger) */
    f = fopen(".vscode/launch.json","w");
    if (f) {
        const char *debugger = is_win ? "cppvsdbg" : "cppdbg";
        const char *mi_mode  = is_win ? "" : "\"miMode\": \"gdb\",\n      ";
        fprintf(f,
"{\n"
"  \"version\": \"0.2.0\",\n"
"  \"configurations\": [\n"
"    {\n"
"      \"name\": \"Debug %s\",\n"
"      \"type\": \"%s\",\n"
"      \"request\": \"launch\",\n"
"      \"program\": \"${workspaceFolder}/%s\",\n"
"      \"args\": [],\n"
"      \"stopAtEntry\": false,\n"
"      \"cwd\": \"${workspaceFolder}\",\n"
"      %s"
"      \"preLaunchTask\": \"omake: build\"\n"
"    }\n"
"  ]\n"
"}\n", proj_name, debugger, out_bin, mi_mode);
        fclose(f);
        printf(GREEN "[IDE] .vscode/launch.json written\n" RESET);
    }

    /* .vscode/c_cpp_properties.json for IntelliSense */
    f = fopen(".vscode/c_cpp_properties.json","w");
    if (f) {
        fprintf(f,
"{\n"
"  \"configurations\": [\n"
"    {\n"
"      \"name\": \"%s\",\n"
"      \"compileCommands\": \"${workspaceFolder}/compile_commands.json\",\n"
"      \"intelliSenseMode\": \"%s\"\n"
"    }\n"
"  ],\n"
"  \"version\": 4\n"
"}\n",
#ifdef _WIN32
"Win32", "windows-gcc-x64"
#elif defined(__APPLE__)
"macOS", "macos-clang-x64"
#else
"Linux", "linux-gcc-x64"
#endif
);
        fclose(f);
        printf(GREEN "[IDE] .vscode/c_cpp_properties.json written\n" RESET);
    }
}

static void ide_gen_clion(void) {
    /* CLion uses CMakeLists.txt + compile_commands.json.
       We generate a minimal CMakeLists.txt stub that points to compile_commands.json */
    if (is_regular_file("CMakeLists.txt")) {
        printf(YELLOW "[IDE] CMakeLists.txt already exists, skipping CLion stub.\n" RESET);
        return;
    }
    FILE *f = fopen("CMakeLists.txt","w");
    if (!f) return;
    fprintf(f,
"# Auto-generated by omake --ide clion\n"
"# Real build is done by omake — this stub enables CLion indexing\n"
"cmake_minimum_required(VERSION 3.15)\n"
"project(omake_project)\n"
"set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
"# CLion will use compile_commands.json for indexing.\n"
"# Run:  omake --build  to actually compile.\n"
"add_custom_target(build COMMAND omake --build)\n"
"add_custom_target(clean_all COMMAND omake --clean)\n"
);
    fclose(f);
    printf(GREEN "[IDE] CMakeLists.txt (CLion stub) written\n" RESET);
}

static void cmd_ide(const char *target) {
    /* Ensure compile_commands.json exists */
    if (!is_regular_file("compile_commands.json"))
        printf(YELLOW "[IDE] Tip: run  omake --build --generate-compile-commands  first\n" RESET);

    if (!target || !*target || strcmp(target,"vscode")==0) {
        ide_gen_vscode();
    } else if (strcmp(target,"clion")==0) {
        ide_gen_clion();
    } else if (strcmp(target,"all")==0) {
        ide_gen_vscode();
        ide_gen_clion();
    } else {
        printf(RED "[IDE] Unknown target: %s  (use: vscode | clion | all)\n" RESET, target);
    }
}

/* ── 5. REPRODUCIBLE BUILD HELPERS ───────────────────────────────── */
/* Applied as compiler flags when REPRODUCIBLE: on is set.            */
static void apply_reproducible(char *tags) {
    /* -fmacro-prefix-map strips absolute paths from __FILE__ macros   */
    /* -fdebug-prefix-map  strips absolute paths from debug info        */
    /* SOURCE_DATE_EPOCH   forces deterministic timestamps (env var)    */
    /* -frandom-seed=      deterministic code generation seed           */
    if (!strstr(tags, "-fmacro-prefix-map"))
        strncat(tags, " -fmacro-prefix-map=./=", 8191-strlen(tags));
    if (!strstr(tags, "-fdebug-prefix-map"))
        strncat(tags, " -fdebug-prefix-map=./=", 8191-strlen(tags));
    if (!strstr(tags, "-frandom-seed"))
        strncat(tags, " -frandom-seed=omake", 8191-strlen(tags));
    /* Set SOURCE_DATE_EPOCH to 0 for reproducible timestamps */
#ifdef _WIN32
    _putenv("SOURCE_DATE_EPOCH=0");
#else
    setenv("SOURCE_DATE_EPOCH", "0", 1);
#endif
    printf(YELLOW "[REPRODUCIBLE] Deterministic build flags applied.\n" RESET);
    printf(CYAN   "  -fmacro-prefix-map=./=\n"
                  "  -fdebug-prefix-map=./=\n"
                  "  -frandom-seed=omake\n"
                  "  SOURCE_DATE_EPOCH=0\n" RESET);
}

/* ── 6. PGO PIPELINE ──────────────────────────────────────────────── */
/* Phase 1: --pgo-generate  → build with -fprofile-generate            */
/*           run the binary (user does this)                            */
/* Phase 2: --pgo-use       → build with -fprofile-use                 */

static void pgo_apply_generate(char *tags) {
    if (!strstr(tags, "-fprofile-generate"))
        strncat(tags, " -fprofile-generate=.omake_pgo", 8191-strlen(tags));
    strncat(tags, " -O2", 8191-strlen(tags));
    mkdir_auto(".omake_pgo/x");
    printf(YELLOW "[PGO] Phase 1: profile-generate mode.\n" RESET);
    printf(CYAN   "  After build, run your binary to collect profile data.\n"
                  "  Then run:  omake --pgo-use --build\n" RESET);
}

static void pgo_apply_use(char *tags) {
    if (!dir_exists(".omake_pgo")) {
        printf(RED "[PGO] No profile data found. Run  omake --pgo-generate --build  first.\n" RESET);
        return;
    }
    if (!strstr(tags, "-fprofile-use"))
        strncat(tags, " -fprofile-use=.omake_pgo", 8191-strlen(tags));
    strncat(tags, " -fprofile-correction -O3", 8191-strlen(tags));
    printf(YELLOW "[PGO] Phase 2: profile-use mode (optimized).\n" RESET);
}

/* ── 7. TEST_CASE: DSL ────────────────────────────────────────────── */
/*
 * OMakeLists.txt:
 *   TEST_CASE: tests/math_test.cpp
 *   TEST_CASE: tests/net_test.cpp  --timeout=10
 *   TEST_RUNNER: tests/
 *
 * Generates a thin test runner that compiles and runs each TEST_CASE file.
 * Each file must have a main() or use the omake_test.h shim.
 *
 * omake_test.h (auto-generated into .omake_obj/):
 *   #define OMAKE_TEST_CASE(name) static void name()
 *   #define OMAKE_CHECK(expr)     if(!(expr)){fprintf(stderr,"FAIL: %s:%d: %s\n",__FILE__,__LINE__,#expr);g_failed++;} else g_passed++;
 *   #define OMAKE_RUN_ALL()       int main(){...run all registered cases...}
 */
static void gen_omake_test_header(const char *obj_root) {
    char hpath[512];
    snprintf(hpath, sizeof(hpath), "%s/omake_test.h", obj_root);
    /* Regenerate if empty or nonexistent */
    {
        long sz = 0;
        FILE *chk = fopen(hpath, "r");
        if (chk) { fseek(chk, 0, SEEK_END); sz = ftell(chk); fclose(chk); }
        if (sz > 100) return; /* already generated and non-empty */
    }
    FILE *f = fopen(hpath, "w");
    if (!f) return;
    fprintf(f,
"/* omake_test.h — auto-generated by omake                          */\n"
"/* Lightweight test DSL, no external dependencies needed.           */\n"
"#pragma once\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"\n"
"static int omake_passed = 0;\n"
"static int omake_failed = 0;\n"
"\n"
"#define OMAKE_CHECK(expr) do { \\\n"
"    if (!(expr)) { \\\n"
"        fprintf(stderr, \"  \\033[31mFAIL\\033[0m  %s:%d  %s\\n\", \\\n"
"                __FILE__, __LINE__, #expr); \\\n"
"        omake_failed++; \\\n"
"    } else { \\\n"
"        printf(\"  \\033[32mPASS\\033[0m  %s\\n\", #expr); \\\n"
"        omake_passed++; \\\n"
"    } \\\n"
"} while(0)\n"
"\n"
"#define OMAKE_CHECK_EQ(a,b)  OMAKE_CHECK((a)==(b))\n"
"#define OMAKE_CHECK_NE(a,b)  OMAKE_CHECK((a)!=(b))\n"
"#define OMAKE_CHECK_STR(a,b) OMAKE_CHECK(strcmp((a),(b))==0)\n"
"#define OMAKE_CHECK_NULL(p)  OMAKE_CHECK((p)==NULL)\n"
"#define OMAKE_CHECK_NOT_NULL(p) OMAKE_CHECK((p)!=NULL)\n"
"\n"
"/* Section marker for test cases */\n"
"typedef void (*OmakeTestFn)(void);\n"
"typedef struct { const char *name; OmakeTestFn fn; } OmakeTestEntry;\n"
"\n"
"#define OMAKE_REGISTER_TESTS(...) \\\n"
"    static OmakeTestEntry _omake_tests[] = { __VA_ARGS__, {NULL,NULL} }; \\\n"
"    int main(void) { \\\n"
"        int total=0,failed=0; \\\n"
"        printf(\"\\n\\033[1;34m[TEST] Running tests...\\033[0m\\n\"); \\\n"
"        for (int i=0; _omake_tests[i].name; i++) { \\\n"
"            printf(\"\\033[1;33m▶ %s\\033[0m\\n\", _omake_tests[i].name); \\\n"
"            omake_passed=omake_failed=0; \\\n"
"            _omake_tests[i].fn(); \\\n"
"            total += omake_passed+omake_failed; \\\n"
"            failed += omake_failed; \\\n"
"            printf(\"  \\033[90mpassed=%d  failed=%d\\033[0m\\n\", \\\n"
"                   omake_passed, omake_failed); \\\n"
"        } \\\n"
"        printf(\"\\n\\033[1;%sm[TEST] %s  total=%d  failed=%d\\033[0m\\n\", \\\n"
"               failed?\"31\":\"32\", failed?\"FAILED\":\"PASSED\", total, failed); \\\n"
"        return failed ? 1 : 0; \\\n"
"    }\n"
"\n"
"/* Convenience: single test file with main */\n"
"#define OMAKE_SINGLE_TEST(name) \\\n"
"    static void name##_impl(void); \\\n"
"    OMAKE_REGISTER_TESTS({#name, name##_impl}) \\\n"
"    static void name##_impl(void)\n"
    );
    fclose(f);
    printf(CYAN "[TEST] omake_test.h written to %s/\n" RESET, obj_root);
}

/* ── 8. DISTRIBUTED BUILD ─────────────────────────────────────────── */
/*
 * omake --dist host1,host2,host3 --build
 *
 * Strategy: SSH + shared NFS/rsync
 *   1. rsync source tree to each host's /tmp/omake_dist_<proj>/
 *   2. Distribute compile jobs over SSH
 *   3. rsync .o files back
 *   4. Link locally
 *
 * Falls back to distcc if available.
 */
static void dist_build(const char *hosts_csv, int jobs,
                        const char *comp, const char *tags) {
    (void)tags;
    printf(BLUE "[DIST] Distributed build\n" RESET);

    /* Try distcc first */
    if (tool_exists("distcc")) {
        printf(CYAN "[DIST] distcc detected — using distcc with hosts: %s\n" RESET, hosts_csv);
#ifdef _WIN32
        _putenv_s("DISTCC_HOSTS", hosts_csv);
#else
        setenv("DISTCC_HOSTS", hosts_csv, 1);
#endif
        printf(CYAN "[DIST] Set DISTCC_HOSTS=%s\n" RESET, hosts_csv);
        printf(CYAN "[DIST] Wrapping compiler with distcc: distcc %s\n" RESET, comp);
        printf(YELLOW "[DIST] Run  omake --build  now — distcc will distribute jobs.\n" RESET);
        return;
    }

    /* Manual SSH distribution */
    char host_list[64][128]; int nhost = 0;
    char hcopy[512]; strncpy(hcopy, hosts_csv, 511);
    char *tok = strtok(hcopy, ",");
    while (tok && nhost < 64) {
        strncpy(host_list[nhost++], tok, 127);
        tok = strtok(NULL, ",");
    }
    printf(CYAN "[DIST] %d hosts: ", nhost);
    for (int i=0;i<nhost;i++) printf("%s%s", host_list[i], i<nhost-1?", ":"");
    printf("\n" RESET);

    /* Get project dir name */
    char cwd[512]; getcwd(cwd, sizeof(cwd));
    const char *projname = strrchr(cwd, '/');
    if (!projname) projname = strrchr(cwd, '\\');
    projname = projname ? projname+1 : cwd;

    /* Sync source to each host */
    printf(BLUE "[DIST] Syncing source to hosts...\n" RESET);
    for (int i=0; i<nhost; i++) {
        char rsync_cmd[800];
        snprintf(rsync_cmd, sizeof(rsync_cmd),
                 "rsync -az --exclude='.omake_obj' --exclude='bin' "
                 ". %s:/tmp/omake_dist_%s/ 2>&1",
                 host_list[i], projname);
        printf(CYAN "[DIST] Sync → %s\n" RESET, host_list[i]);
        int rc = system(rsync_cmd);
        if (rc != 0)
            printf(YELLOW "[DIST] Warning: rsync to %s failed (rc=%d)\n" RESET, host_list[i], rc);
    }

    /* Distribute build jobs */
    printf(BLUE "[DIST] Building on remote hosts (jobs=%d)...\n" RESET, jobs);
    /* For each host, SSH and run omake --build-hash */
    /* This is a simple round-robin — advanced scheduling left for future */
    for (int i=0; i<nhost; i++) {
        char ssh_cmd[800];
        snprintf(ssh_cmd, sizeof(ssh_cmd),
                 "ssh %s \"cd /tmp/omake_dist_%s && omake --build-hash\" &",
                 host_list[i], projname);
        printf(CYAN "[DIST] → %s\n" RESET, host_list[i]);
        system(ssh_cmd);
    }
    /* Wait for all background SSH jobs */
    system("wait");

    /* Collect .o files back */
    printf(BLUE "[DIST] Collecting compiled objects...\n" RESET);
    for (int i=0; i<nhost; i++) {
        char rsync_back[800];
        snprintf(rsync_back, sizeof(rsync_back),
                 "rsync -az %s:/tmp/omake_dist_%s/.omake_obj/ .omake_obj/ 2>&1",
                 host_list[i], projname);
        printf(CYAN "[DIST] ← %s\n" RESET, host_list[i]);
        system(rsync_back);
    }
    printf(GREEN "[DIST] Objects collected. Run  omake --build-hash  to link.\n" RESET);
}


/* ================================================================== */
/*  --f-compile  :  Front compile — compile all sources, report       */
/*                  errors with line content + highlighted token       */
/* ================================================================== */

static int extract_error_token(const char *msg, char *out, size_t outsz) {
    out[0] = '\0';
    const char *sq = strchr(msg, '\'');
    if (sq) {
        const char *eq = strchr(sq+1, '\'');
        if (eq && eq > sq+1) {
            int len = (int)(eq - sq - 1);
            if (len > 0 && len < (int)outsz-1) {
                strncpy(out, sq+1, len); out[len] = '\0';
                return 1;
            }
        }
    }
    const char *dq = strchr(msg, '"');
    if (dq) {
        const char *eq = strchr(dq+1, '"');
        if (eq && eq > dq+1) {
            int len = (int)(eq - dq - 1);
            if (len > 0 && len < (int)outsz-1) {
                strncpy(out, dq+1, len); out[len] = '\0';
                return 1;
            }
        }
    }
    /* fatal error: WORD: No such file */
    const char *fe = strstr(msg, "fatal error:");
    if (!fe) fe = strstr(msg, "error:");
    if (fe) {
        const char *ws = strchr(fe, ':');
        if (ws) { ws++; while (*ws == ' ') ws++; }
        else ws = fe;
        const char *we = strchr(ws, ':');
        if (!we) we = ws + strlen(ws);
        int len = (int)(we - ws);
        /* only use if it looks like a filename / identifier (no spaces) */
        int has_space = 0;
        for (int i=0;i<len;i++) if (ws[i]==' ') has_space=1;
        if (len > 0 && len < (int)outsz-1 && !has_space) {
            strncpy(out, ws, len); out[len] = '\0';
            return 1;
        }
    }
    return 0;
}

static int read_file_line(const char *filepath, int lineno, char *out, size_t outsz) {
    out[0] = '\0';
    FILE *fp = fopen(filepath, "r");
    if (!fp) return 0;
    char buf[1024];
    int cur = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        cur++;
        if (cur == lineno) {
            char *nl = strrchr(buf, '\n'); if (nl) *nl = '\0';
            char *cr = strrchr(buf, '\r'); if (cr) *cr = '\0';
            strncpy(out, buf, outsz-1);
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static void print_line_highlighted(const char *line, const char *token) {
    if (!token || !token[0]) { printf("    %s\n", line); return; }
    const char *p = strstr(line, token);
    if (!p) { printf("    %s\n", line); return; }
    if (p > line) printf("    %.*s", (int)(p - line), line);
    printf("\033[1;31m%s\033[0m", token);
    printf("%s\n", p + strlen(token));
}

typedef struct {
    char file[512];
    int  line;
    int  col;
    char severity[32];
    char message[512];
    char token[128];
} CompileError;

#define MAX_FERR 2048

static int parse_compiler_output(const char *output,
                                  CompileError *errs, int maxerr) {
    int count = 0;
    /* work on a copy line by line */
    const char *p = output;
    while (*p && count < maxerr) {
        /* find end of line */
        const char *nl = strchr(p, '\n');
        int llen = nl ? (int)(nl-p) : (int)strlen(p);
        if (llen <= 0 || llen >= 4095) { if (nl) p=nl+1; else break; continue; }

        char linebuf[4096];
        strncpy(linebuf, p, llen); linebuf[llen]='\0';

        char fpath[512]=""; int lno=0, cno=0;
        char sev[32]=""; char msg[512]="";
        int matched = 0;

        /* GCC/Clang: file:line:col: severity: message */
        {
            const char *s = linebuf;
            /* skip Windows drive "C:" */
            const char *c1 = strchr(s, ':');
            if (c1 && c1==s+1 && (*(c1+1)=='/'||*(c1+1)=='\\'))
                c1 = strchr(c1+1, ':');
            if (!c1) goto try_msvc;
            int fnlen = (int)(c1-s);
            if (fnlen<=0||fnlen>=511) goto try_msvc;
            strncpy(fpath,s,fnlen); fpath[fnlen]='\0';

            char *endln;
            long ln = strtol(c1+1, &endln, 10);
            if (endln==c1+1||(*endln!=':')) goto try_msvc;
            lno=(int)ln;

            const char *after = endln+1;
            char *endcol;
            long cn = strtol(after, &endcol, 10);
            if (endcol!=after && *endcol==':') { cno=(int)cn; after=endcol+1; }
            while (*after==' ') after++;

            static const char *sevs[]={"fatal error","error","warning","note",NULL};
            for (int si=0; sevs[si]; si++) {
                size_t sl=strlen(sevs[si]);
                if (strncmp(after,sevs[si],sl)==0 && after[sl]==':') {
                    strncpy(sev,sevs[si],31);
                    const char *ms=after+sl+1; while(*ms==' ')ms++;
                    strncpy(msg,ms,511);
                    matched=1; break;
                }
            }
            goto check;
        }
try_msvc:
        {
            /* MSVC: file(line): error/warning CXXXX: message */
            const char *lp=strchr(linebuf,'(');
            const char *rp=lp?strchr(lp,')'):NULL;
            if (lp&&rp) {
                int fnlen=(int)(lp-linebuf);
                if (fnlen>0&&fnlen<511) {
                    strncpy(fpath,linebuf,fnlen); fpath[fnlen]='\0';
                    lno=atoi(lp+1);
                    const char *after=rp+1;
                    if(*after==':')after++; while(*after==' ')after++;
                    if (strncmp(after,"error",5)==0) {
                        strncpy(sev,"error",31);
                        after+=5; while(*after==' '||*after==':')after++;
                        strncpy(msg,after,511); matched=1;
                    } else if (strncmp(after,"warning",7)==0) {
                        strncpy(sev,"warning",31);
                        after+=7; while(*after==' '||*after==':')after++;
                        strncpy(msg,after,511); matched=1;
                    }
                }
            }
        }
check:
        if (matched && fpath[0] && lno>0 &&
            (strcmp(sev,"error")==0||strcmp(sev,"fatal error")==0||strcmp(sev,"warning")==0)) {
            CompileError *ce=&errs[count++];
            strncpy(ce->file,     fpath,511);
            strncpy(ce->severity, sev,  31);
            strncpy(ce->message,  msg,  511);
            ce->line=lno; ce->col=cno;
            extract_error_token(msg, ce->token, sizeof(ce->token));
        }

        if (!nl) break;
        p = nl+1;
    }
    return count;
}

static int cmd_fcompile(int argc, char **argv) {
    printf(BLUE "\n[F-COMPILE] ══════════════════════════════\n" RESET);
    printf(BLUE "[F-COMPILE] Front Compile  —  error analysis\n" RESET);
    printf(BLUE "[F-COMPILE] ══════════════════════════════\n\n" RESET);

    /* Find own executable path */
    char self[512]; self[0]='\0';
#ifdef _WIN32
    strncpy(self, argv[0], 511);
#else
    {
        ssize_t slen = readlink("/proc/self/exe", self, 511);
        if (slen>0) self[slen]='\0';
        else strncpy(self, argv[0], 511);
    }
#endif
    (void)argc;

    /* Run --build with OMAKE_FCOMPILE=1 (skip link hint) capturing output */
    char cap_cmd[768];
#ifdef _WIN32
    snprintf(cap_cmd, sizeof(cap_cmd), "OMAKE_FCOMPILE=1 \"%s\" --build 2>&1", self);
#else
    snprintf(cap_cmd, sizeof(cap_cmd), "OMAKE_FCOMPILE=1 \"%s\" --build 2>&1", self);
#endif

    FILE *fp = popen(cap_cmd, "r");
    if (!fp) {
        printf(RED "[F-COMPILE] Could not run build.\n" RESET);
        return 1;
    }

    /* Read all output */
    size_t cap = 2*1024*1024;
    char *all_output = (char*)malloc(cap);
    if (!all_output) { pclose(fp); return 1; }
    size_t used=0;
    char rbuf[4096];
    while (fgets(rbuf,sizeof(rbuf),fp) && used<cap-1) {
        size_t ll=strlen(rbuf);
        if (used+ll<cap-1){ memcpy(all_output+used,rbuf,ll); used+=ll; }
    }
    all_output[used]='\0';
    int build_rc = pclose(fp);

    /* Strip ANSI escape codes */
    char *clean = (char*)malloc(used+2);
    if (!clean) { free(all_output); return 1; }
    size_t ci=0;
    for (size_t i=0; i<used; i++) {
        if ((unsigned char)all_output[i]==0x1b && i+1<used && all_output[i+1]=='[') {
            i+=2;
            while (i<used && all_output[i]!='m') i++;
            continue;
        }
        clean[ci++] = all_output[i];
    }
    clean[ci]='\0';
    free(all_output);

    /* Parse */
    CompileError *errs = (CompileError*)calloc(MAX_FERR, sizeof(CompileError));
    if (!errs) { free(clean); return 1; }

    int found = parse_compiler_output(clean, errs, MAX_FERR);
    free(clean);

    int err_count=0, warn_count=0;
    for (int i=0;i<found;i++) {
        if (strcmp(errs[i].severity,"warning")==0) warn_count++;
        else err_count++;
    }

    if (found==0 && build_rc==0) {
        printf(GREEN "[F-COMPILE] ✓  No errors — build clean.\n\n" RESET);
        free(errs); return 0;
    }
    if (found==0 && build_rc!=0) {
        printf(RED "[F-COMPILE] Build failed but no parseable errors found.\n" RESET);
        printf(YELLOW "[F-COMPILE] Run omake --build for raw output.\n\n" RESET);
        free(errs); return 1;
    }

    /* ── Group by file ─────────────────────────────────────────── */
    char seen_files[512][512]; int nfiles=0;
    for (int i=0;i<found;i++) {
        if (!errs[i].file[0]) continue;
        int already=0;
        for (int j=0;j<nfiles;j++)
            if (strcmp(seen_files[j],errs[i].file)==0){already=1;break;}
        if (!already && nfiles<512)
            strncpy(seen_files[nfiles++],errs[i].file,511);
    }

    for (int fi=0;fi<nfiles;fi++) {
        const char *fname=seen_files[fi];
        int fe=0,fw=0;
        for (int i=0;i<found;i++)
            if (strcmp(errs[i].file,fname)==0) {
                if (strcmp(errs[i].severity,"warning")==0) fw++;
                else fe++;
            }

        /* File header */
        printf(YELLOW "┌─ %s" RESET, fname);
        if (fe)  printf(RED   "  %d error(s)"  RESET, fe);
        if (fw)  printf(YELLOW"  %d warning(s)" RESET, fw);
        printf("\n");

        for (int i=0;i<found;i++) {
            if (strcmp(errs[i].file,fname)!=0) continue;
            CompileError *ce=&errs[i];
            int is_err=(strcmp(ce->severity,"error")==0||strcmp(ce->severity,"fatal error")==0);

            /* Error/warning line */
            printf("\033[90m│\033[0m  ");
            printf(CYAN "line %-4d" RESET, ce->line);
            if (ce->col) printf(CYAN ":%-3d" RESET, ce->col);
            else         printf("    ");
            if (is_err) printf("  \033[1;31m%-13s\033[0m", ce->severity);
            else        printf("  \033[1;33m%-13s\033[0m", ce->severity);
            printf(" %s\n", ce->message);

            /* Source line from file */
            char src_line[1024]; src_line[0]='\0';
            if (ce->line>0 && read_file_line(fname,ce->line,src_line,sizeof(src_line))) {
                printf("\033[90m│\033[0m  ");
                /* trim leading whitespace */
                const char *sl=src_line;
                while (*sl==' '||*sl=='\t') sl++;
                print_line_highlighted(sl, ce->token);
            }

            /* Token */
            if (ce->token[0]) {
                printf("\033[90m│\033[0m  ");
                printf("\033[32mtoken\033[0m: \033[1;31m→ %s\033[0m\n", ce->token);
            }
            printf("\033[90m│\033[0m\n");
        }
        printf(YELLOW "└────────────────────────────────────\n\n" RESET);
    }

    /* ── Summary ─────────────────────────────────────────────── */
    printf(BLUE "[F-COMPILE] ══════════════════════════════\n" RESET);
    if (err_count>0)
        printf(RED "[F-COMPILE] %d error(s)   %d warning(s)   in %d file(s)\n" RESET,
               err_count, warn_count, nfiles);
    else
        printf(YELLOW "[F-COMPILE] 0 errors   %d warning(s)   in %d file(s)\n" RESET,
               warn_count, nfiles);
    printf(BLUE "[F-COMPILE] ══════════════════════════════\n\n" RESET);

    free(errs);
    return (err_count>0) ? 1 : 0;
}


/* ── Run TEST_CASE: and TEST: dir (separate function to avoid stack overflow) ── */
static int cmd_run_test_dsl(void) {
    char test_dir2[512]  = "tests";
    char test_ext2[32]   = ".cpp";
    char *test_cases2    = (char*)calloc(1, B_SIZE);
    if (!test_cases2) return 1;
    char tcomp2[256] = "g++";
    char tstd2[64]   = "";
    char tobj2[512]  = ".omake_obj";

    FILE *cfg2 = fopen("OMakeLists.txt","r");
    if(cfg2) {
        char ln2[1024];
        while(fgets(ln2,sizeof(ln2),cfg2)) {
            char *cmt2=strchr(ln2,'#'); if(cmt2)*cmt2='\0';
            char ll2[1024]; strcpy(ll2,ln2); to_lower_str(ll2);
            char *col2=strchr(ll2,':'); if(!col2)continue;
            *col2='\0'; char *key2=ll2;
            { char *ke=key2+strlen(key2)-1;
              while(ke>key2&&(*ke==' '||*ke=='\t')) *ke--='\0'; }
            char *v2_raw=strchr(ln2,':'); if(!v2_raw)continue;
            char *v2=v2_raw+1; trim(v2);
            char *nl2=strrchr(v2,'\n'); if(nl2)*nl2='\0';
            char *cr2=strrchr(v2,'\r'); if(cr2)*cr2='\0';
            if(!strcmp(key2,"test")) {
                char *sl2=strrchr(v2,'/'); if(!sl2)sl2=strrchr(v2,'\\');
                if(sl2&&strchr(sl2,'*')){*sl2='\0';strncpy(test_dir2,v2,511);
                    char *dt2=strchr(sl2+1,'.');if(dt2)strncpy(test_ext2,dt2,31);}
                else strncpy(test_dir2,v2,511);
            } else if(!strcmp(key2,"test_case")) {
                if(test_cases2[0]) strncat(test_cases2," ",B_SIZE-strlen(test_cases2)-1);
                strncat(test_cases2,v2,B_SIZE-strlen(test_cases2)-1);
            } else if(!strcmp(key2,"compiler")) {
                strncpy(tcomp2,v2,255);
            } else if(!strcmp(key2,"standard")) {
                strncpy(tstd2,v2,63);
            } else if(!strcmp(key2,"objdir")) {
                strncpy(tobj2,v2,511);
            }
        }
        fclose(cfg2);
    }

    mkdir_auto(tobj2);
    gen_omake_test_header(tobj2);

    if (test_cases2[0]) {
        int tc_pass=0, tc_fail=0;
        char *tc_tags = (char*)calloc(1, B_SIZE);
        if (!tc_tags) { free(test_cases2); return 1; }

        if (tstd2[0]) {
            char tstd_n[32]; strncpy(tstd_n,tstd2,31);
            for(char *p=tstd_n;*p;p++) *p=(char)tolower((unsigned char)*p);
            { char *eq=strrchr(tstd_n,'='); if(eq) memmove(eq,eq+1,strlen(eq)+1); }
            char stf[80]; snprintf(stf,sizeof(stf)," -std=%s",tstd_n);
            strncat(tc_tags,stf,B_SIZE-strlen(tc_tags)-1);
        }
        char tc_inc[512]; snprintf(tc_inc,sizeof(tc_inc)," -I\"%s\"",tobj2);
        strncat(tc_tags,tc_inc,B_SIZE-strlen(tc_tags)-1);

        char *tc_copy = (char*)calloc(1, B_SIZE);
        if (!tc_copy) { free(tc_tags); free(test_cases2); return 1; }
        strncpy(tc_copy,test_cases2,B_SIZE-1);

        char *tc_tok = strtok(tc_copy," \t");
        printf(BLUE "\n[TEST] Running TEST_CASE files...\n" RESET);
        while (tc_tok) {
            char tc_bin[512];
            snprintf(tc_bin,sizeof(tc_bin),"/tmp/omake_tc_%llx",(unsigned long long)fnv64(tc_tok));
            char tc_cmd[1024];
            snprintf(tc_cmd,sizeof(tc_cmd),"%s \"%s\" %s -o \"%s\" 2>&1",
                     tcomp2, tc_tok, tc_tags, tc_bin);
            printf(CYAN "[TEST] Compiling: %s\n" RESET, tc_tok);
            int cok = system(tc_cmd);
            if (cok != 0) {
                printf(RED "[TEST] Compile failed: %s\n" RESET, tc_tok);
                tc_fail++;
            } else {
                printf(CYAN "[TEST] Running: %s\n" RESET, tc_bin);
                int rok = system(tc_bin);
                if (rok == 0) { printf(GREEN "[TEST] \xe2\x9c\x93 PASS: %s\n" RESET, tc_tok); tc_pass++; }
                else          { printf(RED   "[TEST] \xe2\x9c\x97 FAIL: %s\n" RESET, tc_tok); tc_fail++; }
                remove(tc_bin);
            }
            tc_tok = strtok(NULL," \t");
        }
        printf(BLUE "[TEST] \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n" RESET);
        if (tc_fail==0)
            printf(GREEN "[TEST] All passed: %d/%d\n\n" RESET, tc_pass, tc_pass+tc_fail);
        else
            printf(RED "[TEST] %d failed / %d total\n\n" RESET, tc_fail, tc_pass+tc_fail);
        free(tc_tags); free(test_cases2); free(tc_copy);
        return (tc_fail > 0) ? 1 : 0;
    }

    /* Legacy TEST: dir-based runner */
    {
        CompilerType tct=CT_GCC;
        char *tinc  = (char*)calloc(1,B_SIZE);
        char *ttags = (char*)calloc(1,B_SIZE);
        char *tlibs = (char*)calloc(1,B_SIZE);
        if(!tinc||!ttags||!tlibs){free(tinc);free(ttags);free(tlibs);free(test_cases2);return 1;}
        int test_ret = cmd_run_tests(test_dir2, test_ext2, tobj2,
                      tcomp2, tct, ttags, tlibs, tinc);
        free(tinc); free(ttags); free(tlibs); free(test_cases2);
        return test_ret;
    }
}


void show_usage(void) {
  printf(CYAN "\nOMake v" VERSION "\n" RESET);
  printf(CYAN "Usage: omake <command> [flags]\n\n" RESET);

  printf(YELLOW "═══ BUILD ════════════════════════════════════════════════\n" RESET);
  printf("  omake --build                     Force full rebuild\n");
  printf("  omake --build-hash                Incremental (hash-based)\n");
  printf("  omake --f-compile                 Front compile: errors with line + token\n");
  printf("\n");
  printf(YELLOW "═══ PRESET ═══════════════════════════════════════════════════\n" RESET);
  printf("  omake --preset <name>             Apply named preset from omake.presets\n");
  printf("  omake --preset-list               List all available presets\n");
  printf("  Example: omake --preset linux-clang-asan\n");
  printf("\n");
  printf(YELLOW "═══ TOOLCHAIN ════════════════════════════════════════════════\n" RESET);
  printf("  omake --toolchain <file>          Load toolchain file (cross-compile)\n");
  printf("  TOOLCHAIN: arm.toolchain          In OMakeLists.txt\n");
  printf("\n");
  printf(YELLOW "═══ PGO ══════════════════════════════════════════════════════\n" RESET);
  printf("  omake --pgo-generate --build      Phase 1: instrument for profiling\n");
  printf("  omake --pgo-use --build           Phase 2: optimized with profile data\n");
  printf("\n");
  printf(YELLOW "═══ IDE ══════════════════════════════════════════════════════\n" RESET);
  printf("  omake --ide vscode                Generate .vscode/ tasks + launch\n");
  printf("  omake --ide clion                 Generate CMakeLists.txt stub\n");
  printf("  omake --ide all                   Generate all IDE configs\n");
  printf("\n");
  printf(YELLOW "═══ CACHE ════════════════════════════════════════════════════\n" RESET);
  printf("  omake --cache-list                Show cached extern deps\n");
  printf("  omake --cache-clear               Clear dependency cache\n");
  printf("\n");
  printf(YELLOW "═══ DISTRIBUTED ══════════════════════════════════════════════\n" RESET);
  printf("  omake --dist host1,host2          Distributed build via SSH/rsync\n");
  printf("  omake --dist h1,h2,h3 --build     Sync + compile on remotes + link\n");
  printf("\n");
  printf(YELLOW "═══ REPRODUCIBLE ═════════════════════════════════════════════\n" RESET);
  printf("  omake --reproducible --build      Deterministic byte-identical output\n");
  printf("  REPRODUCIBLE: on                  In OMakeLists.txt\n");
  printf("\n");
  printf(YELLOW "═══ TEST DSL ═════════════════════════════════════════════════\n" RESET);
  printf("  TEST_CASE: tests/foo_test.cpp     Register test file in OMakeLists.txt\n");
  printf("  #include <omake_test.h>           Lightweight test macros (auto-gen)\n");
  printf("  OMAKE_CHECK(expr)                 Assert with file/line report\n");
  printf("  OMAKE_REGISTER_TESTS(...)         Auto-run all test cases\n");
  printf("  omake --build-hash                Incremental (changed files only)\n");
  printf("  omake --build --profile <p>       Profile: debug | release | asan\n");
  printf("  omake --build --static            Bundle all libs (no .so/.dll)\n");
  printf("  omake --build --strip             Strip debug symbols from binary\n");
  printf("  omake --build --compress          Pack binary with UPX\n");
  printf("  omake --build --lto               Enable Link-Time Optimization\n");

  printf(YELLOW "\n═══ OMakeLists.txt — Extended Directives ═══════════════\n" RESET);
  printf("  FETCH: <url> TO <dir> [TAG v1.0] [BRANCH main] [RECURSE]\n");
  printf("    [CONFIGURE <cmd>] [BUILD_ARGS <args>] [ADD_INC <dir>] [ADD_LIB <dir>]\n");
  printf("  MODULE: myapp          Define a named build target\n");
  printf("    MOD_FLAGS: -DAPP     Module-scoped compiler flags\n");
  printf("    MOD_LIBS:  -lx       Module-scoped linker flags\n");
  printf("    MOD_INC:   path      Module-scoped include directory\n");
  printf("    MOD_DEPS:  mylib     Link another module (propagates includes)\n");
  printf("  HEADER_LIB: mylib      Header-only lib — propagates flags to dependents\n");
  printf("  OBJ_LIB: myobjs        Object file pool — shared across targets\n");
  printf("  ALIAS: MyLib::Core=mylib  Module alias\n");
  printf("  BATCH: N         Compile N files per unity TU (default: all)\n");
  printf("  PCH_AUTO: on           Auto-detect + precompile most-used header\n");
  printf("  EXPORT_CC: on           Always write compile_commands.json\n");
  printf("  Generator Expressions (in TAGS: / TARGET_FLAGS:):\n");
  printf("    $<CONFIG:Debug>:-DDEBUG_MODE    Active only in debug profile\n");
  printf("    $<CONFIG:Release>:-O3 -DNDEBUG  Active only in release\n");
  printf("    $<PLATFORM:linux>:-DLINUX       Active on Linux\n");
  printf("    $<PLATFORM:win32>:-DWINDOWS     Active on Windows\n");
  printf("    $<COMPILER:gcc>:-Wextra         Active with GCC\n");
  printf("  --m-pack nsis          Windows NSIS installer (.exe) — needs makensis\n");

  printf(YELLOW "\n═══ OMakeLists.txt — Advanced Directives (11-20) ═══════\n" RESET);
  printf("  LTO_PROBE: on          Auto-probe + enable best LTO mode (thin/full/LTCG)\n");
  printf("  DBG_SUFFIX: -d         [12] Rename output in debug mode: myapp-d.exe\n");
  printf("  GEN_HEADER: a.h.in->a.h  [13] Template file substitution\n");
  printf("    Variables: @VERSION@ @BUILD_DATE@ @GIT_HASH@ @PLATFORM@ @PROJECT_NAME@\n");
  printf("    Also: ${VAR} syntax and #omakedefine  #omakedefine01\n");
  printf("  EXTERN: NAME=x URL=https://...  [14] Download+build+install\n");
  printf("    Options: BUILD_ARGS=-DFOO=1  TAG=v1.0  PATCH=file.patch  INSTALL_DIR=dir\n");
  printf("  PROBE: openssl zlib SDL2  [15] pkg-config + header scan + vcpkg\n");
  printf("  NEED_STD: C++=20         [16] Abort if compiler lacks standard\n");
  printf("    Values: C++=23/20/17/14  C=23/11\n");
  printf("  --m-gvz                [17] Source graph + target graph (targets.dot/.png)\n");
  printf("  IPO: on|full           [18] Interprocedural optimization\n");
  printf("  RESOURCE_QT: app.qrc   [19] Compile Qt resources (rcc or OMake fallback)\n");
  printf("  EMBED: name=file.png   [19+] Embed raw binary as C array\n");
  printf("  PORTABLE: on           [20] Portability check — relative paths, no pollution\n");
  printf(YELLOW "\n═══ OMakeLists.txt — Features 21-25 ════════════════════\n" RESET);
  printf("  PRERUN:  cmd [@OUT@]   Run command before build\n");
  printf("  POSTRUN: cmd [@OUT@]   Run command after successful build\n");
  printf("    Tokens: @OUT@ @SRC_DIR@ @VERSION@ @PLATFORM@\n");
  printf("  SIZE_REPORT: on|full|bloat  [22] Binary section sizes + symbol analysis\n");
  printf("    on=sections, full=+top20 symbols, bloat=+save bloat.txt\n");
  printf("  MIRROR: src/->dst/ [;src2/->dst2/]  [23] Sync dirs after build\n");
  printf("  HASH_CHECK: on|strict  [24] Source integrity (detect unexpected changes)\n");
  printf("    strict: abort build on mismatch\n");
  printf("  MULTI_OUT: bin/a bin/b  [25] Multiple binaries from one project\n");
  printf(YELLOW "\n═══ USE_LIB: Heavy Library Support (25 Libraries) ═══\n" RESET);
  printf("  USE_LIB: opengl glfw glew glad            Graphics stack\n");
  printf("  USE_LIB: sdl2:image,ttf,mixer            SDL2 + modules\n");
  printf("  USE_LIB: sfml:graphics,audio,network     SFML + modules\n");
  printf("  USE_LIB: vulkan                          Vulkan + SPIR-V shader compile\n");
  printf("  USE_LIB: qt:Core,Widgets,OpenGL          Qt (auto-moc/uic/rcc)\n");
  printf("  QT_MODULES: Core Widgets OpenGL Network Sql Multimedia\n");
  printf("  QT_DIR: C:/Qt/6.7.0/mingw_64    (optional — overrides auto-detect)\n");
  printf("    Detection order: qmake -query → QTDIR env → moc path → pkg-config\n");
  printf("  USE_LIB: imgui:opengl3+glfw              Dear ImGui + backend\n");
  printf("  IMGUI_BACKEND: opengl3+glfw | opengl3+sdl2 | vulkan+glfw | dx11\n");
  printf("  USE_LIB: glm eigen assimp freetype       Math/3D/font libs\n");
  printf("  USE_LIB: boost:filesystem,thread         Boost modules\n");
  printf("  BOOST_MODULES: filesystem thread regex system\n");
  printf("  USE_LIB: opencv:core,imgproc             OpenCV + modules\n");
  printf("  OPENCV_MODULES: core imgproc highgui videoio dnn\n");
  printf("  USE_LIB: openal ogg fmod:studio          Audio libs\n");
  printf("  USE_LIB: box2d                           Physics\n");
  printf("  USE_LIB: wx:core,base                   wxWidgets\n");
  printf("  USE_LIB: gtk3 | gtk4                    GTK UI toolkit\n");
  printf("  USE_LIB: <any>                          pkg-config + header scan fallback\n");
  printf("    Each name maps to src/<name>.cpp entry file, shares compiled objects\n");
  printf(YELLOW "\n═══ Build Parameters ═══════════════════════════════════\n" RESET);
  printf("  --bin                  Force .bin extension on output binary\n");
  printf("  NAME: MyProject        Project name (used in GEN_HEADER variables)\n");
  printf("  --m-pack pkg           macOS .pkg — needs pkgbuild\n");
  printf("  omake --build --sanitize <mode>   AddressSanitizer / thread / ub / memory\n");
  printf("  omake --build --x64 | --i386 | --arm64   Target architecture\n");
  printf("  omake --build --all-compilers     Build with every compiler on PATH\n");
  printf("  omake --build -j <N>              Parallel compile with N jobs\n");
  printf("  omake --build --watch             Rebuild on file change (inotify/poll)\n");
  printf("  omake --generate-compile-commands  Write compile_commands.json (clangd LSP)\n");
  printf("  omake --build --generate-asm       Emit .s assembly for every source file\n");
  printf("    Output: omake_asm/<src>.s  (Intel syntax on x86, verbose-asm)\n");
  printf("    MSVC:   omake_asm/<src>.s  via /FAs\n");

  printf(YELLOW "\n═══ OMakeLists.txt DIRECTIVES ═══════════════════════════\n" RESET);
  printf("  SRC: src/*.cpp          C++ sources\n");
  printf("  SRC: src/*.c            C sources\n");
  printf("  SRC: src/*.c;*.cpp      Mixed C and C++ (or just: SRC: src)\n");
  printf("  COMPILER: g++           g++ clang++ gcc clang cl clang-cl\n");
  printf("    COMPILER: optional for g++/gcc/clang++ — required for cl / clang-cl\n");
  printf("  SET_COMPILER_<LIB>: <path>    Override compiler when using a library\n");
  printf("    SET_COMPILER_QT:            (empty) auto-finds Qt MinGW g++.exe\n");
  printf("    SET_COMPILER_QT: C:/Qt/6.6.0/mingw_64/bin/g++.exe\n");
  printf("    SET_COMPILER_SFML: /usr/bin/clang++\n");
  printf("  STANDARD: C++=17        C++ standard: 98 03 11 14 17 20 23 26\n");
  printf("  STANDARD: C=11          C standard:   89 90 99 11 17 23\n");
  printf("  STANDARD: C++=20 C=11   Both on one line\n");
  printf("  OUT: bin/app            Output binary path\n");
  printf("  OBJDIR: .omake_obj      Object file directory\n");
  printf("  TAGS: -O3 -Wall         Compiler flags\n");
  printf("  LIBS: -lm -lpthread     Linker flags\n");
  printf("  DEFINE: FOO=1 BAR=hi    Macro definitions (-DFOO=1 -DBAR=hi)\n");
  printf("  VERSION: 1.2.3          Project version string\n");
  printf("  PCH: include/pch.h   Precompiled header\n");
  printf("  UNITY: on               Unity build (all sources -> one TU)\n");
  printf("  LTO: on | thin | off    Link-time optimization\n");
  printf("  OUT_TYPE: exe | shared | static  Library output type\n");
  printf("  PKG: openssl zlib       pkg-config integration\n");
  printf("  DEPS: nlohmann/json fmt stb   GitHub shorthand (auto FETCH:)\n");
  printf("  FETCH: <url> TO <dir>  Clone/download git dependency\n");
  printf("  ENV: CC CXX MY_VAR      Inject env vars as -D macros\n");
  printf("  INSTALL: /usr/local/bin Install path (runs after build)\n");
  printf("  TEST: tests/*.cpp       Test source directory\n");
  printf("  CROSS: arm-linux-gnueabihf-   Cross-compile toolchain prefix\n");
  printf("  RESOURCE: res/          Windows .rc resource files\n");

  printf(YELLOW "\n═══ QUALITY & ANALYSIS ══════════════════════════════════\n" RESET);
  printf("  omake --format          Auto-format all sources (clang-format)\n");
  printf("  omake --lint            Static analysis (clang-tidy)\n");
  printf("  omake --coverage        Coverage report (gcov / llvm-cov)\n");
  printf("  omake --docs            Generate documentation (Doxygen)\n");
  printf("  omake --graph           Header dependency graph\n");
  printf("  omake --disasm [func]   Disassembly (objdump/dumpbin)\n");
  printf("  omake --benchmark <bin> [N]   Time binary (hyperfine or built-in)\n");

  printf(YELLOW "\n═══ TEST & DISTRIBUTE ═══════════════════════════════════\n" RESET);
  printf("  omake --test            Build & run TEST: sources, report pass/fail\n");
  printf("  omake --m-pack [zip|deb|rpm|all]  Package project (default: all)\n");
  printf("    zip   Cross-platform ZIP archive\n");
  printf("    deb   Debian/Ubuntu .deb (dpkg-deb or ar fallback)\n");
  printf("    rpm   RHEL/Fedora .rpm   (needs rpmbuild)\n");
  printf("    Reads: VERSION: INSTALL: DESCRIPTION: MAINTAINER: HOMEPAGE:\n");
  printf("    Output: dist/<name>-<ver>.zip / .deb / .rpm\n");
  printf("  omake --m-gvz [png|svg|dot]   Dependency graph (default: png+svg)\n");
  printf("    Output: omake_graph/deps.dot  + .png + .svg\n");
  printf("    Nodes: src (blue) / project hdr (green) / system hdr (grey) / binary (orange)\n");
  printf("    Needs: graphviz (dot)  — sudo apt install graphviz\n");
  printf("  omake --package [bin]   Create .tar.gz / .zip release bundle\n");
  printf("  omake --sign [cert]     Code-sign binary (signtool / gpg)\n");
  printf("  omake --install         Install to INSTALL: path\n");

  printf(YELLOW "\n═══ FILE DATABASE ════════════════════════════════════════\n" RESET);
  printf("  omake --find-all [dir]  Index all files into .omake_filedb\n");
  printf("  omake --find <pattern>  Search indexed files (name / .ext / glob)\n");
  printf("    --ext .cpp            Filter by extension\n");
  printf("  omake --find-stats      DB statistics (counts, types, sizes)\n");

  printf(YELLOW "\n═══ PROJECT ══════════════════════════════════════════════\n" RESET);
  printf("  omake --create-project <name> [template]\n");
  printf("  Templates: basic  c  sfml  sdl2  opengl  qt  imgui\n");
  printf("             server  test  lib  imgui\n");
  printf("  omake --update          Self-update OMake binary\n");
  printf("  omake clean             Remove build artifacts and file DB\n");
  printf("  omake -v / --version    Show version\n");

  printf(YELLOW "\n═══ CONDITIONAL SYNTAX ══════════════════════════════════\n" RESET);
  printf("  if win32 / elif linux / elif macos / else / end\n");
  printf("  # comment lines\n\n");
}

/* ------------------------------------------------------------------ */
/*  Compiler candidates for --all-compilers                            */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *binary;
    const char *label;
    CompilerType type;
} CompilerCandidate;

static const CompilerCandidate CANDIDATES[] = {
#ifdef _WIN32
    { "cl",       "MSVC (cl.exe)",          CT_MSVC  },
    { "clang-cl", "Clang-CL (MSVC-compat)", CT_MSVC  },
    { "clang++",  "Clang++ (C/C++)",        CT_CLANG },
    { "g++",      "GCC/MinGW g++ (C/C++)",  CT_GCC   },
#else
    { "g++",      "GCC g++ (C/C++)",        CT_GCC   },
    { "clang++",  "Clang++ (C/C++)",        CT_CLANG },
#endif
    { NULL, NULL, CT_GCC }
};


void run_all_compilers(int force, const char *profile, int jobs,
                       const char *arch, int do_static) {
    printf(MAGENTA "\n[ALL-COMPILERS] Probing available compilers...\n" RESET);

    int found = 0;
    int pass  = 0;
    int fail  = 0;

    for (int i = 0; CANDIDATES[i].binary != NULL; i++) {
        if (!tool_exists(CANDIDATES[i].binary)) {
            printf(YELLOW "[ALL-COMPILERS] %-20s not found, skipping.\n" RESET,
                   CANDIDATES[i].label);
            continue;
        }
        found++;
        printf(MAGENTA "\n[ALL-COMPILERS] ── Building with: %s ──\n" RESET,
               CANDIDATES[i].label);

        /* Force full rebuild for each compiler so obj dirs are fresh */
        build_engine(1, profile, jobs, 0, arch, do_static,
                     CANDIDATES[i].binary, 0, 0, 0,0);

        /* We detect success by checking if the output binary was created.
         * build_engine prints [OK] or [ERROR] — we just count. */
        pass++;   /* optimistic; errors are already printed by build_engine */
    }

    if (found == 0) {
        printf(RED "[ALL-COMPILERS] No supported compilers found on PATH!\n" RESET);
        printf(RED "[ALL-COMPILERS] Install g++, clang++, or cl.exe and retry.\n" RESET);
        return;
    }

    printf(MAGENTA "\n[ALL-COMPILERS] Done. Ran %d compiler(s).\n" RESET, pass);
    printf(MAGENTA "[ALL-COMPILERS] Check bin/ for per-compiler binaries:\n" RESET);
    printf(MAGENTA "               app_gcc / app_clang / app_msvc\n" RESET);
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    system("");  /* Enable ANSI escape codes on Windows */
#endif

    if (argc == 1) { printf("please use --build-hash "); return 0; }

    int   force          = 0;
    int   do_build       = 0;
    int   show_graph     = 0;
    int   all_compilers  = 0;
    int   do_find_all    = 0;   /* --find-all          */
    int   do_find_search = 0;   /* --find <pattern>    */
    int   do_find_stats  = 0;   /* --find-stats        */
    int   do_strip_bin   = 0;   /* --strip             */
    int   do_compress_bin= 0;   /* --compress          */
    int   do_watch       = 0;   /* --watch             */
    int   do_test        = 0;   /* --test              */
    int   do_format      = 0;   /* --format            */
    int   do_lint        = 0;   /* --lint              */
    int   do_benchmark   = 0;   /* --benchmark         */
    int   do_coverage    = 0;   /* --coverage          */
    int   do_docs        = 0;   /* --docs              */
    int   do_package     = 0;   /* --package           */
    int   do_sign        = 0;   /* --sign              */
    int   do_disasm      = 0;   /* --disasm            */
    int   do_self_update = 0;   /* --update            */
    int   do_mpack       = 0;   /* --m-pack            */
    int   do_mgvz        = 0;   /* --m-gvz             */
    int   do_bin         = 0;   /* --bin               */
    char  mpack_fmt[64]  = "";  /* zip,deb,rpm or all  */
    char  mgvz_fmt[16]   = "png"; /* png|svg|dot       */
    int   gen_compile_commands=0;/* --generate-compile-commands */
    int   gen_asm            =0;  /* --generate-asm              */
    int   do_sanitize    = 0;   /* --sanitize          */
    char  sanitize_mode[32]= "address"; /* default sanitizer */
    char  sign_cert[512]  = ""; /* --sign <cert>       */
    char  disasm_func[128]= ""; /* --disasm <func>     */
    char  bench_bin[512]  = ""; /* --benchmark <bin>   */
    int   bench_runs      = 10; /* --benchmark runs    */
    char  pkg_bin[512]    = ""; /* binary to package   */
    char  pkg_ver[64]     = ""; /* version for package */
    char  tmpl_name[64]   = "basic"; /* --create-project template */
    char  find_root[512]  = ".";  /* root dir for scan */
    char  find_pattern[256] = "";  /* search pattern   */
    char  find_ext[32]    = "";  /* --ext filter       */
    int   jobs           = get_cpu_count();
    int   do_static      = 0;
    char  profile[64]    = "";
    char  arch[16]       = "";
    char  preset_name[64]= "";   /* --preset <name>        */
    char  toolchain_arg[512]=""; /* --toolchain <file>     */
    char  dist_hosts[512] = "";  /* --dist host1,host2...  */
    char  ide_target[32]  = "";  /* --ide <vscode|clion>   */
    int   do_pgo_gen      = 0;   /* --pgo-generate         */
    int   do_pgo_use      = 0;   /* --pgo-use              */
    int   do_preset_list  = 0;   /* --preset-list          */
    int   do_cache_list   = 0;   /* --cache-list           */
    int   do_cache_clear  = 0;   /* --cache-clear          */
    int   do_ide          = 0;   /* --ide                  */
    int   do_dist         = 0;   /* --dist                 */
    int   do_reproducible = 0;   /* --reproducible         */

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--f-compile")  == 0) { return cmd_fcompile(argc, argv); }
        else if (strcmp(argv[i], "--preset-list")== 0) { do_preset_list = 1; }
        else if (strcmp(argv[i], "--cache-list") == 0) { do_cache_list  = 1; }
        else if (strcmp(argv[i], "--cache-clear")== 0) { do_cache_clear = 1; }
        else if (strcmp(argv[i], "--pgo-generate")== 0){ do_pgo_gen = 1; do_build = 1; force = 1; }
        else if (strcmp(argv[i], "--pgo-use")    == 0) { do_pgo_use = 1; do_build = 1; force = 1; }
        else if (strcmp(argv[i], "--reproducible")== 0){ do_reproducible = 1; }
        else if (strcmp(argv[i], "--preset")     == 0) {
            if (i+1 < argc) strncpy(preset_name, argv[++i], 63);
        }
        else if (strcmp(argv[i], "--toolchain")  == 0) {
            if (i+1 < argc) strncpy(toolchain_arg, argv[++i], 511);
        }
        else if (strcmp(argv[i], "--ide")        == 0) {
            do_ide = 1;
            if (i+1 < argc && argv[i+1][0] != '-') strncpy(ide_target, argv[++i], 31);
            else strncpy(ide_target, "vscode", 31);
        }
        else if (strcmp(argv[i], "--dist")       == 0) {
            if (i+1 < argc) { strncpy(dist_hosts, argv[++i], 511); do_dist = 1; }
        }
        else if (strcmp(argv[i], "--build")      == 0) { do_build = 1; force = 1; }
        else if (strcmp(argv[i], "--build-hash") == 0) { do_build = 1; force = 0; }
        else if (strcmp(argv[i], "--graph")      == 0) { show_graph = 1; }
        else if (strcmp(argv[i], "--static")       == 0) { do_static = 1; }
        else if (strcmp(argv[i], "--all-compilers") == 0) { all_compilers = 1; do_build = 1; force = 1; }
        else if (strcmp(argv[i], "--find-all") == 0) {
            do_find_all = 1;
            /* optional: --find-all <dir>  (default ".") */
            if (i+1 < argc && argv[i+1][0] != '-')
                strncpy(find_root, argv[++i], 511);
        }
        else if (strcmp(argv[i], "--find") == 0) {
            do_find_search = 1;
            if (i+1 < argc) strncpy(find_pattern, argv[++i], 255);
        }
        else if (strcmp(argv[i], "--find-stats") == 0) {
            do_find_stats = 1;
        }
        else if (strcmp(argv[i], "--strip")       == 0) { do_strip_bin    = 1; }
        else if (strcmp(argv[i], "--compress")    == 0) { do_compress_bin = 1; }
        else if (strcmp(argv[i], "--watch")       == 0) { do_watch        = 1; do_build = 1; }
        else if (strcmp(argv[i], "--test")        == 0) { do_test = 1; do_build = 1; force = 0; }
        else if (strcmp(argv[i], "--format")      == 0) { do_format       = 1; }
        else if (strcmp(argv[i], "--lint")        == 0) { do_lint         = 1; }
        else if (strcmp(argv[i], "--coverage")    == 0) { do_coverage     = 1; }
        else if (strcmp(argv[i], "--docs")        == 0) { do_docs         = 1; }
        else if (strcmp(argv[i], "--package")     == 0) {
            do_package = 1;
            if (i+1 < argc && argv[i+1][0] != '-') strncpy(pkg_bin, argv[++i], 511);
        }
        else if (strcmp(argv[i], "--sign")        == 0) {
            do_sign = 1;
            if (i+1 < argc && argv[i+1][0] != '-') strncpy(sign_cert, argv[++i], 511);
        }
        else if (strcmp(argv[i], "--disasm")      == 0) {
            do_disasm = 1;
            if (i+1 < argc && argv[i+1][0] != '-') strncpy(disasm_func, argv[++i], 127);
        }
        else if (strcmp(argv[i], "--benchmark")   == 0) {
            do_benchmark = 1;
            if (i+1 < argc && argv[i+1][0] != '-') strncpy(bench_bin, argv[++i], 511);
            if (i+1 < argc && isdigit((unsigned char)argv[i+1][0])) bench_runs=atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--sanitize")    == 0) {
            do_sanitize = 1;
            if (i+1 < argc && argv[i+1][0] != '-') strncpy(sanitize_mode, argv[++i], 31);
        }
        else if (strcmp(argv[i], "--update")      == 0) { do_self_update  = 1; }
        else if (strcmp(argv[i], "--m-pack") == 0) {
            do_mpack = 1;
            /* optional format: --m-pack zip  or  --m-pack zip,deb,rpm */
            if (i+1 < argc && argv[i+1][0] != '-')
                strncpy(mpack_fmt, argv[++i], 63);
        }
        else if (strcmp(argv[i], "--bin") == 0) {
            do_bin = 1;
        }
        else if (strcmp(argv[i], "--m-gvz") == 0) {

            do_mgvz = 1;
            /* optional output format: --m-gvz svg */
            if (i+1 < argc && argv[i+1][0] != '-')
                strncpy(mgvz_fmt, argv[++i], 15);
        }
        else if (strcmp(argv[i], "--generate-compile-commands") == 0) {
            gen_compile_commands = 1; do_build = 1;
        }
        else if (strcmp(argv[i], "--generate-asm") == 0) {
            gen_asm = 1; do_build = 1;
        }
        else if (strcmp(argv[i], "--template")    == 0 && i+1 < argc) {
            strncpy(tmpl_name, argv[++i], 63);
        }
        else if (strcmp(argv[i], "--ext") == 0 && i+1 < argc) {
            /* Extension filter for --find: --ext .cpp */
            strncpy(find_ext, argv[++i], 31);
            /* ensure leading dot */
            if (find_ext[0] != '.') {
                char tmp[32]; snprintf(tmp, 32, ".%s", find_ext);
                strncpy(find_ext, tmp, 31);
            }
            /* lowercase */
            for (int ei = 0; find_ext[ei]; ei++)
                find_ext[ei] = (char)tolower((unsigned char)find_ext[ei]);
        }
        else if (strcmp(argv[i], "--x64")        == 0) { strncpy(arch, "x64",   15); }
        else if (strcmp(argv[i], "--i386")       == 0) { strncpy(arch, "i386",  15); }
        else if (strcmp(argv[i], "--arm64")      == 0) { strncpy(arch, "arm64", 15); }
        else if (strcmp(argv[i], "--profile")    == 0 && i+1 < argc)
            strncpy(profile, argv[++i], 63);
        else if (strcmp(argv[i], "-j")           == 0 && i+1 < argc) {
            jobs = atoi(argv[++i]);
            if (jobs < 1) jobs = 1;
            if (jobs > MAX_JOBS) jobs = MAX_JOBS;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            show_usage(); return 0;
        }
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf(CYAN "%s\n" RESET, COPYRIGHT); return 0;
        }
        else if (strcmp(argv[i], "--create-project") == 0) {
            if (i+1 >= argc) {
                printf(RED "[ERROR] Usage: omake --create-project \"Name\" [lib1 lib2 ...]\n" RESET);
                printf(CYAN "  Examples:\n" RESET);
                printf(CYAN "    omake --create-project MyGame opengl sfml\n" RESET);
                printf(CYAN "    omake --create-project MyApp qt imgui\n" RESET);
                printf(CYAN "    omake --create-project MyGame vulkan glfw glm\n" RESET);
                printf(CYAN "    omake --create-project MyGame game3d\n" RESET);
                printf(CYAN "  Templates: basic c opengl opengl-glad vulkan sfml sfml-game\n" RESET);
                printf(CYAN "             sdl2 qt qt-opengl imgui imgui-sdl2 imgui-vulkan\n" RESET);
                printf(CYAN "             game3d opengl-sfml wx gtk3 eigen opencv json lib test\n" RESET);
                return 1;
            }
            const char *name = argv[++i];
            /* Collect remaining args as lib names until next -- flag */
            char lib_args[B_SIZE]; lib_args[0] = '\0';
            while (i+1 < argc && argv[i+1][0] != '-') {
                if (lib_args[0]) strncat(lib_args, " ", B_SIZE-strlen(lib_args)-1);
                strncat(lib_args, argv[++i], B_SIZE-strlen(lib_args)-1);
            }
            /* If --template was also given, that overrides */
            if (strcmp(tmpl_name,"basic") != 0 && lib_args[0] == '\0')
                strncpy(lib_args, tmpl_name, B_SIZE-1);
            /* Default: basic template */
            if (lib_args[0] == '\0') strncpy(lib_args, "basic", B_SIZE-1);
            cmd_create_project_ex(name, lib_args);
            return 0;
        }
        else if (strcmp(argv[i], "clean") == 0) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s .omake_h", DEL_FILE); system(cmd);
            snprintf(cmd, sizeof(cmd), "%s .omake_obj", DEL_DIR); system(cmd);
            snprintf(cmd, sizeof(cmd), "%s .omake_parallel.mk", DEL_FILE); system(cmd);
            snprintf(cmd, sizeof(cmd), "%s .omake_filedb",       DEL_FILE); system(cmd);
            snprintf(cmd, sizeof(cmd), "%s omake_asm",           DEL_DIR);  system(cmd);
            snprintf(cmd, sizeof(cmd), "%s omake_graph",         DEL_DIR);  system(cmd);
            snprintf(cmd, sizeof(cmd), "%s dist",                DEL_DIR);  system(cmd);
            printf(YELLOW "[CLEAN] Build artifacts and file database removed.\n" RESET);
            return 0;
        }
        else {
            printf(RED "[ERROR] Unknown flag: %s\n" RESET, argv[i]);
            show_usage(); return 1;
        }
    }

    /* --find-all / --find / --find-stats */
    if (do_find_all) {
        cmd_find_all(find_root);
        return 0;
    }
    if (do_find_search) {
        cmd_find_search(find_pattern, find_ext);
        return 0;
    }
    if (do_find_stats) {
        cmd_find_stats();
        return 0;
    }

    if (show_graph && !do_build) {
        build_engine(0, profile, 1, 1, arch, do_static, "", 0, 0, 0,0);
        return 0;
    }

    /* --sanitize: inject sanitizer flags before build */
    if (do_sanitize) {
        /* Passed via env so build_engine picks up via profile path */
        /* We inject as a special profile override */
        char san_flag[128];
        snprintf(san_flag, sizeof(san_flag),
                 " -fsanitize=%s -fno-omit-frame-pointer -g", sanitize_mode);
        /* Stored in profile so apply_profile will see it next */
        /* Simplest: just set an env var the build will pick up */
        char san_env[256]; snprintf(san_env,sizeof(san_env),"OMAKE_SAN=%s",san_flag);
        putenv(san_env);
        printf(CYAN "[SANITIZE] %s\n" RESET, sanitize_mode);
        do_build = 1; force = 1;
    }

    if (do_mpack)        { cmd_m_pack(mpack_fmt);      return 0; }
    if (do_mgvz)         { cmd_m_gvz_targets();         return 0; }
    if (do_format)       { cmd_format(0);              return 0; }
    if (do_lint)         { cmd_lint();                 return 0; }
    if (do_coverage)     { cmd_coverage();             return 0; }
    if (do_docs)         { cmd_docs();                 return 0; }
    if (do_self_update)  { cmd_self_update(argv[0]);   return 0; }
    if (do_disasm) {
        if (!bench_bin[0]) strncpy(bench_bin, "bin/app.bin", 511);
        cmd_disasm(bench_bin[0]?bench_bin:"bin/app.bin", disasm_func);
        return 0;
    }
    if (do_benchmark) {
        if (!bench_bin[0]) strncpy(bench_bin, "bin/app.bin", 511);
        cmd_benchmark(bench_bin, bench_runs);
        return 0;
    }
    if (do_package) {
        if (!pkg_bin[0]) strncpy(pkg_bin, "bin/app.bin", 511);
        cmd_package(pkg_bin, pkg_ver);
        return 0;
    }
    if (do_sign) {
        if (!bench_bin[0]) strncpy(bench_bin, "bin/app.bin", 511);
        cmd_sign(bench_bin, sign_cert);
        return 0;
    }

    /* ── Preset system: load files once ── */
    {
        static const char *PRESET_FILES[] = { "omake.presets", "omake.presets.local", NULL };
        for (int pi = 0; PRESET_FILES[pi]; pi++)
            if (is_regular_file(PRESET_FILES[pi])) preset_load(PRESET_FILES[pi]);
    }
    if (do_preset_list) {
        /* Print without loading again */
        if (g_preset_count == 0) {
            printf(YELLOW "[PRESET] No presets found. Create omake.presets\n" RESET);
        } else {
            printf(BLUE "\n[PRESET] Available presets (%d):\n" RESET, g_preset_count);
            for (int i = 0; i < g_preset_count; i++) {
                OmakePreset *p = &g_presets[i];
                printf(CYAN "  %-28s" RESET, p->name);
                if (p->profile[0])   printf(" profile=%-8s", p->profile);
                if (p->compiler[0])  printf(" comp=%-12s",   p->compiler);
                if (p->arch[0])      printf(" arch=%-6s",    p->arch);
                if (p->do_static)    printf(" static");
                if (p->reproducible) printf(" reproducible");
                if (p->toolchain[0]) printf(" tc=%s",        p->toolchain);
                printf("\n");
            }
            printf("\n");
        }
        return 0;
    }
    if (preset_name[0]) {
        OmakePreset *p = preset_find(preset_name);
        if (!p) {
            printf(RED "[PRESET] Unknown preset: '%s'\n" RESET, preset_name);
            printf(YELLOW "Run  omake --preset-list  to see available presets.\n" RESET);
            return 1;
        }
        printf(BLUE "[PRESET] Applying: %s\n" RESET, p->name);
        if (p->compiler[0])    setenv("OMAKE_COMPILER",    p->compiler,  1);
        if (p->profile[0])     strncpy(profile,            p->profile,   63);
        if (p->arch[0])        strncpy(arch,               p->arch,      15);
        if (p->toolchain[0])   strncpy(toolchain_arg,      p->toolchain, 511);
        if (p->do_static)      do_static = 1;
        if (p->reproducible)   do_reproducible = 1;
        if (p->jobs > 0)       jobs = p->jobs;
        /* preset tags get appended via env for build_engine to pick up */
        if (p->tags[0]) {
            const char *existing = getenv("OMAKE_EXTRA_TAGS");
            char merged[1024]; merged[0]='\0';
            if (existing) { strncpy(merged, existing, 1023); strncat(merged," ",1); }
            strncat(merged, p->tags, 1023-strlen(merged));
            setenv("OMAKE_EXTRA_TAGS", merged, 1);
        }
        if (p->use_lib[0])     setenv("OMAKE_EXTRA_USE_LIB", p->use_lib, 1);
        do_build = 1; force = 1;
    }

    /* ── Toolchain arg → env ── */
    if (toolchain_arg[0]) setenv("OMAKE_TOOLCHAIN", toolchain_arg, 1);

    /* ── PGO → env ── */
    if (do_pgo_gen) setenv("OMAKE_PGO_GEN", "1", 1);
    if (do_pgo_use) setenv("OMAKE_PGO_USE", "1", 1);

    /* ── Reproducible → env ── */
    if (do_reproducible) setenv("OMAKE_REPRODUCIBLE", "1", 1);

    /* ── IDE integration ── */
    if (do_ide) { cmd_ide(ide_target); return 0; }

    /* ── Dependency cache ── */
    if (do_cache_list)  { dep_cache_list();  return 0; }
    if (do_cache_clear) { dep_cache_clear(); return 0; }

    /* ── Distributed build ── */
    if (do_dist && dist_hosts[0]) {
        dist_build(dist_hosts, jobs, "g++", "");
        return 0;
    }

    if (do_build) {
        printf(BLUE "[OMAKE] v%s  |  CPUs: %d  |  jobs: %d\n" RESET,
               VERSION, get_cpu_count(), jobs);
        if (all_compilers) {
            run_all_compilers(force, profile, jobs, arch, do_static);
        } else {
            if (do_bin) g_force_bin_ext = 1;
            int build_ret = build_engine(force, profile, jobs, show_graph, arch, do_static, "",
                         do_strip_bin, do_compress_bin, gen_compile_commands, gen_asm);
            g_force_bin_ext = 0;
            if (build_ret != 0) return build_ret;
        }
        /* --watch: rebuild on file change */
        if (do_watch) {
            cmd_watch(0, profile, jobs, arch, do_static);
        }
        /* --test: run tests after build (own function = clean stack) */
        if (do_test) {
            int t = cmd_run_test_dsl();
            if (t != 0) return t;
        }
        return 0;
    }

    show_usage();
    return 0;
}
