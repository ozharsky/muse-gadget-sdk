/* Stage C cadence simulator + accent finder (muse_idle.h only; no renderer).
 *
 *   cc -O2 -Wall -I components/muse/avatar tools/muse/idle_sim.c -lm -o /tmp/idle_sim
 *
 *   /tmp/idle_sim --table [secs]                        persona comparison table (default 3600 s)
 *   /tmp/idle_sim <persona> <seed|face:NAME> [secs]    cadence metrics for one persona
 *   /tmp/idle_sim <persona> <seed|face:NAME> --events [secs]
 *        prints the schedule in rt seconds -- the same clock `idle_demo OUTDIR <secs>
 *        idle <start>` renders, so an accent found here can be captured there.
 *
 * The face seeds replicate the renderers' own derivation (a fresh xorshift ->
 * frand()*100000 + {7 shodan, 13 stone, 11 golem}), so the schedule lines up with
 * a real idle_demo run of that face (verified: shodan and golem match the runtime
 * schedule exactly; stone draws from its face RNG before the scheduler initializes,
 * so for stone windows use tools/muse/probe_idle.c -- the ground-truth probe).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_idle.h"

#define DT (0.04f)
#define BASE 10.0f     /* the demo's clock origin (pose t = BASE + rt) */
#define WARM 2.0f

static uint32_t s_rng = 0x9e3779b9u;
static unsigned g_skip;   /* extra face-RNG draws before the scheduler seed (IDLE_SIM_SKIP) */
static uint32_t irand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t face_seed(unsigned off)
{
    uint32_t v = 0;
    for (unsigned i = 0; i <= g_skip; i++) {
        v = irand();
    }
    return (uint32_t)((float)(v & 0xffffu) / 65535.0f * 100000.0f) + off;
}

static const muse_idle_cfg_t cfg_neutral = MUSE_IDLE_PERSONA_NEUTRAL;
static const muse_idle_cfg_t cfg_shodan = MUSE_IDLE_PERSONA_SHODAN;
static const muse_idle_cfg_t cfg_stone = MUSE_IDLE_PERSONA_STONE;
static const muse_idle_cfg_t cfg_golem = MUSE_IDLE_PERSONA_GOLEM;

typedef struct {
    int blinks, doubles, doze;
    int micro, dart, drift;
    int beats, accents;
    int widen, squint, smile, wink;
    float lid_sum;
    float accent_dur_min, accent_dur_max;
    float accent_times[48];
    int accent_n;
    float blink_times[512];
    int blink_n;
    int breath_cycles;
} stats_t;

static stats_t sim(const muse_idle_cfg_t *cfg, uint32_t seed, float start, float secs)
{
    stats_t st;
    memset(&st, 0, sizeof(st));
    st.accent_dur_min = 1e9f;
    muse_idle_t s;
    muse_idle_init_cfg(&s, seed, cfg);
    int frames = (int)(secs / DT + 0.5f);
    int warm = (int)(WARM / DT + 0.5f);
    int first = (int)(start / DT + 0.5f);
    bool in_blink = false, ev_open = false;
    float prev_start = -1e9f;
    float ltx = 0, lty = 0;
    float prev_b = 0;
    for (int i = first - warm; i < first + frames; i++) {
        float t = BASE + (float)i * DT;
        muse_idle_step(&s, t, DT, 0, true);
        float sh = muse_idle_shut(&s, t);
        if (!in_blink && sh > 0.02f) {
            st.blinks++;
            st.lid_sum += s.blink_dur;
            if (prev_start > -1e8f) {
                float gap = t - prev_start;
                if (gap > 0.25f && gap < 0.75f) {
                    st.doubles++;
                }
            }
            if (s.blink_dur > 0.28f) {
                st.doze++;
            }
            if (st.blink_n < 512) {
                st.blink_times[st.blink_n] = (float)i * DT;
            }
            st.blink_n++;
            prev_start = t;
            in_blink = true;
        } else if (in_blink && sh <= 0.02f) {
            in_blink = false;
        }
        if (s.tx != ltx || s.ty != lty) {
            float d = fabsf(s.tx - ltx) + fabsf(s.ty - lty);
            if (s.gaze_tau > 0.09f) {
                st.drift++;
            } else if (d < 0.6f) {
                st.micro++;                 /* the micro-saccade's small in-place nudge */
            } else {
                st.dart++;
            }
            ltx = s.tx;
            lty = s.ty;
        }
        float k = muse_idle_ev_k(&s, t);
        if (!ev_open && s.ev != MUSE_IDLE_EV_NONE && k > 0.05f) {
            ev_open = true;
            st.beats++;
            switch (s.ev) {
            case MUSE_IDLE_EV_WIDEN: st.widen++; break;
            case MUSE_IDLE_EV_SQUINT: st.squint++; break;
            case MUSE_IDLE_EV_SMILE: st.smile++; break;
            case MUSE_IDLE_EV_WINK: st.wink++; break;
            case MUSE_IDLE_EV_ACCENT:
                st.accents++;
                if (s.ev_dur < st.accent_dur_min) st.accent_dur_min = s.ev_dur;
                if (s.ev_dur > st.accent_dur_max) st.accent_dur_max = s.ev_dur;
                if (st.accent_n < 48) {
                    st.accent_times[st.accent_n++] = (float)i * DT;
                }
                break;
            default: break;
            }
        } else if (ev_open && s.ev == MUSE_IDLE_EV_NONE) {
            ev_open = false;
        }
        float b = muse_idle_breath(&s);
        if (prev_b <= 0 && b > 0) {
            st.breath_cycles++;
        }
        prev_b = b;
    }
    return st;
}

static const muse_idle_cfg_t *pick(const char *name)
{
    if (strcmp(name, "neutral") == 0) return &cfg_neutral;
    if (strcmp(name, "shodan") == 0) return &cfg_shodan;
    if (strcmp(name, "stone") == 0) return &cfg_stone;
    if (strcmp(name, "golem") == 0) return &cfg_golem;
    fprintf(stderr, "idle_sim: unknown persona '%s' (neutral|shodan|stone|golem)\n", name);
    exit(2);
}

static void print_row(const char *name, uint32_t seed, float secs) {
    const muse_idle_cfg_t *cfg = pick(name);
    stats_t st = sim(cfg, seed, 0.0f, secs);
    float pm = secs / 60.0f;
    printf("| %-7s | %5.1f | %4.0f%% | %4.0f%% | %.3f | %5.1f | %3.0f/%3.0f/%3.0f | %4.1f | %4.2f | %6.1f | %.2f-%.2f | %4.1f |\n",
           name, st.blinks / pm, 100.0f * st.doubles / (st.blinks ? st.blinks : 1),
           100.0f * st.doze / (st.blinks ? st.blinks : 1),
           st.lid_sum / (st.blinks ? st.blinks : 1),
           (st.micro + st.dart + st.drift) / pm,
           100.0f * st.micro / (st.micro + st.dart + st.drift ? st.micro + st.dart + st.drift : 1),
           100.0f * st.dart / (st.micro + st.dart + st.drift ? st.micro + st.dart + st.drift : 1),
           100.0f * st.drift / (st.micro + st.dart + st.drift ? st.micro + st.dart + st.drift : 1),
           st.beats / pm, st.accents / pm,
           st.accents ? secs / st.accents : 0.0f,
           st.accents ? st.accent_dur_min : 0.0f, st.accents ? st.accent_dur_max : 0.0f,
           st.breath_cycles / pm);
}

int main(int argc, char **argv)
{
    const char *sk = getenv("IDLE_SIM_SKIP");
    if (sk) {
        g_skip = (unsigned)atoi(sk);
    }
    if (argc >= 2 && strcmp(argv[1], "--table") == 0) {
        float secs = argc > 2 ? (float)atof(argv[2]) : 3600.0f;
        printf("| persona | blinks/m | dbl%% | doze%% | lid s | sacc/m | micro/dart/drift %% | beats/m | accent/m |~every s | accent dur | breath/m |\n");
        printf("|---|---|---|---|---|---|---|---|---|---|---|---|\n");
        print_row("neutral", 12345u, secs);
        print_row("shodan", face_seed(7u), secs);
        print_row("stone", face_seed(13u), secs);
        print_row("golem", face_seed(11u), secs);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: idle_sim <persona> <seed|face:NAME> [--events] [secs]\n"
                        "       idle_sim --table [secs]\n");
        return 2;
    }
    const char *persona = argv[1];
    uint32_t seed = 0;
    if (strncmp(argv[2], "face:", 5) == 0) {
        const char *nm = argv[2] + 5;
        unsigned off = strcmp(nm, "shodan") == 0 ? 7u : strcmp(nm, "stone") == 0 ? 13u
                        : strcmp(nm, "golem") == 0 ? 11u : 0u;
        if (!off) { fprintf(stderr, "idle_sim: face must be shodan|stone|golem\n"); return 2; }
        seed = face_seed(off);
    } else {
        seed = (uint32_t)strtoul(argv[2], NULL, 0);
    }
    bool events = argc > 3 && strcmp(argv[3], "--events") == 0;
    float secs = argc > (events ? 4 : 3) ? (float)atof(argv[events ? 4 : 3]) : 600.0f;
    const muse_idle_cfg_t *cfg = pick(persona);
    stats_t st = sim(cfg, seed, 0.0f, secs);
    printf("%s seed=%u  %gs sim\n", persona, seed, secs);
    printf("blinks    %d (%.1f/min)  doubles %d (%.0f%%)  doze %d (%.0f%%)  mean lid %.3fs\n",
           st.blinks, st.blinks / (secs / 60.0f), st.doubles, 100.0f * st.doubles / (st.blinks ? st.blinks : 1),
           st.doze, 100.0f * st.doze / (st.blinks ? st.blinks : 1), st.lid_sum / (st.blinks ? st.blinks : 1));
    printf("saccades  %d total  micro %d  dart %d  drift %d  (%.1f/min)\n",
           st.micro + st.dart + st.drift, st.micro, st.dart, st.drift, (st.micro + st.dart + st.drift) / (secs / 60.0f));
    printf("beats     %d total (%.1f/min)  widen %d squint %d smile %d wink %d\n",
           st.beats, st.beats / (secs / 60.0f), st.widen, st.squint, st.smile, st.wink);
    printf("accents   %d (%.2f/min)  dur %.2f-%.2f s\n", st.accents,
           st.accents / (secs / 60.0f), st.accents ? st.accent_dur_min : 0.0f,
           st.accents ? st.accent_dur_max : 0.0f);
    printf("breath    %d cycles (%.1f/min measured; cfg rate %.2f rad/s)\n",
           st.breath_cycles, st.breath_cycles / (secs / 60.0f), cfg->breath_rate);
    if (events) {
        printf("blinks 100-160 @ rt:");
        for (int i = 0; i < st.blink_n && i < 512; i++) {
            if (st.blink_times[i] < 100.0f || st.blink_times[i] > 160.0f) {
                continue;
            }
            printf(" %.2f", st.blink_times[i]);
        }
        printf("\n");
        printf("accents @ rt:");
        for (int i = 0; i < st.accent_n; i++) {
            printf(" %.2f", st.accent_times[i]);
        }
        printf("\n");
    }
    return 0;
}
