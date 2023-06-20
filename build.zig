const std = @import("std");
const print = @import("std").debug.print;

pub fn build(b: *std.build.Builder) void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{});
    const t = target.toTarget();

    const lib = b.addStaticLibrary(.{
        .name = "gc",
        .target = target,
        .optimize = optimize,
    });

    var flags = std.ArrayList([]const u8).init(b.allocator);
    defer flags.deinit();

    flags.appendSlice(&.{
        "-fno-sanitize=undefined",
        // These are standard options from Makefile.direct
        "-DALL_INTERIOR_POINTERS",
        "-DENABLE_DISCLAIM",
        "-DGC_ATOMIC_UNCOLLECTABLE",
        "-DGC_GCJ_SUPPORT",
        "-DJAVA_FINALIZATION",
        "-DNO_EXECUTE_PERMISSION",
        "-DUSE_MMAP",
        "-DUSE_MUNMAP",
        // Acton specific config
        // TODO: how to modularize this?
        "-DLARGE_CONFIG",
        "-DGC_BUILTIN_ATOMIC",
        "-DGC_THREADS",
        "-DNO_PROC_FOR_LIBRARIES",
        "-DREDIRECT_MALLOC=GC_malloc",
        "-DIGNORE_FREE",
        "-DPARALLEL_MARK",
        "-DNO_GETCONTEXT",
    }) catch |err| {
        std.log.err("Error appending flags: {}", .{err});
        std.os.exit(1);
    };

    if (t.abi.isMusl()) {
        print("Using musl flags\n", .{});
        flags.appendSlice(&.{
            "-DNO_GETCONTEXT",
        }) catch |err| {
            std.log.err("Error appending flags: {}", .{err});
            std.os.exit(1);
        };
    }

    const source_files = [_][]const u8{
        "allchblk.c",
        "alloc.c",
        "backgraph.c",
        "blacklst.c",
        "checksums.c",
        "darwin_stop_world.c",
        "dbg_mlc.c",
        "dyn_load.c",
        "finalize.c",
        "fnlz_mlc.c",
        "gc_dlopen.c",
        "gcj_mlc.c",
        "headers.c",
        "mach_dep.c",
        "malloc.c",
        "mallocx.c",
        "mark.c",
        "mark_rts.c",
        "misc.c",
        "new_hblk.c",
        "obj_map.c",
        "os_dep.c",
        "pthread_start.c",
        "pthread_stop_world.c",
        "pthread_support.c",
        "ptr_chck.c",
        "reclaim.c",
        "specific.c",
        "thread_local_alloc.c",
        "typd_mlc.c",
        "win32_threads.c"
    };

    lib.addCSourceFiles(&source_files, flags.items);
    lib.addIncludePath("include");
    lib.linkLibC();
    lib.installHeader("include/gc.h", "gc.h");
    lib.installHeadersDirectory("include/gc", "gc");
    b.installArtifact(lib);

}
