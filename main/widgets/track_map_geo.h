/*
 * track_map_geo.h — RDMTRK parsing and the geography→pixel projection.
 *
 * Split out from widget_track_map.c on purpose: everything in here is pure
 * arithmetic over a byte buffer, with no LVGL, no FreeRTOS and no ESP-IDF, so
 * it compiles and runs on the host under tests/native. That matters because
 * this is where the bugs that cannot be seen would live — a wrong endianness,
 * a sign flip, an off-by-one in the header — and none of those announce
 * themselves on a dash. They just draw a track somewhere that is not the
 * track.
 *
 * The format is written by RDM Studio (gpTrackAssetBuild in
 * rdm7-desktop/src/tauri-overlay.html). Both sides are tested against each
 * other by tests/native/test_track_map_geo.c, which parses a buffer produced
 * by the real Studio encoder rather than by a re-implementation of it.
 *
 *   off  size  field
 *     0     6  magic "RDMTRK"
 *     6     1  u8  version   (loader REJECTS unknown — RDMIMG's version byte
 *                             is written and never read, so it cannot be
 *                             versioned in place; do not repeat that)
 *     7     1  u8  flags     bit0 = the shape closes on itself
 *     8    32  char name[32] NUL-padded, the human-readable track name
 *    40     4  i32 lat0_1e7  projection origin (bbox centre)
 *    44     4  i32 lon0_1e7
 *    48     2  u16 n_points
 *    50     2  u16 reserved  pad, so the point array is 4-ALIGNED
 *    52   8·n  i32 lat_1e7, i32 lon_1e7  × n_points
 */
#ifndef TRACK_MAP_GEO_H
#define TRACK_MAP_GEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACK_MAP_MAGIC       "RDMTRK"
#define TRACK_MAP_MAGIC_LEN   6
#define TRACK_MAP_VERSION     1
#define TRACK_MAP_HEADER      52
#define TRACK_MAP_NAME_LEN    32

/* Shared by the loader AND the upload endpoint. web_server_assets.c already
 * carries a comment about exactly this hazard for fonts: if the upload cap and
 * the loader's limit disagree, a file in between uploads happily and then
 * silently fails to load. One constant, included by both. */
#define TRACK_MAP_MAX_POINTS  400

/* The largest a well-formed file can be. Derived, never typed. */
#define TRACK_MAP_MAX_FILE    (TRACK_MAP_HEADER + TRACK_MAP_MAX_POINTS * 8)

/* Where the dash keeps its circuits. Here, with the format, because FOUR
 * places open these files — the HTTP upload, the HTTP list/read/delete, the
 * serial upload and the widget's loader — and a path spelled four times is
 * three chances to disagree. A widget looking in the wrong directory reports
 * "no such track asset" for a track that is plainly in the list. */
#define TRACK_MAP_LFS_DIR     "/lfs/tracks"
#define TRACK_MAP_EXT         ".rdmtrk"

#define TRACK_MAP_FLAG_CLOSED 0x01u

typedef enum {
    TRACK_MAP_OK = 0,
    TRACK_MAP_ERR_SHORT,      /* buffer smaller than the header, or truncated */
    TRACK_MAP_ERR_MAGIC,
    TRACK_MAP_ERR_VERSION,
    TRACK_MAP_ERR_POINTS,     /* 0, or more than TRACK_MAP_MAX_POINTS */
    TRACK_MAP_ERR_RANGE,      /* a coordinate outside ±90 / ±180 */
} track_map_err_t;

typedef struct {
    char     name[TRACK_MAP_NAME_LEN];  /* always NUL-terminated after parse */
    uint8_t  version;
    uint8_t  flags;
    int32_t  lat0_1e7, lon0_1e7;
    uint16_t n_points;
    /* Borrowed pointer into the caller's buffer — NOT copied. The caller owns
     * the bytes and must keep them alive for as long as it reads points. */
    const uint8_t *points;
} track_map_file_t;

/* Parse and validate a header. Does not copy the points. */
track_map_err_t track_map_parse(const uint8_t *buf, size_t len, track_map_file_t *out);

/* Point i as degrees ×1e7. Bounds-checked; returns false past the end. */
bool track_map_point(const track_map_file_t *f, uint16_t i, int32_t *lat_1e7, int32_t *lon_1e7);

/* The tangent-plane fit: metres-per-degree at this origin, and the scale and
 * offset that put the whole shape inside a w×h box with `margin_px` to spare.
 *
 * Kept as an explicit struct rather than baked into the widget so the same
 * numbers can drive BOTH the static outline and the live position dot. Two
 * projections would drift, and the dot would sit next to the track instead of
 * on it. */
typedef struct {
    double lat0, lon0;      /* degrees */
    double kx, ky;          /* metres per degree, east and north */
    double px_per_m;
    double cx, cy;          /* box centre, in the caller's pixel space */
    double rot_sin, rot_cos;
} track_map_proj_t;

/* Build a projection that fits every point of `f` into w×h.
 * `rot_deg` rotates the shape clockwise on screen (0 = north up). */
void track_map_fit(const track_map_file_t *f, int w, int h, int margin_px,
                   double rot_deg, track_map_proj_t *out);

/* Project one coordinate to pixels, relative to the same origin the fit used.
 * Works for any lat/lon, not just points in the file — this is what the live
 * position dot calls. */
void track_map_project(const track_map_proj_t *p, int32_t lat_1e7, int32_t lon_1e7,
                       double *px, double *py);

#ifdef __cplusplus
}
#endif
#endif /* TRACK_MAP_GEO_H */
