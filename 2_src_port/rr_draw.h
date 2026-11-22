/* rr_draw.h - ESP frame drawing for ROB RUN. */
#ifndef RR_DRAW_H
#define RR_DRAW_H

#include "rr_game.h"
#include "menu_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws one frame of markers into the current gpu_begin()/gpu_present() pair.
 * vw/vh are the overlay surface size in pixels. */
void draw_esp(const RR_PLAYER* arr, int n, const RR_CAMERA* cam, int vw, int vh);

/* Builds the cop counter HUD line from the published telemetry in `b`
 * (telem_cop_count, telem_slot_total, telem_slot_seats):
 *     U+1F46E  "12 cops"  U+00B7  "3 vagas"
 * (police officer, middle dot; singular forms for one cop / one free seat).
 * Returns 1 when `out` holds a line to draw, 0 when the HUD must stay hidden:
 * switch off, or no battle (no occupied seat). The renderer and the self-test
 * share this rule, so no test needs a GPU or a game to cover it. */
int cop_hud_text(char* out, int cap, const BR_Config* b);

typedef struct {
    unsigned fresh, repeated, initial, state, gap, jump;
    unsigned max_gap_ms;
    float max_jump_m;
    unsigned moving_samples, reversals, bone_jumps;
    float max_bone_delta_m;
    unsigned head_spikes, left_spikes, right_spikes;
} RR_CHAM_DIAG;
void cham_diag_enable(void);
void cham_diag_sample(const RR_PLAYER* player);
RR_CHAM_DIAG cham_diag_results(void);

#ifdef __cplusplus
}
#endif

#endif /* RR_DRAW_H */
