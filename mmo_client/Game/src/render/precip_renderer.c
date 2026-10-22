// Rain, snow, wind ribbons and (switched off) blown dust round the camera.

#include "render/gl_loader.h"
#include "render/precip_renderer.h"
#include "render/cover_map.h"
#include "render/weather_map.h"
#include "core/client_log.h"
#include "world/weather_now.h"
#include "world/wind_gust.h"
#include "world/world_lighting.h"

#include <math.h>

// Drops a downpour or a blizzard is drawn with; the weather map thins them to
// the fall at each spot
#define RAIN_MAX 48000
#define SNOW_MAX 26000
#define DEBRIS_MAX 4500

// The box the drops fall in: half its width, and its height, world units. Twice
// the half-width is what weather_now's wind_travel wraps at.
#define BOX_HALF 1100.0f
#define BOX_HEIGHT 1300.0f

// Where the box starts, a fixed height of the world so the rain does not rise
// and sink with the ground the player stands on (below the ground it is hidden)
#define BOX_BOTTOM -120.0f

// Rain is blown sideways this many times as fast as the wind blows, so a gale
// slants it hard and a storm's blast drives it nearly flat
#define RAIN_WIND_GAIN 2.5f

// Fall speeds, world units a second
#define RAIN_SPEED 750.0f
#define SNOW_SPEED 70.0f

// A streak's length and a flake's radius, world units
#define RAIN_LENGTH 14.0f
#define FLAKE_RADIUS 2.0f

// Blown dust and leaves: the length of a streak of one, and the wind level they start at
#define DEBRIS_LENGTH 5.0f
#define DEBRIS_FROM_LEVEL 0.9f
#define DEBRIS_FULL_LEVEL 2.2f
// 1 draws blown dust and leaves in a wind; 0 leaves them out (code stays)
#define DEBRIS_ON 0

// Wind ribbons: pale curved lines sweeping along in gusts. Most there are,
// segments each, half the box width round the focus, and the wind levels
#define RIBBON_MAX 70
#define RIBBON_SEGMENTS 20
#define RIBBON_HALF 650.0f
#define RIBBON_FROM_LEVEL 0.9f
#define RIBBON_FULL_LEVEL 2.6f
// A ribbon's length, flight and height up, world units: least, and more
#define RIBBON_LENGTH_MIN 110.0
#define RIBBON_LENGTH_MORE 150.0
#define RIBBON_TRAVEL_MIN 320.0
#define RIBBON_TRAVEL_MORE 200.0
#define RIBBON_LIFT_MIN 18.0
#define RIBBON_LIFT_MORE 70.0
// Seconds before a ribbon comes round (least, more), and the share it lives
#define RIBBON_PERIOD_MIN 2.4
#define RIBBON_PERIOD_MORE 2.2
#define RIBBON_LIFE 0.55
// A ribbon's width at its middle in pixels, and its opacity
#define RIBBON_WIDTH 5.0f
#define RIBBON_ALPHA 0.55f

// Below this the fall is too light to draw at all
#define FALL_DRAWN_MIN 0.01f

// Numbers into GLSL, so the shaders and the tuning agree
#define STR_(x) #x
#define STR(x) STR_(x)

// Rain splashes: how many drops get one, how far out they are, how long one lasts
#define SPLASH_MAX RAIN_MAX
#define SPLASH_HALF BOX_HALF
#define SPLASH_LIFE_S 0.45

typedef enum {
	KIND_RAIN = 0,
	KIND_SNOW = 1,
	KIND_DEBRIS = 2
} Kind;

static const char* VERSION_SRC = "#version 330 core\n";

// The fall of rain and snow, and the dust the wind carries
static const char* VERTEX_SRC =
	"uniform mat4 u_viewproj;\n"
	"uniform vec3 u_centre;\n"
	"uniform vec3 u_box;\n"
	"uniform vec3 u_motion;\n"
	"uniform vec2 u_travel;\n"
	"uniform float u_time;\n"
	"uniform vec2 u_viewport;\n"
	"uniform vec2 u_pscale;\n"
	"uniform float u_size;\n"
	"uniform float u_kind;\n"
	"uniform float u_amount;\n"
	"out vec2 v_uv;\n"
	"out float v_fade;\n"
	"out float v_tone;\n"
	"\n"
	"const vec2 CORNERS[6] = vec2[6](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),\n"
	"                                vec2(-1.0, 0.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));\n"
	"\n"
	"uint hash_u(uint x) {\n"
	"    x ^= x >> 16u; x *= 0x7feb352du;\n"
	"    x ^= x >> 15u; x *= 0x846ca68bu;\n"
	"    return x ^ (x >> 16u);\n"
	"}\n"
	"\n"
	"vec3 rand3(uint id) {\n"
	"    return vec3(hash_u(id * 3u + 1u), hash_u(id * 3u + 2u), hash_u(id * 3u + 3u)) / 4294967295.0;\n"
	"}\n"
	"\n"
	"// Where a repeating lattice of drops falls inside the box round the centre\n"
	"float wrap_into(float v, float centre, float half_width) {\n"
	"    float low = centre - half_width;\n"
	"    return low + mod(v - low, 2.0 * half_width);\n"
	"}\n"
	"\n"
	"void hide() {\n"
	"    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    int id = gl_VertexID / 6;\n"
	"    int corner = gl_VertexID % 6;\n"
	"    vec2 q = CORNERS[corner];\n"
	"    vec3 r = rand3(uint(id));\n"
	"    vec3 v = rand3(uint(id) + 77777u);\n"
	"    float keep = rand3(uint(id) + 31337u).x;\n"
	"    float half_width = u_box.x;\n"
	"    bool debris = u_kind > 1.5;\n"
	"    bool snow = u_kind > 0.5 && !debris;\n"
	"    vec3 p;\n"
	"    float local = 1.0;\n"
	"    float clip = 0.0;\n"
	"    float phase = 0.5;\n"
	"    v_tone = v.y;\n"
	"    if (debris) {\n"
	"        // Carried along the ground by the wind, bouncing as it goes\n"
	"        vec2 xz = vec2(wrap_into(r.x * 2.0 * half_width + u_travel.x * (0.7 + 0.5 * v.x), u_centre.x, half_width),\n"
	"                       wrap_into(r.z * 2.0 * half_width + u_travel.y * (0.7 + 0.5 * v.x), u_centre.z, half_width));\n"
	"        float top = cover_height(xz);\n"
	"        if (top < -500.0 || keep > u_amount) { hide(); return; }\n"
	"        p = vec3(xz.x, top + 2.0 + 12.0 * abs(sin(u_time * (1.2 + 2.0 * v.y) + v.z * 40.0)), xz.y);\n"
	"    } else {\n"
	"        float speed = u_motion.x * (0.85 + 0.3 * v.x);\n"
	"        phase = fract(r.y - u_time * speed / u_box.z);\n"
	"        float since = (1.0 - phase) * u_box.z / speed;\n"
	"        p.x = wrap_into(r.x * 2.0 * half_width + u_motion.y * since, u_centre.x, half_width);\n"
	"        p.z = wrap_into(r.z * 2.0 * half_width + u_motion.z * since, u_centre.z, half_width);\n"
	"        p.y = u_box.y + phase * u_box.z;\n"
	"        if (snow) {\n"
	"            float ph = v.y * 40.0;\n"
	"            p.x += sin(u_time * 0.8 + ph) * 12.0;\n"
	"            p.z += cos(u_time * 0.6 + ph * 1.3) * 12.0;\n"
	"        }\n"
	"        // Only where a patch of weather is falling; thicker where it falls harder\n"
	"        vec4 w = weather_at(p.xz);\n"
	"        local = snow ? w.z : w.y;\n"
	"        if (local <= 0.0 || keep > pow(local, 0.6)) { hide(); return; }\n"
	"        // Flakes vanish at the surface; a streak of rain is cut off at it, below\n"
	"        float top = cover_height(p.xz);\n"
	"        if (snow && p.y < top + 1.0) { hide(); return; }\n"
	"        clip = snow ? 0.0 : max(top - p.y, 0.0);\n"
	"    }\n"
	"    float edge = max(abs(p.x - u_centre.x), abs(p.z - u_centre.z)) / half_width;\n"
	"    v_fade = (1.0 - smoothstep(0.6, 1.0, edge)) * (debris ? 1.0 : smoothstep(0.0, 0.04, phase)\n"
	"           * (1.0 - smoothstep(0.92, 1.0, phase))) * (debris ? 1.0 : 0.55 + 0.45 * local);\n"
	"    v_uv = q;\n"
	"    vec4 head = u_viewproj * vec4(p, 1.0);\n"
	"    if (head.w < 1.0) { hide(); return; }\n"
	"    if (snow) {\n"
	"        vec2 r_ndc = max(u_size * (0.7 + 0.5 * local) * u_pscale / head.w, vec2(3.0) / u_viewport);\n"
	"        vec2 round_q = vec2(q.x, q.y * 2.0 - 1.0);\n"
	"        vec2 ndc = head.xy / head.w + round_q * r_ndc;\n"
	"        v_uv = round_q;\n"
	"        gl_Position = vec4(ndc * head.w, head.z, head.w);\n"
	"        return;\n"
	"    }\n"
	"    vec3 way;\n"
	"    float length_units;\n"
	"    float thick;\n"
	"    if (debris) {\n"
	"        way = normalize(vec3(-u_motion.y, 0.1, -u_motion.z) + vec3(0.001));\n"
	"        length_units = u_size * (0.8 + 0.8 * v.z);\n"
	"        thick = 3.2;\n"
	"    } else {\n"
	"        way = normalize(vec3(-u_motion.y, u_motion.x * (0.85 + 0.3 * v.x), -u_motion.z));\n"
	"        length_units = u_size * (0.6 + 0.6 * local) * (0.7 + 0.6 * v.z);\n"
	"        thick = 1.3 + 1.0 * local;\n"
	"        // Its head is under the surface: what is left of the streak ends on it\n"
	"        if (clip > 0.0) {\n"
	"            float cut = clip / way.y;\n"
	"            if (cut >= length_units) { hide(); return; }\n"
	"            p += way * cut;\n"
	"            length_units -= cut;\n"
	"            head = u_viewproj * vec4(p, 1.0);\n"
	"            if (head.w < 1.0) { hide(); return; }\n"
	"        }\n"
	"    }\n"
	"    vec4 tail = u_viewproj * vec4(p + way * length_units, 1.0);\n"
	"    if (tail.w < 1.0) { hide(); return; }\n"
	"    vec4 end = q.y > 0.5 ? tail : head;\n"
	"    vec2 s0 = head.xy / head.w * u_viewport * 0.5;\n"
	"    vec2 s1 = tail.xy / tail.w * u_viewport * 0.5;\n"
	"    vec2 along = s1 - s0;\n"
	"    along = length(along) > 0.001 ? normalize(along) : vec2(0.0, 1.0);\n"
	"    vec2 side = vec2(-along.y, along.x);\n"
	"    vec2 ndc = end.xy / end.w + side * q.x * thick / u_viewport;\n"
	"    gl_Position = vec4(ndc * end.w, end.z, end.w);\n"
	"}\n";

// A splash is a ring where one of the rain's own drops lands: the drop's path
// is followed down to the highest surface under it, so the ring is on the roof
// or the ground the streak ends on, at the moment it ends.
static const char* SPLASH_VERTEX_SRC =
	"uniform mat4 u_viewproj;\n"
	"uniform vec2 u_centre;\n"
	"uniform vec3 u_box;\n"
	"uniform vec3 u_motion;\n"
	"uniform vec2 u_wind_rate;\n"
	"uniform float u_half;\n"
	"uniform float u_time;\n"
	"out vec2 v_uv;\n"
	"out float v_age;\n"
	"out float v_fade;\n"
	"\n"
	"const vec2 CORNERS[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),\n"
	"                                vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));\n"
	"\n"
	"uint hash_u(uint x) {\n"
	"    x ^= x >> 16u; x *= 0x7feb352du;\n"
	"    x ^= x >> 15u; x *= 0x846ca68bu;\n"
	"    return x ^ (x >> 16u);\n"
	"}\n"
	"\n"
	"vec3 rand3(uint id) {\n"
	"    return vec3(hash_u(id * 3u + 1u), hash_u(id * 3u + 2u), hash_u(id * 3u + 3u)) / 4294967295.0;\n"
	"}\n"
	"\n"
	"float wrap_into(float v, float centre, float half_width) {\n"
	"    float low = centre - half_width;\n"
	"    return low + mod(v - low, 2.0 * half_width);\n"
	"}\n"
	"\n"
	"// The drop's place in the box at a height: the same line the streak falls along\n"
	"vec3 g_lattice;\n"
	"float g_speed;\n"
	"vec2 g_wind;\n"
	"vec2 drop_at(float h) {\n"
	"    float since = (u_box.y + u_box.z - h) / g_speed;\n"
	"    return vec2(wrap_into(g_lattice.x * 2.0 * u_half + g_wind.x * since, u_centre.x, u_half),\n"
	"                wrap_into(g_lattice.z * 2.0 * u_half + g_wind.y * since, u_centre.y, u_half));\n"
	"}\n"
	"\n"
	"void hide() {\n"
	"    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    uint id = uint(gl_VertexID / 6);\n"
	"    vec2 c = CORNERS[gl_VertexID % 6];\n"
	"    // The same numbers the rain makes this drop from\n"
	"    g_lattice = rand3(id);\n"
	"    vec3 v = rand3(id + 77777u);\n"
	"    float keep = rand3(id + 31337u).x;\n"
	"    g_speed = u_motion.x * (0.85 + 0.3 * v.x);\n"
	"\n"
	"    float k = g_speed / u_box.z;\n"
	"    float phase = fract(g_lattice.y - u_time * k);\n"
	"    // Down the slanted path to the surface it meets: each guess of the height\n"
	"    // moves where the drop is, which moves the height under it\n"
	"    g_wind = u_motion.yz;\n"
	"    float h = 0.0;\n"
	"    for (int i = 0; i < 3; i++)\n"
	"        h = cover_height(drop_at(h));\n"
	"    // The wind as it blew when the drop landed, so a ring stays where it fell\n"
	"    // while the wind changes. How long ago that was needs the landing spot.\n"
	"    float ago = min(fract((cover_height(drop_at(h)) - u_box.y) / u_box.z - phase) / k, " STR(SPLASH_LIFE_S) ");\n"
	"    g_wind = u_motion.yz - u_wind_rate * ago;\n"
	"    h = 0.0;\n"
	"    for (int i = 0; i < 3; i++)\n"
	"        h = cover_height(drop_at(h));\n"
	"    vec2 xz = drop_at(h);\n"
	"    float top = cover_height(xz);\n"
	"    if (top < -500.0) { hide(); return; }\n"
	"\n"
	"    // How long ago the drop reached that height\n"
	"    float phase_hit = (top - u_box.y) / u_box.z;\n"
	"    float age = fract(phase_hit - phase) / k / " STR(SPLASH_LIFE_S) ";\n"
	"    float rain = weather_at(xz).y;\n"
	"    if (age >= 1.0 || rain <= 0.0 || keep > pow(rain, 0.6)) { hide(); return; }\n"
	"\n"
	"    float radius = (1.2 + age * 4.5) * (0.7 + 0.6 * v.y);\n"
	"    vec3 world = vec3(xz.x + c.x * radius / 0.8, top + 0.6, xz.y + c.y * radius / 0.8);\n"
	"    v_uv = c;\n"
	"    v_age = age;\n"
	"    v_fade = 1.0 - smoothstep(0.6, 1.0, max(abs(xz.x - u_centre.x), abs(xz.y - u_centre.y)) / u_half);\n"
	"    gl_Position = u_viewproj * vec4(world, 1.0);\n"
	"}\n";

static const char* SPLASH_FRAGMENT_SRC =
	"in vec2 v_uv;\n"
	"in float v_age;\n"
	"in float v_fade;\n"
	"uniform vec3 u_tint;\n"
	"uniform float u_alpha;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    float d = length(v_uv);\n"
	"    float ring = (1.0 - smoothstep(0.0, 0.16, abs(d - 0.8))) * (1.0 - v_age);\n"
	"    float spot = (1.0 - smoothstep(0.0, 0.3, d)) * (1.0 - smoothstep(0.0, 0.25, v_age));\n"
	"    float a = (ring * 0.8 + spot * 0.9) * u_alpha * v_fade;\n"
	"    if (a < 0.01) discard;\n"
	"    frag = vec4(u_tint, a);\n"
	"}\n";

static const char* FRAGMENT_SRC =
	"in vec2 v_uv;\n"
	"in float v_fade;\n"
	"in float v_tone;\n"
	"uniform vec3 u_tint;\n"
	"uniform float u_alpha;\n"
	"uniform float u_kind;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    float a;\n"
	"    vec3 colour = u_tint;\n"
	"    if (u_kind > 1.5) {\n"
	"        // Dust and dry leaves: earth and olive, as bright as the day\n"
	"        float lum = dot(u_tint, vec3(0.333)) * 1.4;\n"
	"        colour = mix(vec3(0.40, 0.30, 0.16), vec3(0.30, 0.38, 0.12), v_tone) * lum;\n"
	"        a = (1.0 - v_uv.x * v_uv.x) * (1.0 - 0.5 * v_uv.y);\n"
	"    } else if (u_kind > 0.5) {\n"
	"        a = 1.0 - smoothstep(0.45, 1.0, length(v_uv));\n"
	"    } else {\n"
	"        a = (1.0 - v_uv.x * v_uv.x) * (1.0 - 0.7 * v_uv.y);\n"
	"    }\n"
	"    frag = vec4(colour, a * u_alpha * v_fade);\n"
	"}\n";

// A wind ribbon: a curve along the wind, a strip narrowing at both ends
static const char* RIBBON_VERTEX_SRC =
	"uniform mat4 u_viewproj;\n"
	"uniform vec2 u_centre;\n"
	"uniform vec2 u_dir;\n"
	"uniform float u_half;\n"
	"uniform float u_time;\n"
	"uniform float u_amount;\n"
	"uniform float u_size;\n"
	"uniform vec2 u_viewport;\n"
	"out vec2 v_uv;\n"
	"out float v_fade;\n"
	"\n"
	"const int SEGMENTS = " STR(RIBBON_SEGMENTS) ";\n"
	"const vec2 CORNERS[6] = vec2[6](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),\n"
	"                                vec2(-1.0, 0.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));\n"
	"\n"
	"uint hash_u(uint x) {\n"
	"    x ^= x >> 16u; x *= 0x7feb352du;\n"
	"    x ^= x >> 15u; x *= 0x846ca68bu;\n"
	"    return x ^ (x >> 16u);\n"
	"}\n"
	"\n"
	"vec3 rand3(uint id) {\n"
	"    return vec3(hash_u(id * 3u + 1u), hash_u(id * 3u + 2u), hash_u(id * 3u + 3u)) / 4294967295.0;\n"
	"}\n"
	"\n"
	"float wrap_into(float v, float centre, float half_width) {\n"
	"    float low = centre - half_width;\n"
	"    return low + mod(v - low, 2.0 * half_width);\n"
	"}\n"
	"\n"
	"// The ribbon being drawn: where it starts, which way it goes, its shape\n"
	"vec3 g_base;\n"
	"vec3 g_dir;\n"
	"vec3 g_side;\n"
	"vec3 g_loop;\n"
	"float g_len;\n"
	"float g_sway;\n"
	"float g_rise;\n"
	"float g_phase;\n"
	"float g_curl;\n"
	"float g_flown;\n"
	"\n"
	"// A point along the ribbon, s from 0 to 1: a wave to the side, a hump up, and\n"
	"// for some a loop at the far end\n"
	"vec3 ribbon_at(float s) {\n"
	"    float a = g_curl * 6.2831853 * smoothstep(0.5, 1.0, s);\n"
	"    float radius = g_len * 0.11 * g_curl;\n"
	"    float along = g_len * s + radius * sin(a) + g_flown;\n"
	"    float lift = radius * (1.0 - cos(a));\n"
	"    float side = g_sway * sin(6.2831853 * 1.25 * s + g_phase);\n"
	"    float up = g_rise * sin(3.1415927 * s);\n"
	"    return g_base + g_dir * along + g_side * side + g_loop * lift + vec3(0.0, up, 0.0);\n"
	"}\n"
	"\n"
	"void hide() {\n"
	"    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    int id = gl_VertexID / (6 * SEGMENTS);\n"
	"    int seg = (gl_VertexID / 6) % SEGMENTS;\n"
	"    vec2 q = CORNERS[gl_VertexID % 6];\n"
	"    vec3 r0 = rand3(uint(id));\n"
	"    float period = " STR(RIBBON_PERIOD_MIN) " + " STR(RIBBON_PERIOD_MORE) " * r0.x;\n"
	"    float cycles = u_time / period + r0.y * 64.0;\n"
	"    float cycle = floor(cycles);\n"
	"    float age = (cycles - cycle) / " STR(RIBBON_LIFE) ";\n"
	"    vec3 r = rand3(uint(id) * 7919u + uint(cycle));\n"
	"    vec3 r2 = rand3(uint(id) * 104729u + uint(cycle) + 17u);\n"
	"    if (age > 1.0 || r2.x > u_amount) { hide(); return; }\n"
	"\n"
	"    vec2 spot = vec2(wrap_into(r.x * 2.0 * u_half, u_centre.x, u_half),\n"
	"                     wrap_into(r.y * 2.0 * u_half, u_centre.y, u_half));\n"
	"    float top = cover_height(spot);\n"
	"    if (top < -500.0) { hide(); return; }\n"
	"\n"
	"    g_dir = normalize(vec3(u_dir.x, 0.0, u_dir.y));\n"
	"    g_side = vec3(-g_dir.z, 0.0, g_dir.x);\n"
	"    float roll = (r2.y - 0.5) * 2.4;\n"
	"    g_loop = vec3(0.0, cos(roll), 0.0) + g_side * sin(roll);\n"
	"    g_len = " STR(RIBBON_LENGTH_MIN) " + " STR(RIBBON_LENGTH_MORE) " * r.z;\n"
	"    g_sway = g_len * (0.05 + 0.12 * r2.z);\n"
	"    g_rise = g_len * (0.04 + 0.10 * r0.z);\n"
	"    g_phase = r.x * 6.2831853;\n"
	"    g_curl = r0.z > 0.45 ? 0.55 + 0.45 * r2.y : 0.0;\n"
	"    g_flown = (" STR(RIBBON_TRAVEL_MIN) " + " STR(RIBBON_TRAVEL_MORE) " * r2.z) * age;\n"
	"    g_base = vec3(spot.x, top + " STR(RIBBON_LIFT_MIN) " + " STR(RIBBON_LIFT_MORE) " * r2.y, spot.y);\n"
	"\n"
	"    // Only while a gust is over it, and not at the edge of the box or of its life\n"
	"    vec3 mid = ribbon_at(0.5);\n"
	"    float gust = smoothstep(0.3, 0.75, wind_gust(mid.xz, u_dir, u_time));\n"
	"    float edge = max(abs(mid.x - u_centre.x), abs(mid.z - u_centre.y)) / u_half;\n"
	"    float s = (float(seg) + q.y) / float(SEGMENTS);\n"
	"    v_fade = gust * (1.0 - smoothstep(0.6, 1.0, edge)) * smoothstep(0.0, 0.2, age) *\n"
	"             (1.0 - smoothstep(0.7, 1.0, age)) * smoothstep(0.0, 0.15, s) *\n"
	"             (1.0 - smoothstep(0.7, 1.0, s));\n"
	"\n"
	"    vec4 c = u_viewproj * vec4(ribbon_at(s), 1.0);\n"
	"    vec4 ca = u_viewproj * vec4(ribbon_at(max(s - 0.03, 0.0)), 1.0);\n"
	"    vec4 cb = u_viewproj * vec4(ribbon_at(min(s + 0.03, 1.0)), 1.0);\n"
	"    if (c.w < 1.0 || ca.w < 1.0 || cb.w < 1.0) { hide(); return; }\n"
	"    vec2 along = cb.xy / cb.w * u_viewport * 0.5 - ca.xy / ca.w * u_viewport * 0.5;\n"
	"    along = length(along) > 0.001 ? normalize(along) : vec2(0.0, 1.0);\n"
	"    vec2 side = vec2(-along.y, along.x);\n"
	"    float width = u_size * sqrt(sin(3.1415927 * s));\n"
	"    vec2 ndc = c.xy / c.w + side * q.x * width / u_viewport;\n"
	"    v_uv = vec2(q.x, s);\n"
	"    gl_Position = vec4(ndc * c.w, c.z, c.w);\n"
	"}\n";

static const char* RIBBON_FRAGMENT_SRC =
	"in vec2 v_uv;\n"
	"in float v_fade;\n"
	"uniform vec3 u_tint;\n"
	"uniform float u_alpha;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    float a = (1.0 - smoothstep(0.35, 1.0, abs(v_uv.x))) * u_alpha * v_fade;\n"
	"    if (a < 0.01) discard;\n"
	"    frag = vec4(u_tint, a);\n"
	"}\n";

// The programs: rain, snow and dust, the splashes, and the wind ribbons
typedef struct {
	GLuint program;
	cover_uniforms cover;
	weather_map_uniforms map;
	GLint viewproj, centre, box, motion, travel, time, viewport, pscale, dir, wind_rate;
	GLint size, kind, amount, tint, alpha, half_width;
} precip_program;

static precip_program g_fall;
static precip_program g_splash;
static precip_program g_ribbon;
static GLuint g_vao;

static GLuint compile(GLenum type, const char* const* parts, int count) {
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, count, parts, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) {
		char log[1024];

		glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
		CLOG_ERROR("[PRECIP] %s shader failed to compile: %s",
		           type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

// Build a program from a vertex body, which gets the cover map's and the
// weather map's GLSL, and a fragment body. Uniforms are looked up by name,
// whichever the program has.
static int build(precip_program* out, const char* vertex_body, const char* fragment_body) {
	const char* const vertex_parts[] = { VERSION_SRC, cover_map_glsl(), weather_map_glsl(),
	                                     wind_gust_glsl(), vertex_body };
	const char* const fragment_parts[] = { VERSION_SRC, fragment_body };
	GLuint vs = compile(GL_VERTEX_SHADER, vertex_parts, 5);
	GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_parts, 2);
	GLint ok = 0;

	if(!vs || !fs) {
		if(vs)
			glDeleteShader(vs);
		if(fs)
			glDeleteShader(fs);
		return 0;
	}
	out->program = glCreateProgram();
	glAttachShader(out->program, vs);
	glAttachShader(out->program, fs);
	glLinkProgram(out->program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(out->program, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024];

		glGetProgramInfoLog(out->program, (GLsizei)sizeof(log), NULL, log);
		CLOG_ERROR("[PRECIP] Shader program failed to link: %s", log);
		glDeleteProgram(out->program);
		out->program = 0;
		return 0;
	}
	out->viewproj = glGetUniformLocation(out->program, "u_viewproj");
	out->centre = glGetUniformLocation(out->program, "u_centre");
	out->box = glGetUniformLocation(out->program, "u_box");
	out->motion = glGetUniformLocation(out->program, "u_motion");
	out->travel = glGetUniformLocation(out->program, "u_travel");
	out->time = glGetUniformLocation(out->program, "u_time");
	out->viewport = glGetUniformLocation(out->program, "u_viewport");
	out->pscale = glGetUniformLocation(out->program, "u_pscale");
	out->dir = glGetUniformLocation(out->program, "u_dir");
	out->wind_rate = glGetUniformLocation(out->program, "u_wind_rate");
	out->size = glGetUniformLocation(out->program, "u_size");
	out->kind = glGetUniformLocation(out->program, "u_kind");
	out->amount = glGetUniformLocation(out->program, "u_amount");
	out->tint = glGetUniformLocation(out->program, "u_tint");
	out->alpha = glGetUniformLocation(out->program, "u_alpha");
	out->half_width = glGetUniformLocation(out->program, "u_half");
	cover_map_locate(out->program, &out->cover);
	weather_map_locate(out->program, &out->map);

	return 1;
}

int precip_renderer_init(void) {
	if(!build(&g_fall, VERTEX_SRC, FRAGMENT_SRC))
		return 0;
	// The splashes are a garnish: without them the rain still falls
	if(!build(&g_splash, SPLASH_VERTEX_SRC, SPLASH_FRAGMENT_SRC))
		CLOG_WARN("[PRECIP] Rain will fall without splashes");
	if(!build(&g_ribbon, RIBBON_VERTEX_SRC, RIBBON_FRAGMENT_SRC))
		CLOG_WARN("[PRECIP] The wind will blow without ribbons");
	glGenVertexArrays(1, &g_vao);
	CLOG_INFO("[PRECIP] Shader ready");

	return 1;
}

void precip_renderer_shutdown(void) {
	if(g_vao) {
		glDeleteVertexArrays(1, &g_vao);
		g_vao = 0;
	}
	if(g_fall.program)
		glDeleteProgram(g_fall.program);
	if(g_splash.program)
		glDeleteProgram(g_splash.program);
	if(g_ribbon.program)
		glDeleteProgram(g_ribbon.program);
	g_fall.program = 0;
	g_splash.program = 0;
	g_ribbon.program = 0;
}

// Rain and snow take the light of the day: dim at night, lit by lightning
static void tint_of(int snow, float flash, float out[3]) {
	const world_lighting* light = world_lighting_current();
	float bright = (light->ambient_up[0] + light->ambient_up[1] + light->ambient_up[2]) / 3.0f +
	               0.6f * (light->color[0] + light->color[1] + light->color[2]) / 3.0f;
	float level = bright * 1.5f;
	const float rain_color[3] = { 0.62f, 0.70f, 0.86f };
	const float snow_color[3] = { 0.95f, 0.97f, 1.0f };

	if(level < 0.15f)
		level = 0.15f;
	if(level > 1.0f)
		level = 1.0f;
	for(int i = 0; i < 3; i++) {
		out[i] = (snow ? snow_color[i] : rain_color[i]) * level + flash * 0.5f;
		if(out[i] > 1.0f)
			out[i] = 1.0f;
	}
}

static float smooth_step(float lo, float hi, float x) {
	float t = (x - lo) / (hi - lo);

	if(t < 0.0f)
		t = 0.0f;
	if(t > 1.0f)
		t = 1.0f;

	return t * t * (3.0f - 2.0f * t);
}

// Draw one kind of fall or flight: its drops, in a box about the focus
static void draw_fall(const CameraView* view, const weather_now* weather, float x, float y,
                      float h, Kind kind) {
	const precip_program* p = &g_fall;
	float tint[3];
	float speed = sqrtf(weather->wind[0] * weather->wind[0] + weather->wind[1] * weather->wind[1]);
	int count = kind == KIND_RAIN ? RAIN_MAX : (kind == KIND_SNOW ? SNOW_MAX : DEBRIS_MAX);
	float size = kind == KIND_RAIN ? RAIN_LENGTH : (kind == KIND_SNOW ? FLAKE_RADIUS : DEBRIS_LENGTH + speed * 0.05f);
	float amount = smooth_step(DEBRIS_FROM_LEVEL, DEBRIS_FULL_LEVEL, weather->wind_level);

	tint_of(kind == KIND_SNOW, weather->flash, tint);
	glUseProgram(p->program);
	cover_map_apply(&p->cover);
	weather_map_apply(&p->map, 0);
	glUniformMatrix4fv(p->viewproj, 1, GL_FALSE, view->viewproj);
	glUniform3f(p->centre, x, h, y);
	glUniform3f(p->box, BOX_HALF, BOX_BOTTOM, BOX_HEIGHT);
	glUniform3f(p->motion, kind == KIND_SNOW ? SNOW_SPEED : RAIN_SPEED,
	            weather->wind[0] * (kind == KIND_SNOW ? 1.0f : RAIN_WIND_GAIN),
	            weather->wind[1] * (kind == KIND_SNOW ? 1.0f : RAIN_WIND_GAIN));
	glUniform2f(p->travel, weather->wind_travel[0], weather->wind_travel[1]);
	glUniform1f(p->time, weather->time_s);
	glUniform2f(p->viewport, (float)view->viewport_w, (float)view->viewport_h);
	glUniform2f(p->pscale, view->proj[0], view->proj[5]);
	glUniform1f(p->size, size);
	glUniform1f(p->kind, (float)kind);
	glUniform1f(p->amount, amount);
	glUniform3f(p->tint, tint[0], tint[1], tint[2]);
	glUniform1f(p->alpha, kind == KIND_SNOW ? 0.85f : (kind == KIND_RAIN ? 0.75f : 0.85f));
	glDrawArrays(GL_TRIANGLES, 0, count * 6);
}

// How fast the wind has been changing, units a second a second, from the last
// half second of it. A ring needs it to stay where its drop fell.
#define WIND_HISTORY 8
#define WIND_SAMPLE_S 0.1f

static void wind_rate_of(const weather_now* weather, float out[2]) {
	static float t[WIND_HISTORY];
	static float wind[WIND_HISTORY][2];
	static int count;
	float span;

	// Time wrapped, or nothing drawn for a while: start the history again
	if(count > 0 && (weather->time_s < t[count - 1] || weather->time_s - t[count - 1] > 1.0f))
		count = 0;
	if(count == 0 || weather->time_s - t[count - 1] >= WIND_SAMPLE_S) {
		if(count == WIND_HISTORY) {
			for(int i = 1; i < WIND_HISTORY; i++) {
				t[i - 1] = t[i];
				wind[i - 1][0] = wind[i][0];
				wind[i - 1][1] = wind[i][1];
			}
			count--;
		}
		t[count] = weather->time_s;
		wind[count][0] = weather->wind[0];
		wind[count][1] = weather->wind[1];
		count++;
	}
	span = weather->time_s - t[0];
	if(span < 0.25f) {
		out[0] = 0.0f;
		out[1] = 0.0f;
		return;
	}
	out[0] = (weather->wind[0] - wind[0][0]) / span;
	out[1] = (weather->wind[1] - wind[0][1]) / span;
}

// Rings on whatever the rain lands on, thick where it falls hard
static void draw_splashes(const CameraView* view, const weather_now* weather, float x, float y) {
	const precip_program* p = &g_splash;
	float tint[3];
	float rate[2];

	if(!p->program || !cover_map_ready())
		return;
	tint_of(0, weather->flash, tint);
	glUseProgram(p->program);
	cover_map_apply(&p->cover);
	weather_map_apply(&p->map, 0);
	glUniformMatrix4fv(p->viewproj, 1, GL_FALSE, view->viewproj);
	glUniform2f(p->centre, x, y);
	glUniform3f(p->box, BOX_HALF, BOX_BOTTOM, BOX_HEIGHT);
	glUniform3f(p->motion, RAIN_SPEED, weather->wind[0] * RAIN_WIND_GAIN, weather->wind[1] * RAIN_WIND_GAIN);
	wind_rate_of(weather, rate);
	glUniform2f(p->wind_rate, rate[0] * RAIN_WIND_GAIN, rate[1] * RAIN_WIND_GAIN);
	glUniform1f(p->half_width, SPLASH_HALF);
	glUniform1f(p->time, weather->time_s);
	glUniform3f(p->tint, tint[0], tint[1], tint[2]);
	glUniform1f(p->alpha, 0.42f);
	glDrawArrays(GL_TRIANGLES, 0, SPLASH_MAX * 6);
}

// Pale lines sweeping along in the gusts, more of them the harder it blows
static void draw_ribbons(const CameraView* view, const weather_now* weather, float x, float y) {
	const precip_program* p = &g_ribbon;
	float tint[3];
	float dir[2];

	if(!p->program || !cover_map_ready())
		return;
	wind_direction(weather, dir);
	tint_of(1, weather->flash, tint);
	glUseProgram(p->program);
	cover_map_apply(&p->cover);
	glUniformMatrix4fv(p->viewproj, 1, GL_FALSE, view->viewproj);
	glUniform2f(p->centre, x, y);
	glUniform2f(p->dir, dir[0], dir[1]);
	glUniform1f(p->half_width, RIBBON_HALF);
	glUniform1f(p->time, weather->time_s);
	glUniform1f(p->amount, smooth_step(RIBBON_FROM_LEVEL, RIBBON_FULL_LEVEL, weather->wind_level));
	glUniform1f(p->size, RIBBON_WIDTH);
	glUniform2f(p->viewport, (float)view->viewport_w, (float)view->viewport_h);
	glUniform3f(p->tint, tint[0], tint[1], tint[2]);
	glUniform1f(p->alpha, RIBBON_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, RIBBON_MAX * RIBBON_SEGMENTS * 6);
}

void precip_renderer_draw(const CameraView* view, float x, float y, float h) {
	const weather_now* weather = weather_now_get();
	int rain = weather_map_peak_rain() >= FALL_DRAWN_MIN;
	int snow = weather_map_peak_snow() >= FALL_DRAWN_MIN;
	int dust = DEBRIS_ON && weather->wind_level >= DEBRIS_FROM_LEVEL;
	int ribbons = weather->wind_level >= RIBBON_FROM_LEVEL;
	GLboolean blend_was_on;

	if(!g_fall.program || (!rain && !snow && !dust && !ribbons))
		return;

	blend_was_on = glIsEnabled(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(g_vao);
	if(dust)
		draw_fall(view, weather, x, y, h, KIND_DEBRIS);
	if(ribbons)
		draw_ribbons(view, weather, x, y);
	if(rain) {
		draw_splashes(view, weather, x, y);
		draw_fall(view, weather, x, y, h, KIND_RAIN);
	}
	if(snow)
		draw_fall(view, weather, x, y, h, KIND_SNOW);
	glBindVertexArray(0);
	glUseProgram(0);
	glDepthMask(GL_TRUE);
	glDisable(GL_DEPTH_TEST);
	if(!blend_was_on)
		glDisable(GL_BLEND);
}
