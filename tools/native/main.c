/* MonoMesh native tile generator: CLI, TUI and parallel pipeline. */
#include "monomesh.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static int g_quiet;

static void info(const char *fmt, ...)
{
    if (g_quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* --------------------------------------------------------------- helpers -- */

/* Cache key: CRC32 over the raw bytes of the four bounding box doubles, so the
 * native tool and the Python tool derive exactly the same file name. */
static void bbox_cache_key(const double bbox[4], char *out, size_t out_len)
{
    unsigned char raw[32];
    memcpy(raw, bbox, sizeof(raw));
    uint32_t crc = (uint32_t)crc32(0L, raw, sizeof(raw));
    snprintf(out, out_len, "%08x", crc);
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void join_path(char *out, size_t out_len, const char *a, const char *b)
{
    snprintf(out, out_len, "%s/%s", a, b);
}

static long default_jobs(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2)
        return 1;
    return n - 1;
}

/* ------------------------------------------------------------------- TUI -- */

static void read_line(const char *prompt, char *buf, size_t len, const char *dflt)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)len, stdin)) {
        buf[0] = '\0';
    } else {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
    }
    if (buf[0] == '\0' && dflt)
        snprintf(buf, len, "%s", dflt);
}

static int find_pbfs(char paths[][PATH_MAX], int max)
{
    int n = 0;
    const char *dirs[] = {".", ".."};
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && n < max; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp)
            continue;
        struct dirent *e;
        while ((e = readdir(dp)) && n < max) {
            size_t l = strlen(e->d_name);
            if (l > 8 && strcmp(e->d_name + l - 8, ".osm.pbf") == 0) {
                char full[PATH_MAX], real[PATH_MAX];
                join_path(full, sizeof(full), dirs[d], e->d_name);
                if (!realpath(full, real))
                    continue;
                int dup = 0;
                for (int i = 0; i < n; i++)
                    if (strcmp(paths[i], real) == 0)
                        dup = 1;
                if (!dup)
                    snprintf(paths[n++], PATH_MAX, "%s", real);
            }
        }
        closedir(dp);
    }
    return n;
}

/* ------------------------------------------------------------ tile lists -- */

typedef struct {
    int32_t *z, *x, *y;
    size_t len, cap;
} tile_list;

static int tile_push(tile_list *t, int z, int x, int y)
{
    if (t->len == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 512;
        int32_t *nz = realloc(t->z, sizeof(int32_t) * ncap);
        int32_t *nx = realloc(t->x, sizeof(int32_t) * ncap);
        int32_t *ny = realloc(t->y, sizeof(int32_t) * ncap);
        if (!nz || !nx || !ny) {
            free(nz);
            free(nx);
            free(ny);
            return -1;
        }
        t->z = nz;
        t->x = nx;
        t->y = ny;
        t->cap = ncap;
    }
    t->z[t->len] = z;
    t->x[t->len] = x;
    t->y[t->len] = y;
    t->len++;
    return 0;
}

static void tile_free(tile_list *t)
{
    free(t->z);
    free(t->x);
    free(t->y);
    memset(t, 0, sizeof(*t));
}

/* ----------------------------------------------------------- render pool -- */

typedef struct {
    const tile_list *tiles;
    size_t next;
    pthread_mutex_t lock;
    mm_index **indexes;
    const int *zooms;
    int n_zooms;
    const char *out_dir;
    int style_dark;
    int png_bits;
    size_t total_bytes;
    size_t rendered;
    int failed;
    int show_progress;
} pool_ctx;

static mm_index *index_for_zoom(pool_ctx *p, int z)
{
    for (int i = 0; i < p->n_zooms; i++)
        if (p->zooms[i] == z)
            return p->indexes[i];
    return NULL;
}

static void *render_worker(void *arg)
{
    pool_ctx *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        size_t i = p->next++;
        pthread_mutex_unlock(&p->lock);
        if (i >= p->tiles->len)
            break;

        int z = p->tiles->z[i], tx = p->tiles->x[i], ty = p->tiles->y[i];
        mm_index *ix = index_for_zoom(p, z);
        if (!ix) {
            pthread_mutex_lock(&p->lock);
            p->failed = 1;
            pthread_mutex_unlock(&p->lock);
            break;
        }
        char dir[PATH_MAX], path[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%d/%d", p->out_dir, z, tx);
        mm_mkpath(dir);
        snprintf(path, sizeof(path), "%s/%d.png", dir, ty);
        size_t size = 0;
        if (mm_render_tile_png(ix, z, tx, ty, p->style_dark, p->png_bits, path, &size) != 0) {
            pthread_mutex_lock(&p->lock);
            p->failed = 1;
            pthread_mutex_unlock(&p->lock);
            break;
        }
        struct stat st;
        if (stat(path, &st) == 0)
            size = (size_t)st.st_size;

        pthread_mutex_lock(&p->lock);
        p->total_bytes += size;
        p->rendered++;
        if (p->show_progress && (p->rendered % 50 == 0 || p->rendered == p->tiles->len))
            printf("[%zu/%zu] tiles rendered\n", p->rendered, p->tiles->len);
        pthread_mutex_unlock(&p->lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ main -- */

static void usage(void)
{
    printf(
        "monomesh-tiles - MonoMesh OSM PBF Toner tile generator (native)\n\n"
        "Usage:\n"
        "  monomesh-tiles --tui\n"
        "  monomesh-tiles --pbf <file.osm.pbf> --bbox min_lat min_lon max_lat max_lon [options]\n"
        "  monomesh-tiles --pbf <file.osm.pbf> --lat 45.81 --lon 9.08 --radius_km 4 [options]\n\n"
        "Options:\n"
        "  --pbf PATH          OSM PBF input file\n"
        "  --lat/--lon FLOAT   centre coordinates\n"
        "  --radius_km FLOAT   radius around the centre (default 4)\n"
        "  --bbox A B C D      min_lat min_lon max_lat max_lon\n"
        "  --zooms LIST        comma separated zoom levels (default 12,13,14)\n"
        "  --dark              inverted (black background) style\n"
        "  --output DIR        destination directory (default ./tiles)\n"
        "  --force             re-extract and re-render everything\n"
        "  --jobs N            render threads (default: cores-1)\n"
        "  --extract-jobs N    PBF parsing threads (default: jobs)\n"
        "  --png-mode MODE     fast (default) | small | mono (1-bit)\n"
        "  --cache-dir DIR     cache directory (default <pbf dir>/.pbf_cache)\n"
        "  --quiet             less output\n"
        "  --help              this message\n");
}

int main(int argc, char **argv)
{
    const char *pbf = NULL, *output = "./tiles", *cache_dir = NULL, *png_mode = "fast";
    const char *zooms_arg = "12,13,14";
    const char *city_arg = NULL;
    double lat = 0, lon = 0, radius_km = 4.0;
    double bbox[4] = {0, 0, 0, 0};
    int have_bbox = 0, have_center = 0, force = 0, tui = 0;
    int jobs = (int)default_jobs(), extract_jobs = 0;
    int style_dark = 0, png_bits = 8;
    char area_name[256] = "area";
    char zooms_buf[256] = "12,13,14";
    char output_buf[PATH_MAX] = "./tiles";

    if (argc == 1)
        tui = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--tui") == 0) tui = 1;
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
        else if (strcmp(a, "--pbf") == 0 && i + 1 < argc) pbf = argv[++i];
        else if (strcmp(a, "--city") == 0 && i + 1 < argc) city_arg = argv[++i];
        else if (strcmp(a, "--lat") == 0 && i + 1 < argc) { lat = atof(argv[++i]); have_center = 1; }
        else if (strcmp(a, "--lon") == 0 && i + 1 < argc) { lon = atof(argv[++i]); have_center = 1; }
        else if (strcmp(a, "--radius_km") == 0 && i + 1 < argc) radius_km = atof(argv[++i]);
        else if (strcmp(a, "--bbox") == 0 && i + 4 < argc) {
            for (int k = 0; k < 4; k++)
                bbox[k] = atof(argv[++i]);
            have_bbox = 1;
        } else if (strcmp(a, "--zooms") == 0 && i + 1 < argc) zooms_arg = argv[++i];
        else if (strcmp(a, "--dark") == 0) style_dark = 1;
        else if (strcmp(a, "--output") == 0 && i + 1 < argc) output = argv[++i];
        else if (strcmp(a, "--cache-dir") == 0 && i + 1 < argc) cache_dir = argv[++i];
        else if (strcmp(a, "--force") == 0) force = 1;
        else if (strcmp(a, "--jobs") == 0 && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (strcmp(a, "--extract-jobs") == 0 && i + 1 < argc) extract_jobs = atoi(argv[++i]);
        else if (strcmp(a, "--png-mode") == 0 && i + 1 < argc) png_mode = argv[++i];
        else if (strcmp(a, "--quiet") == 0) g_quiet = 1;
        else {
            fprintf(stderr, "[!] Unrecognized argument: %s\n", a);
            usage();
            return 2;
        }
    }
    if (jobs < 1)
        jobs = 1;
    if (extract_jobs <= 0)
        extract_jobs = jobs;
    if (strcmp(png_mode, "mono") == 0)
        png_bits = 1;

    char pbf_buf[PATH_MAX];
    if (tui) {
        printf("===========================================================\n");
        printf("  MonoMesh Offline PBF Tile Generator (Toner E-Paper) C\n");
        printf("===========================================================\n\n");
        static char paths[8][PATH_MAX];
        int n = find_pbfs(paths, 8);
        char line[512];
        if (n == 0) {
            printf("[!] No .osm.pbf file found!\n");
            read_line("Path to the .osm.pbf file: ", line, sizeof(line), NULL);
            snprintf(pbf_buf, sizeof(pbf_buf), "%s", line);
        } else if (n == 1) {
            snprintf(pbf_buf, sizeof(pbf_buf), "%s", paths[0]);
            printf("[+] PBF file detected: %s\n", paths[0]);
        } else {
            printf("PBF files found:\n");
            for (int i = 0; i < n; i++)
                printf("  [%d] %s\n", i + 1, paths[i]);
            read_line("Choose the PBF file [default: 1]: ", line, sizeof(line), "1");
            int sel = atoi(line);
            if (sel < 1 || sel > n)
                sel = 1;
            snprintf(pbf_buf, sizeof(pbf_buf), "%s", paths[sel - 1]);
        }
        pbf = pbf_buf;

        printf("\nChoose how to define the area to extract:\n");
        printf("  [1] Search by city / municipality name\n");
        printf("  [2] Manual coordinates (lat, lon, radius km)\n");
        printf("  [3] Manual bounding box\n");
        read_line("Choice [1..3, default: 1]: ", line, sizeof(line), "1");
        if (line[0] == '2') {
            read_line("Latitude (e.g. 45.8081): ", line, sizeof(line), "45.8081");
            lat = atof(line);
            read_line("Longitude (e.g. 9.0852): ", line, sizeof(line), "9.0852");
            lon = atof(line);
            read_line("Radius in km [default: 4.0]: ", line, sizeof(line), "4.0");
            radius_km = atof(line);
            have_center = 1;
            snprintf(area_name, sizeof(area_name), "lat%.2f_lon%.2f_r%ikm", lat, lon,
                     (int)radius_km);
        } else if (line[0] == '3') {
            read_line("Min Lat (South): ", line, sizeof(line), NULL);
            bbox[0] = atof(line);
            read_line("Max Lat (North): ", line, sizeof(line), NULL);
            bbox[2] = atof(line);
            read_line("Min Lon (West): ", line, sizeof(line), NULL);
            bbox[1] = atof(line);
            read_line("Max Lon (East): ", line, sizeof(line), NULL);
            bbox[3] = atof(line);
            have_bbox = 1;
            snprintf(area_name, sizeof(area_name), "bbox_%.2f_%.2f", bbox[0], bbox[1]);
        } else {
            read_line("City or municipality name: ", line, sizeof(line), "Como");
            for (char *q = line; *q; q++)
                *q = (char)tolower((unsigned char)*q);
            static const char *const names[] = {
                "milano", "como", "varese", "lecco", "monza", "bergamo", "brescia", "sondrio",
                "pavia", "cremona", "mantova", "lodi", "torino", "novara", "verbania", "genova",
                "bologna", "roma", "firenze", "venezia", "verona", "trento", "bolzano", "aosta"};
            static const double coords[][2] = {
                {45.4642, 9.1900}, {45.8081, 9.0852}, {45.8184, 8.8239}, {45.8559, 9.3902},
                {45.5845, 9.2744}, {45.6983, 9.6773}, {45.5416, 10.2118}, {46.1712, 9.8728},
                {45.1847, 9.1582}, {45.1332, 10.0275}, {45.1564, 10.7914}, {45.3142, 9.5033},
                {45.0703, 7.6869}, {45.4469, 8.6219}, {45.9228, 8.5518}, {44.4056, 8.9463},
                {44.4949, 11.3426}, {41.9028, 12.4964}, {43.7696, 11.2558}, {45.4408, 12.3155},
                {45.4384, 10.9916}, {46.0748, 11.1217}, {46.4983, 11.3548}, {45.7373, 7.3196}};
            int found = -1;
            for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
                if (strcmp(names[k], line) == 0)
                    found = (int)k;
            if (found < 0) {
                printf("[!] City not in the offline list: using Como.\n");
                found = 1;
            }
            lat = coords[found][0];
            lon = coords[found][1];
            have_center = 1;
            read_line("Radius in km to cover [default: 4.0]: ", line, sizeof(line), "4.0");
            radius_km = atof(line);
            snprintf(area_name, sizeof(area_name), "%s_r%ikm", names[found], (int)radius_km);
        }

        printf("\nZoom Levels:\n");
        printf("  - Z12: overview    - Z13: city (recommended)\n");
        printf("  - Z14: street detail   - Z15: blocks\n");
        read_line("Zoom levels separated by comma [default: 12,13,14]: ", line, sizeof(line),
                  "12,13,14");
        snprintf(zooms_buf, sizeof(zooms_buf), "%s", line);
        zooms_arg = zooms_buf;

        read_line("\nToner style: [1] Pure B/W (default) [2] Dark inverted: ", line, sizeof(line), "1");
        style_dark = (line[0] == '2');

        char jobs_line[32];
        snprintf(jobs_line, sizeof(jobs_line), "%d", jobs);
        read_line("Parallel processes [default: auto]: ", line, sizeof(line), jobs_line);
        jobs = atoi(line);
        if (jobs < 1)
            jobs = (int)default_jobs();
        extract_jobs = jobs;

        read_line("PNG quality: [1] fast [2] small [3] mono 1-bit [default: 1]: ",
                  line, sizeof(line), "1");
        png_mode = (line[0] == '3') ? "mono" : (line[0] == '2' ? "small" : "fast");
        png_bits = (line[0] == '3') ? 1 : 8;

        read_line("\nDestination folder [default: ./tiles]: ", line, sizeof(line), "./tiles");
        snprintf(output_buf, sizeof(output_buf), "%s", line);
        output = output_buf;
        read_line("Overwrite existing tiles if present? (y/N): ", line, sizeof(line), "n");
        force = (line[0] == 'y' || line[0] == 'Y');
    }

    if (!pbf || !path_exists(pbf)) {
        fprintf(stderr, "[!] PBF file not found%s%s\n", pbf ? ": " : "", pbf ? pbf : "");
        return 1;
    }
    if (city_arg && !have_center && !have_bbox) {
        static const char *const names[] = {
            "milano", "como", "varese", "lecco", "monza", "bergamo", "brescia", "sondrio",
            "pavia", "cremona", "mantova", "lodi", "torino", "novara", "verbania", "genova",
            "bologna", "roma", "firenze", "venezia", "verona", "trento", "bolzano", "aosta"};
        static const double coords[][2] = {
            {45.4642, 9.1900}, {45.8081, 9.0852}, {45.8184, 8.8239}, {45.8559, 9.3902},
            {45.5845, 9.2744}, {45.6983, 9.6773}, {45.5416, 10.2118}, {46.1712, 9.8728},
            {45.1847, 9.1582}, {45.1332, 10.0275}, {45.1564, 10.7914}, {45.3142, 9.5033},
            {45.0703, 7.6869}, {45.4469, 8.6219}, {45.9228, 8.5518}, {44.4056, 8.9463},
            {44.4949, 11.3426}, {41.9028, 12.4964}, {43.7696, 11.2558}, {45.4408, 12.3155},
            {45.4384, 10.9916}, {46.0748, 11.1217}, {46.4983, 11.3548}, {45.7373, 7.3196}};
        char lower[256];
        snprintf(lower, sizeof(lower), "%s", city_arg);
        for (char *q = lower; *q; q++)
            *q = (char)tolower((unsigned char)*q);
        int found = -1;
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            if (strcmp(names[k], lower) == 0)
                found = (int)k;
        if (found < 0) {
            fprintf(stderr, "[!] City '%s' not in the offline list\n", city_arg);
            return 1;
        }
        lat = coords[found][0];
        lon = coords[found][1];
        have_center = 1;
        snprintf(area_name, sizeof(area_name), "%s_r%ikm", lower, (int)radius_km);
    }

    if (have_bbox) {
        if (area_name[0] == 'a')
            snprintf(area_name, sizeof(area_name), "bbox_%.2f_%.2f", bbox[0], bbox[1]);
    } else if (have_center) {
        double dlat = radius_km / 111.0;
        double dlon = radius_km / (111.0 * cos(lat * M_PI / 180.0));
        bbox[0] = lat - dlat;
        bbox[2] = lat + dlat;
        bbox[1] = lon - dlon;
        bbox[3] = lon + dlon;
    } else {
        bbox[0] = 45.77; bbox[1] = 9.04; bbox[2] = 45.85; bbox[3] = 9.13;
        snprintf(area_name, sizeof(area_name), "como_default");
    }

    int zooms[16], n_zooms = 0;
    {
        const char *s = zooms_arg;
        while (*s && n_zooms < 16) {
            int z = atoi(s);
            if (z > 0 && z <= 22)
                zooms[n_zooms++] = z;
            while (*s && *s != ',')
                s++;
            if (*s == ',')
                s++;
        }
    }
    if (n_zooms == 0) {
        fprintf(stderr, "[!] No valid zoom level\n");
        return 1;
    }

    static char cache_dir_buf[PATH_MAX];
    if (!cache_dir) {
        const char *slash = strrchr(pbf, '/');
        if (slash)
            snprintf(cache_dir_buf, sizeof(cache_dir_buf), "%.*s/.pbf_cache",
                     (int)(slash - pbf), pbf);
        else
            snprintf(cache_dir_buf, sizeof(cache_dir_buf), ".pbf_cache");
        cache_dir = cache_dir_buf;
    }
    mm_mkpath(cache_dir);
    char key[32], cache_path[PATH_MAX];
    bbox_cache_key(bbox, key, sizeof(key));
    snprintf(cache_path, sizeof(cache_path), "%s/%s_%s.mosmb", cache_dir, area_name, key);

    double t_start = mm_now();
    mm_data data;
    memset(&data, 0, sizeof(data));
    int from_cache = 0;
    if (!force && path_exists(cache_path)) {
        info("[*] Loading vectors from cache: %s\n", cache_path);
        if (mm_cache_read(cache_path, &data) == 0) {
            /* the tile grid always follows the requested bbox */
            memcpy(data.bbox, bbox, sizeof(bbox));
            info("[+] Cache loaded in %.2fs (%d streets, %d places)\n",
                 mm_now() - t_start, data.n_ways, data.n_places);
            from_cache = 1;
        } else {
            info("[!] Unreadable cache, re-extracting...\n");
        }
    }
    if (!from_cache) {
        double dlat = (bbox[2] - bbox[0]) * 0.15;
        double dlon = (bbox[3] - bbox[1]) * 0.15;
        double eb[4] = {bbox[0] - dlat, bbox[1] - dlon, bbox[2] + dlat, bbox[3] + dlon};
        struct stat st;
        if (stat(pbf, &st) == 0)
            info("[*] Scanning PBF: %s (%.0f MB) with %d threads\n", pbf,
                 st.st_size / (1024.0 * 1024.0), extract_jobs);
        double t0 = mm_now();
        if (mm_pbf_extract(pbf, eb, bbox, &data, extract_jobs) != 0) {
            fprintf(stderr, "[!] PBF extraction failed\n");
            return 1;
        }
        info("[+] Extraction completed in %.1fs: %d ways, %d places\n",
             mm_now() - t0, data.n_ways, data.n_places);
        if (mm_cache_write(cache_path, &data) == 0)
            info("[*] Cache saved to: %s\n", cache_path);
    }

    tile_list tiles = {0};
    size_t total_tiles = 0;
    for (int zi = 0; zi < n_zooms; zi++) {
        mm_grid g = mm_tile_grid(&data, zooms[zi]);
        total_tiles += (size_t)g.nx * (size_t)g.ny;
        for (int32_t x = g.tx0; x < g.tx0 + g.nx; x++) {
            for (int32_t y = g.ty0; y < g.ty0 + g.ny; y++) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%d/%d/%d.png", output, zooms[zi], x, y);
                if (!force && path_exists(path))
                    continue;
                if (tile_push(&tiles, zooms[zi], x, y) != 0) {
                    fprintf(stderr, "[!] Out of memory\n");
                    return 1;
                }
            }
        }
    }
    info("\n[*] Rendering %zu Toner tiles (Zoom", total_tiles);
    for (int zi = 0; zi < n_zooms; zi++)
        info(" %d", zooms[zi]);
    info(")...\n");
    if (tiles.len == 0) {
        info("[+] Nothing to render: all tiles are already present.\n");
        mm_data_free(&data);
        tile_free(&tiles);
        return 0;
    }

    int ref_z = zooms[0];
    for (int i = 1; i < n_zooms; i++)
        if (zooms[i] > ref_z)
            ref_z = zooms[i];
    double t_idx = mm_now();
    double *xy = mm_project_points(&data, ref_z);
    if (!xy) {
        fprintf(stderr, "[!] Insufficient memory for projection\n");
        return 1;
    }
    mm_index **indexes = calloc((size_t)n_zooms, sizeof(*indexes));
    if (!indexes) {
        fprintf(stderr, "[!] Insufficient memory\n");
        return 1;
    }
    for (int i = 0; i < n_zooms; i++) {
        indexes[i] = mm_index_build(&data, zooms[i], ref_z, xy);
        if (!indexes[i]) {
            fprintf(stderr, "[!] Cannot build index for zoom %d\n", zooms[i]);
            return 1;
        }
    }
    info("[+] Index ready in %.2fs\n", mm_now() - t_idx);

    pool_ctx pool;
    memset(&pool, 0, sizeof(pool));
    pool.tiles = &tiles;
    pool.indexes = indexes;
    pool.zooms = zooms;
    pool.n_zooms = n_zooms;
    pool.out_dir = output;
    pool.style_dark = style_dark;
    pool.png_bits = png_bits;
    pool.show_progress = !g_quiet;
    pthread_mutex_init(&pool.lock, NULL);

    int nthreads = jobs;
    if ((size_t)nthreads > tiles.len)
        nthreads = (int)tiles.len;
    if (nthreads < 1)
        nthreads = 1;
    pthread_t *threads = calloc((size_t)nthreads, sizeof(pthread_t));
    double t_render = mm_now();
    for (int i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, render_worker, &pool);
    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);
    double render_time = mm_now() - t_render;
    pthread_mutex_destroy(&pool.lock);

    if (pool.failed) {
        fprintf(stderr, "[!] Rendering aborted due to an error\n");
        return 1;
    }
    double mb = pool.total_bytes / (1024.0 * 1024.0);
    info("\n===========================================================\n");
    info("[+] Toner rendering completed!\n");
    info("    Tiles generated: %zu in %.2fs (%.1f tiles/s)\n", pool.rendered, render_time,
         pool.rendered / (render_time > 0.001 ? render_time : 0.001));
    info("    Total time: %.2fs with %d threads\n", mm_now() - t_start, nthreads);
    info("    Disk usage: %.2f MB (~%.1f KB/tile)\n", mb,
         pool.total_bytes / (double)(pool.rendered ? pool.rendered : 1) / 1024.0);
    info("    Destination: %s\n", output);
    info("\n[*] Copy the folder to the MicroSD root: /sdcard/tiles/{z}/{x}/{y}.png\n");
    info("[*] Map data: (C) OpenStreetMap contributors (ODbL)\n");
    info("===========================================================\n");

    for (int i = 0; i < n_zooms; i++)
        mm_index_free(indexes[i]);
    free(indexes);
    free(xy);
    free(threads);
    mm_data_free(&data);
    tile_free(&tiles);
    return 0;
}
