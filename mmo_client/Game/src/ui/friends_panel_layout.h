#ifndef FRIENDS_PANEL_LAYOUT_H
#define FRIENDS_PANEL_LAYOUT_H

/**
 * @file
 * Share the friends panel's geometry between its model and its renderer.
 *
 * Private to Game/src/ui/. The panel is split in two because the model half is
 * linked into tests that have no GPU: keeping the drawing in the same
 * translation unit meant every one of those link lines had to supply renderer
 * stubs, which is the complaint inventory.c already earns. The two halves must
 * still agree on where a row is, or a click would land somewhere other than
 * what it looks like it hit -- hence one file, included by both.
 */

#include "ui/friends_panel.h"
#include "ui/window_pos.h"

#define FP_PW        340.0f   /**< Panel width. */
#define FP_TITLE_H    40.0f   /**< Title bar height. */
#define FP_TAB_H      30.0f   /**< Tab strip height. */
#define FP_ROW_H      28.0f   /**< Height per list row. */
#define FP_PAD        12.0f   /**< Inner padding. */
#define FP_FOOT_H     26.0f   /**< Footer: the notice line and the key hint. */
#define FP_VISIBLE     8      /**< Rows drawn at once; the rest are paged. */

#define FP_BTN_W      64.0f   /**< Width of a row's action button. */
#define FP_BTN_H      22.0f   /**< Height of a row's action button. */
#define FP_BTN_Y       3.0f   /**< Gap above a row's button. */
#define FP_CLOSE      24.0f   /**< Side of the close button. */

/** Report the panel's height, which is fixed. */
static inline float fp_panel_height(void) {
    return FP_TITLE_H + FP_TAB_H + FP_PAD
         + (float)FP_VISIBLE * FP_ROW_H + FP_FOOT_H;
}

/** Report the panel's top-left corner: where it was dragged, else centred. */
static inline void fp_panel_origin(const FriendsState* fs, int vw, int vh,
                                   float* out_x, float* out_y) {
    window_pos_origin(&fs->win, (float)vw, (float)vh, FP_PW, fp_panel_height(), out_x, out_y);
}

/** Report the y of the first list row. */
static inline float fp_rows_top(float py) {
    return py + FP_TITLE_H + FP_TAB_H + FP_PAD;
}

/** Report how many rows the active tab holds. */
static inline int fp_row_count(const FriendsState* fs) {
    return (fs->tab == FRIENDS_TAB_REQUESTS) ? fs->request_count : fs->friend_count;
}

#endif // FRIENDS_PANEL_LAYOUT_H
