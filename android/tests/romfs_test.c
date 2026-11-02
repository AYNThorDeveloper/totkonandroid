/* Exercise the actual backend, including exported SDK symbol dispatch. */
#include "../android_romfs.c"
#include <assert.h>
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL: %s\n", s); exit(90); }
static int expect_unlocked;
static uint64_t close_during_read;
static int overlap_probe, overlap_entered;
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_cv = PTHREAD_COND_INITIALIZER;
void dm_guest_write_prepare(void *buf, size_t size) {
    assert(buf && size);
    if (overlap_probe) {
        pthread_mutex_lock(&probe_lock); overlap_entered++; pthread_cond_broadcast(&probe_cv);
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 5;
        while (overlap_entered < 2) assert(!pthread_cond_timedwait(&probe_cv, &probe_lock, &deadline));
        pthread_mutex_unlock(&probe_lock);
    }
    if (expect_unlocked) {
        assert(!pthread_mutex_trylock(&lock)); pthread_mutex_unlock(&lock);
        if (close_during_read) {
            uint64_t old = close_during_read, replacement; close_during_read = 0;
            close_file(old);
            assert(!open_file(&replacement, "rom:/tail", 1) && replacement != old);
            close_file(replacement);
        }
    }
}
static int fs_host_marker;
void *runtime_fs_host_override(const char *name) { return strcmp(name, "unknown") ? &fs_host_marker : NULL; }
static void *reader(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 100; ++i) {
        uint64_t h, n; char buf[8] = {0};
        assert(!open_file(&h, "rom://Sound/test", 1));
        assert(!read_file(&n, h, 0, buf, sizeof buf) && n == 5 && !memcmp(buf, "hello", 5));
        close_file(h);
    }
    return NULL;
}
static void *overlap_reader(void *arg) {
    uint64_t n; char buf[8];
    assert(!read_file(&n, *(uint64_t *)arg, 0, buf, sizeof buf) && n == 5 && !memcmp(buf, "hello", 5));
    return NULL;
}
int main(int argc, char **argv) {
    runtime_fs_init();
    uint64_t h = 0, n = 9, stale; int type; int64_t size; char buf[16] = {0};
    assert(!cache_size(&n) && n == 0 && cache_size(NULL) == INVALID);
    assert(runtime_fs_override("_ZN2nn2fs8MountRomEPKcPvm") == mount_rom);
    /* File entry points belong to fs_host.c, which routes RomFS work here. */
    assert(runtime_fs_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci") == &fs_host_marker);
    assert(!runtime_fs_override("unknown"));
    assert(!runtime_romfs_owns("rom:/Sound/test"));
    assert(open_file(&h, "rom:/Sound/test", 1) == NOT_FOUND);
    assert(mount_rom("../bad", NULL, 0) == INVALID);
    assert(mount_rom("rom:", NULL, 0) == INVALID);
    if (argc == 2 && !strcmp(argv[1], "init-only")) return 0;
    assert(!mount_rom("rom", NULL, 0));
    if (argc == 2 && !strcmp(argv[1], "mount-only")) return 0;
    assert(runtime_romfs_owns("rom:/Sound/test") && !runtime_romfs_owns("save:/a") && !runtime_romfs_owns("romx:/a"));
    assert(!runtime_romfs_open(&h, "rom:/Sound/test", 1) && runtime_romfs_handle(h) && !runtime_romfs_handle(0x200000001ULL));
    assert(!runtime_romfs_size(&size, h) && size == 5);
    assert(!runtime_romfs_read(&n, h, 0, buf, sizeof buf) && n == 5 && !memcmp(buf, "hello", 5));
    runtime_romfs_close(h);
    assert(!runtime_romfs_entry_type(&type, "rom:/Sound") && type == 0);
    memset(buf, 0, sizeof buf);
    assert(!mount_rom("rom", NULL, 0));
    assert(mount_rom("other", NULL, 0) == UNSUPPORTED);
    assert(!entry_type(&type, "rom:/") && type == 0);
    assert(!entry_type(&type, "rom://Sound") && type == 0);
    assert(!entry_type(&type, "rom:/Sound/test") && type == 1);
    assert(entry_type(&type, "rom:/Sound/absent") == NOT_FOUND);
    assert(entry_type(&type, "rom:/Soun") == NOT_FOUND);
    const char *bad[] = {"rom:/../Sound/test", "rom://../Sound/test", "rom:/Sound//test", "rom:/Sound/./test", "rom:/Sound/../test", "other:/Sound/test", "rom:/Sound\\test", "rom:", "r"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) assert(open_file(&h, bad[i], 1) == NOT_FOUND);
    assert(open_file(&h, "rom:/Sound/test", 2) == UNSUPPORTED);
    assert(!open_file(&h, "rom:///Sound/test", 1));
    assert(!file_size(&size, h) && size == 5);
    assert(!read_file(&n, h, 2, buf, sizeof buf) && n == 3 && !memcmp(buf, "llo", 3));
    assert(!read_file(&n, h, 5, buf, 1) && n == 0);
    assert(!read_file(&n, h, 5, NULL, 0) && n == 0);
    assert(read_file(&n, h, 6, buf, 1) == INVALID);
    assert(read_file(&n, h, -1, buf, 1) == INVALID);
    assert(read_file(&n, h, 0, NULL, 1) == INVALID);
    assert(!read_file(&n, h, 0, buf, 6) && n == 5 && !memcmp(buf, "hello", 5));   /* short read at EOF */
    stale = h; close_file(h);
    assert(file_size(&size, stale) == INVALID);
    assert(!open_file(&h, "rom:/Sound/test", 1) && h != stale);
    assert(read_file(&n, stale, 0, buf, 1) == INVALID); close_file(h);
    assert(!open_file(&h, "rom:/empty", 1));
    assert(!file_size(&size, h) && size == 0);
    assert(!read_file(&n, h, 0, buf, 2) && n == 0); close_file(h);
    assert(!open_file(&h, "rom:/tail", 1));
    assert(!read_file(&n, h, 0, buf, sizeof buf) && n == 8 && !memcmp(buf, "BOUNDARY", 8)); close_file(h);
    assert(file_size(&size, UINT64_MAX) == INVALID);
    uint64_t all[NH - 1];
    for (unsigned i = 0; i < NH - 1; ++i) assert(!open_file(&all[i], "rom:/empty", 1));
    assert(open_file(&h, "rom:/empty", 1) == UNSUPPORTED);
    for (unsigned i = 0; i < NH - 1; ++i) close_file(all[i]);
    if (romfs_parallel_on()) {
        assert(!open_file(&h, "rom:/Sound/test", 1));
        close_during_read = h; expect_unlocked = 1;
        assert(!read_file(&n, h, 0, buf, 5) && n == 5 && !memcmp(buf, "hello", 5));
        assert(!close_during_read && file_size(&size, h) == INVALID);
        expect_unlocked = 0;
        uint64_t same_file[2]; pthread_t pair[2];
        assert(!open_file(&same_file[0], "rom:/Sound/test", 1) && !open_file(&same_file[1], "rom:/Sound/test", 1));
        overlap_probe = 1;
        for (unsigned j = 0; j < 2; ++j) assert(!pthread_create(&pair[j], NULL, overlap_reader, &same_file[j]));
        for (unsigned j = 0; j < 2; ++j) assert(!pthread_join(pair[j], NULL));
        assert(overlap_entered == 2); overlap_probe = 0;
        close_file(same_file[0]); close_file(same_file[1]);
        puts("RomFS concurrent reads: pass (two readers simultaneously reach guest write preparation)");
        puts("RomFS parallel read lifetime: pass (table unlocked, captured image survives close and slot reuse)");
    }
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, reader, NULL));
    for (int i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    puts("RomFS functional/concurrent tests passed (synthetic data; no game startup)");
    return 0;
}
