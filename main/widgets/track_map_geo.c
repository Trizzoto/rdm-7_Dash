#include "widgets/track_map_geo.h"

#include <math.h>
#include <string.h>

/* Little-endian reads done byte-at-a-time rather than by casting the buffer to
 * an int32_t*. The point array IS 4-aligned by the format's reserved pad, but
 * the buffer the caller hands us need not be — a file read into an arbitrary
 * heap offset would fault on the S3 for an aligned-load instruction. Byte
 * assembly costs nothing here and cannot trap. */
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t rd_i32(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)v;   /* two's complement on every target we build for */
}

track_map_err_t track_map_parse(const uint8_t *buf, size_t len, track_map_file_t *out) {
    if (!buf || !out || len < TRACK_MAP_HEADER)
        return TRACK_MAP_ERR_SHORT;
    if (memcmp(buf, TRACK_MAP_MAGIC, TRACK_MAP_MAGIC_LEN) != 0)
        return TRACK_MAP_ERR_MAGIC;

    uint8_t version = buf[6];
    if (version != TRACK_MAP_VERSION)
        return TRACK_MAP_ERR_VERSION;

    uint16_t n = rd_u16(buf + 48);
    if (n < 2 || n > TRACK_MAP_MAX_POINTS)
        return TRACK_MAP_ERR_POINTS;

    /* Overflow-safe: n is bounded above by TRACK_MAP_MAX_POINTS, so n*8 cannot
     * wrap, and the comparison is done in size_t. */
    if (len < (size_t)TRACK_MAP_HEADER + (size_t)n * 8u)
        return TRACK_MAP_ERR_SHORT;

    memset(out, 0, sizeof(*out));
    out->version  = version;
    out->flags    = buf[7];
    /* Copy 31 bytes and force the terminator: a name that filled the field
     * exactly would otherwise run into whatever follows it in the struct. */
    memcpy(out->name, buf + 8, TRACK_MAP_NAME_LEN - 1);
    out->name[TRACK_MAP_NAME_LEN - 1] = '\0';
    out->lat0_1e7 = rd_i32(buf + 40);
    out->lon0_1e7 = rd_i32(buf + 44);
    out->n_points = n;
    out->points   = buf + TRACK_MAP_HEADER;

    /* Range-check the origin AND every point. A single corrupt coordinate is
     * enough to blow the bounding box out to the whole planet, which collapses
     * px_per_m to near zero and draws the entire track as one dot in the
     * middle of the widget — a failure that looks like "the shape is missing"
     * rather than "the file is bad". */
    if (out->lat0_1e7 < -900000000 || out->lat0_1e7 > 900000000 ||
        out->lon0_1e7 < -1800000000 || out->lon0_1e7 > 1800000000)
        return TRACK_MAP_ERR_RANGE;
    for (uint16_t i = 0; i < n; i++) {
        int32_t la = rd_i32(out->points + (size_t)i * 8);
        int32_t lo = rd_i32(out->points + (size_t)i * 8 + 4);
        if (la < -900000000 || la > 900000000 || lo < -1800000000 || lo > 1800000000)
            return TRACK_MAP_ERR_RANGE;
    }
    return TRACK_MAP_OK;
}

bool track_map_point(const track_map_file_t *f, uint16_t i, int32_t *lat_1e7, int32_t *lon_1e7) {
    if (!f || !f->points || i >= f->n_points)
        return false;
    if (lat_1e7) *lat_1e7 = rd_i32(f->points + (size_t)i * 8);
    if (lon_1e7) *lon_1e7 = rd_i32(f->points + (size_t)i * 8 + 4);
    return true;
}

/* Metres per degree on the local tangent plane. Identical constants to
 * gpToLocalM in Studio, deliberately — the shape is simplified there against
 * a metre tolerance, and if the two planes disagreed the tolerance would mean
 * something different on each side. */
static void plane_constants(double lat0_deg, double *kx, double *ky) {
    const double D = 3.14159265358979323846 / 180.0;
    *kx = cos(lat0_deg * D) * 111320.0;
    *ky = 110540.0;
}

void track_map_fit(const track_map_file_t *f, int w, int h, int margin_px,
                   double rot_deg, track_map_proj_t *out) {
    if (!f || !out) return;
    memset(out, 0, sizeof(*out));

    out->lat0 = f->lat0_1e7 / 1e7;
    out->lon0 = f->lon0_1e7 / 1e7;
    plane_constants(out->lat0, &out->kx, &out->ky);

    const double D = 3.14159265358979323846 / 180.0;
    out->rot_sin = sin(rot_deg * D);
    out->rot_cos = cos(rot_deg * D);

    out->cx = w / 2.0;
    out->cy = h / 2.0;

    /* Bounding box AFTER rotation — rotating first and fitting second is what
     * lets a diagonal circuit fill the box instead of being scaled down to the
     * diagonal of its own unrotated extent. */
    double minx = 0, maxx = 0, miny = 0, maxy = 0;
    bool first = true;
    for (uint16_t i = 0; i < f->n_points; i++) {
        int32_t la, lo;
        if (!track_map_point(f, i, &la, &lo)) break;
        double ex = (lo / 1e7 - out->lon0) * out->kx;
        double ny = (la / 1e7 - out->lat0) * out->ky;
        double rx = ex * out->rot_cos - ny * out->rot_sin;
        double ry = ex * out->rot_sin + ny * out->rot_cos;
        if (first) { minx = maxx = rx; miny = maxy = ry; first = false; }
        else {
            if (rx < minx) minx = rx;
            if (rx > maxx) maxx = rx;
            if (ry < miny) miny = ry;
            if (ry > maxy) maxy = ry;
        }
    }

    double spanx = maxx - minx, spany = maxy - miny;
    double availw = (double)w - 2.0 * margin_px;
    double availh = (double)h - 2.0 * margin_px;
    if (availw < 1) availw = 1;
    if (availh < 1) availh = 1;

    /* A degenerate span (every point identical, or a single straight line seen
     * end-on) would divide by zero and produce inf pixels, which LVGL turns
     * into a coordinate wrap rather than an error. Fall back to a scale that
     * simply centres it. */
    double sx = spanx > 1e-6 ? availw / spanx : 1.0;
    double sy = spany > 1e-6 ? availh / spany : 1.0;
    out->px_per_m = sx < sy ? sx : sy;
    if (!(out->px_per_m > 0.0) || !isfinite(out->px_per_m))
        out->px_per_m = 1.0;

    /* Centre on the shape's own middle, not on the origin: the origin is the
     * bbox centre in UNROTATED metres, which is not the centre of the rotated
     * extent unless the rotation is a multiple of 90°. */
    out->cx = w / 2.0 - (minx + maxx) / 2.0 * out->px_per_m;
    out->cy = h / 2.0 + (miny + maxy) / 2.0 * out->px_per_m;
}

void track_map_project(const track_map_proj_t *p, int32_t lat_1e7, int32_t lon_1e7,
                       double *px, double *py) {
    if (!p) return;
    double ex = (lon_1e7 / 1e7 - p->lon0) * p->kx;
    double ny = (lat_1e7 / 1e7 - p->lat0) * p->ky;
    double rx = ex * p->rot_cos - ny * p->rot_sin;
    double ry = ex * p->rot_sin + ny * p->rot_cos;
    /* Y is negated: north is up on a map and DOWN in screen coordinates.
     * Forgetting this draws the circuit mirrored, which is the single easiest
     * way to ship a track map that looks plausible and is wrong. */
    if (px) *px = p->cx + rx * p->px_per_m;
    if (py) *py = p->cy - ry * p->px_per_m;
}
