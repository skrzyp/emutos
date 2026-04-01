/*
 * deskrez.c - handle desktop resolution change dialog
 *
 * This file was created to support desktop resolution changes
 * for the TT and Falcon.
 *
 * Copyright (C) 2012-2025 The EmuTOS development team
 *
 * Authors:
 *  RFB    Roger Burrows
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "aesbind.h"
#include "obdefs.h"
#include "optimize.h"

#include "deskbind.h"
#include "deskglob.h"
#include "deskrsrc.h"
#include "desk_rsc.h"
#include "deskapp.h"
#include "deskfpd.h"
#include "deskwin.h"
#include "deskinf.h"
#include "deskfun.h"
#include "deskrez.h"
#include "desksupp.h"

#include "bdosbind.h"
#include "xbiosbind.h"
#include "has.h"                /* for has_videl etc */
#include "biosdefs.h"
#include "biosext.h"
#include "aesdefs.h"
#include "string.h"


#if CONF_WITH_TT_SHIFTER

/*
 * maps TT dialog buttons to resolution
 */
#define NUM_TT_BUTTONS  5
static const WORD ttrez_from_button[NUM_TT_BUTTONS] =
    { ST_LOW, ST_MEDIUM, ST_HIGH, TT_LOW, TT_MEDIUM };

#endif /* CONF_WITH_TT_SHIFTER */

#if CONF_WITH_VIDEL

/*
 * maps Falcon dialog buttons to 'base' mode
 * note: these correspond to the VGA videomode settings; the
 * VIDEL_VERTICAL bit is inverted for RGB mode (see the code)
 */
static const WORD falconmode_from_button[] =        /*     VGA           RGB     */
    { VIDEL_80COL|VIDEL_1BPP,                       /* 640x480x2     640x400x2   */
      VIDEL_80COL|VIDEL_2BPP,                       /* 640x480x4     640x400x4   */
      VIDEL_80COL|VIDEL_4BPP,                       /* 640x480x16    640x400x16  */
      VIDEL_80COL|VIDEL_8BPP,                       /* 640x480x256   640x400x256 */
      VIDEL_80COL|VIDEL_TRUECOLOR,                  /*     n/a       640x400x64K */
      VIDEL_VERTICAL|VIDEL_80COL|VIDEL_1BPP,        /* 640x240x2     640x200x2   */
      VIDEL_VERTICAL|VIDEL_80COL|VIDEL_2BPP,        /* 640x240x4     640x200x4   */
      VIDEL_VERTICAL|VIDEL_80COL|VIDEL_4BPP,        /* 640x240x16    640x200x16  */
      VIDEL_VERTICAL|VIDEL_80COL|VIDEL_8BPP,        /* 640x240x256   640x200x256 */
      VIDEL_VERTICAL|VIDEL_80COL|VIDEL_TRUECOLOR,   /*     n/a       640x200x64K */
      VIDEL_2BPP,                                   /* 320x480x4     320x400x4   */
      VIDEL_4BPP,                                   /* 320x480x16    320x400x16  */
      VIDEL_8BPP,                                   /* 320x480x256   320x400x256 */
      VIDEL_TRUECOLOR,                              /* 320x480x64K   320x400x64K */
      VIDEL_VERTICAL|VIDEL_2BPP,                    /* 320x240x4     320x200x4   */
      VIDEL_VERTICAL|VIDEL_4BPP,                    /* 320x240x16    320x200x16  */
      VIDEL_VERTICAL|VIDEL_8BPP,                    /* 320x240x256   320x200x256 */
      VIDEL_VERTICAL|VIDEL_TRUECOLOR,               /* 320x240x64K   320x200x64K */
      VIDEL_COMPAT|VIDEL_80COL|VIDEL_1BPP,                  /* ST High */
      VIDEL_COMPAT|VIDEL_VERTICAL|VIDEL_80COL|VIDEL_2BPP,   /* ST Medium */
      VIDEL_COMPAT|VIDEL_VERTICAL|VIDEL_4BPP };             /* ST Low */

#define NUM_FALCON_BUTTONS ARRAY_SIZE(falconmode_from_button)

#endif /* CONF_WITH_VIDEL */

/*
 *  change_st_rez(): change desktop ST resolution
 *  returns:    0   user cancelled change
 *              1   user wants to change; newres is updated with new resolution.
 */
static int change_st_rez(WORD *newres)
{
    if (fun_alert(1,STRESOL) != 1)
        return 0;

    *newres = Getrez() ? 0 : 1;
    return 1;
}

#if CONF_WITH_TT_SHIFTER
/*
 *  change_tt_rez(): change desktop TT resolution
 *  returns:    0   user cancelled change
 *              1   user wants to change; newres is updated with new resolution.
 */
static int change_tt_rez(WORD *newres)
{
OBJECT *tree, *obj;
int i, selected;
WORD oldres;

    oldres = Getrez();
    for (i = 0; i < NUM_TT_BUTTONS; i++)
        if (oldres == ttrez_from_button[i])
            break;

    selected = i;

    /* set up dialog & display */
    tree = desk_rs_trees[ADTTREZ];
    for (i = 0, obj = tree+TTREZSTL; i < NUM_TT_BUTTONS; i++, obj++) {
        if (i == selected)
            obj->ob_state |= SELECTED;
        else obj->ob_state &= ~SELECTED;
    }

    inf_show(tree,ROOT);

    if (inf_what(tree,TTREZOK) == 0)
        return 0;

    /* look for button with SELECTED state */
    i = inf_gindex(tree,TTREZSTL,NUM_TT_BUTTONS);
    if (i < 0)                  /* paranoia */
        return 0;
    if (i == selected)          /* no change */
        return 0;

    *newres = ttrez_from_button[i];
    return 1;
}
#endif

#if CONF_WITH_VIDEL
/*
 *  change_falcon_rez(): change desktop Falcon resolution
 *  returns:    0   user cancelled change
 *              1   user wants to change; newres is set to 3, and newmode
 *                  is updated with the new video mode.
 */
static int change_falcon_rez(WORD *newres,WORD *newmode)
{
OBJECT *tree, *obj;
int i, selected;
WORD oldmode, oldbase, oldoptions;
WORD mode, monitor;

    oldmode = VsetMode(-1);
    oldbase = oldmode & (VIDEL_VERTICAL|VIDEL_COMPAT|VIDEL_80COL|VIDEL_BPPMASK);
    oldoptions = oldmode & (VIDEL_OVERSCAN|VIDEL_PAL|VIDEL_VGA);
    if (!(oldoptions&VIDEL_VGA))    /* if RGB mode, */
        oldbase ^= VIDEL_VERTICAL;  /* this bit has inverted meaning */

    for (i = 0; i < NUM_FALCON_BUTTONS; i++)
        if (oldbase == falconmode_from_button[i])
            break;
    selected = i;

    /* remember monitor type */
    monitor = VgetMonitor();

    /* set up dialog & display */
    tree = desk_rs_trees[ADFALREZ];

    if (monitor != MON_VGA) {       /* fix up rez descriptions if not VGA */
        for (i = 0, obj = tree+FREZNAME; i < 4; i++, obj++)
            obj->ob_spec = (LONG) desktop_str_addr(STREZ1+i);
    }

    /*
     * if we're not compiling with 16-bit support in the VDI,
     * hide the Truecolor header text
     */
#if !CONF_WITH_VDI_16BIT
    obj = tree + FREZTEXT;          /* this hides the "TC" header text */
    obj->ob_flags |= HIDETREE;
#endif

    for (i = 0, obj = tree+FREZLIST; i < NUM_FALCON_BUTTONS; i++, obj++) {
        mode = falconmode_from_button[i];
        if ((mode&VIDEL_BPPMASK) > VIDEL_8BPP) {
            /* hide unsupported TC modes for VGA monitors */
            if ((monitor == MON_VGA) && (mode&VIDEL_80COL))
                obj->ob_flags |= HIDETREE;
            /* hide all TC modes if we're not compiling with 16-bit VDI */
#if !CONF_WITH_VDI_16BIT
            obj->ob_flags |= HIDETREE;
#endif
        }
        if (i == selected)
            obj->ob_state |= SELECTED;
        else obj->ob_state &= ~SELECTED;
    }

    inf_show(tree,ROOT);

    if (inf_what(tree,FREZOK) == 0)
        return 0;

    /* look for button with SELECTED state */
    i = inf_gindex(tree,FREZLIST,NUM_FALCON_BUTTONS);
    if (i < 0)                  /* paranoia */
        return 0;
    if (i == selected)          /* no change */
        return 0;

    mode = falconmode_from_button[i] | oldoptions;
    if (!(oldoptions&VIDEL_VGA))    /* if RGB mode, */
        mode ^= VIDEL_VERTICAL;     /* invert the bit returned */

    if (Srealloc(-1L) < VgetSize(mode)) {
        malloc_fail_alert();
        return 0;
    }

    *newres = FALCON_REZ;
    *newmode = mode;

    return 1;
}
#endif

#ifdef MACHINE_AMIGA

#define N_PRESETS   3
#define N_WIDTHS    2
#define N_HEIGHTS   5
#define N_COLORS    5

static const WORD ami_widths[]   = { 320, 640 };
static const WORD ami_heights[]  = { 200, 256, 400, 480, 512 };
static const WORD ami_hflags[]   = { 0, VIDEL_PAL, VIDEL_VERTICAL,
                                     VIDEL_VGA, VIDEL_PAL|VIDEL_VERTICAL };
static const WORD ami_bpp_code[] = { VIDEL_1BPP, VIDEL_2BPP, VIDEL_3BPP,
                                     VIDEL_4BPP, VIDEL_5BPP };
static const WORD ami_nplanes[]  = { 1, 2, 3, 4, 5 };

/* Preset button -> W/H/C indices for one-click mode selection */
static const WORD preset_w[] = { 0, 1, 1 };    /* 320, 640, 640 */
static const WORD preset_h[] = { 0, 0, 2 };    /* 200, 200, 400 */
static const WORD preset_c[] = { 3, 1, 0 };    /* 16c, 4c, 2c   */

static char ami_info1[42], ami_info2[42], ami_info3[22];

static void ami_select_one(OBJECT *tree, WORD first, WORD count, WORD idx)
{
    WORD i;
    for (i = 0; i < count; i++)
        if (i == idx)
            tree[first+i].ob_state |= SELECTED;
        else
            tree[first+i].ob_state &= ~SELECTED;
}

static void ami_deselect_all(OBJECT *tree, WORD first, WORD count)
{
    WORD i;
    for (i = 0; i < count; i++)
        tree[first+i].ob_state &= ~SELECTED;
}

static WORD ami_find_selected(OBJECT *tree, WORD first, WORD count)
{
    WORD i;
    for (i = 0; i < count; i++)
        if (tree[first+i].ob_state & SELECTED)
            return i;
    return 0;
}

/* Select W/H/C buttons matching a VIDEL mode word */
static void ami_select_mode(OBJECT *tree, WORD mode)
{
    WORD w, h, bpp, hf, i;

    if (mode & VIDEL_COMPAT) {
        bpp = mode & VIDEL_BPPMASK;
        if (bpp == VIDEL_4BPP)      { w = 320; h = 200; }
        else if (bpp == VIDEL_2BPP) { w = 640; h = 200; }
        else                        { w = 640; h = 400; }
    } else {
        w = (mode & VIDEL_80COL) ? 640 : 320;
        if (mode & VIDEL_VGA)
            h = (mode & VIDEL_VERTICAL) ? 240 : 480;
        else if (mode & VIDEL_PAL)
            h = (mode & VIDEL_VERTICAL) ? 512 : 256;
        else
            h = (mode & VIDEL_VERTICAL) ? 400 : 200;
    }

    ami_select_one(tree, AMWID0, N_WIDTHS, (w >= 640) ? 1 : 0);

    hf = mode & (VIDEL_PAL|VIDEL_VGA|VIDEL_VERTICAL);
    for (i = 0; i < N_HEIGHTS; i++)
        if (ami_heights[i] == h && ami_hflags[i] == hf) break;
    ami_select_one(tree, AMHGT0, N_HEIGHTS, (i < N_HEIGHTS) ? i : 0);

    bpp = mode & VIDEL_BPPMASK;
    for (i = 0; i < N_COLORS; i++)
        if (ami_bpp_code[i] == bpp) break;
    ami_select_one(tree, AMCOL0, N_COLORS, (i < N_COLORS) ? i : 0);
}

/* Build VIDEL mode word from current button state */
static WORD ami_build_mode(OBJECT *tree)
{
    WORD w_idx = ami_find_selected(tree, AMWID0, N_WIDTHS);
    WORD h_idx = ami_find_selected(tree, AMHGT0, N_HEIGHTS);
    WORD c_idx = ami_find_selected(tree, AMCOL0, N_COLORS);
    WORD w = ami_widths[w_idx], h = ami_heights[h_idx];
    WORD mode = ami_bpp_code[c_idx];

    if (w >= 640) mode |= VIDEL_80COL;
    mode |= ami_hflags[h_idx];

    /* ST-compatible presets */
    if (w == 320 && h == 200 && c_idx == 3)
        return VIDEL_COMPAT|VIDEL_4BPP;
    if (w == 640 && h == 200 && c_idx == 1)
        return VIDEL_COMPAT|VIDEL_2BPP|VIDEL_80COL;
    if (w == 640 && h == 400 && c_idx == 0)
        return VIDEL_COMPAT|VIDEL_1BPP|VIDEL_80COL|VIDEL_VERTICAL;

    return mode;
}

/* Disable PAL-only height buttons (256, 512) on NTSC systems */
static void ami_validate_heights(OBJECT *tree)
{
    WORD i;
    if (!amiga_is_ntsc)
        return;
    for (i = 0; i < N_HEIGHTS; i++) {
        if (ami_hflags[i] & VIDEL_PAL) {
            tree[AMHGT0 + i].ob_state |= DISABLED;
            if (tree[AMHGT0 + i].ob_state & SELECTED) {
                tree[AMHGT0 + i].ob_state &= ~SELECTED;
                tree[AMHGT0].ob_state |= SELECTED;  /* fall back to 200 */
            }
        }
    }
}

/* Disable 32-color button in hires (OCS/ECS: max 4 planes at 640px) */
static void ami_validate_colors(OBJECT *tree)
{
    WORD w_idx = ami_find_selected(tree, AMWID0, N_WIDTHS);
    OBJECT *c32 = &tree[AMCOL0 + N_COLORS - 1];

    if (ami_widths[w_idx] >= 640) {
        c32->ob_state |= DISABLED;
        if (c32->ob_state & SELECTED) {
            c32->ob_state &= ~SELECTED;
            tree[AMCOL0 + N_COLORS - 2].ob_state |= SELECTED;
        }
    } else {
        c32->ob_state &= ~DISABLED;
    }
}

static void ami_ltoa(char *buf, long n)
{
    char tmp[12];
    WORD i = 0, j;
    if (n <= 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (n > 0) { tmp[i++] = '0' + (char)(n % 10); n /= 10; }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = 0;
}

static void ami_update_info(OBJECT *tree, WORD current_mode)
{
    WORD h_idx = ami_find_selected(tree, AMHGT0, N_HEIGHTS);
    LONG avail = Srealloc(-1L);
    ULONG cur_mem, new_mem;
    char tmp[12];

    cur_mem = amiga_vram_for_mode(current_mode);
    new_mem = amiga_vram_for_mode(ami_build_mode(tree));

    /* Info line 1: "CHIP RAM: nnn KB free" */
    strcpy(ami_info1, "CHIP RAM: ");
    ami_ltoa(tmp, avail / 1024);
    strcat(ami_info1, tmp);
    strcat(ami_info1, " KB free");
    tree[AMINFO1].ob_spec = (LONG) ami_info1;

    /* Info line 2: "Current: nnn KB, New: nnn KB" */
    strcpy(ami_info2, "Current: ");
    ami_ltoa(tmp, (long)cur_mem / 1024);
    strcat(ami_info2, tmp);
    strcat(ami_info2, " KB, New: ");
    ami_ltoa(tmp, (long)new_mem / 1024);
    strcat(ami_info2, tmp);
    strcat(ami_info2, " KB");
    if (avail >= 0 && new_mem > (ULONG)avail)
        strcat(ami_info2, " !");
    tree[AMINFO2].ob_spec = (LONG) ami_info2;

    /* Info line 3: interlace status */
    strcpy(ami_info3, "Interlace: ");
    strcat(ami_info3, (ami_heights[h_idx] >= 400) ? "Yes" : "No");
    tree[AMINFO3].ob_spec = (LONG) ami_info3;
}

static int change_amiga_rez(WORD *newres, WORD *newmode)
{
    OBJECT *tree;
    WORD x, y, w, h, clicked;
    WORD current_mode = amiga_vgetmode();

    tree = desk_rs_trees[ADAMIREZ];

    /* Select current mode */
    ami_select_mode(tree, current_mode);
    ami_deselect_all(tree, AMPRST0, N_PRESETS);
    ami_validate_heights(tree);
    ami_validate_colors(tree);
    ami_update_info(tree, current_mode);

    form_center(tree, &x, &y, &w, &h);
    form_dial(FMD_START, 0, 0, 0, 0, x, y, w, h);
    objc_draw(tree, ROOT, MAX_DEPTH, x, y, w, h);

    for (;;) {
        clicked = form_do(tree, 0) & 0x7FFF;

        /* Deselect only EXIT buttons (OK/Cancel), not TOUCHEXIT radio
         * buttons -- the AES RBUTTON handling already set the correct
         * selection state for those. */
        if (tree[clicked].ob_flags & EXIT)
            tree[clicked].ob_state &= ~SELECTED;

        if (clicked == AMREZCAN)
            break;

        if (clicked == AMREZOK) {
            WORD new_mode = ami_build_mode(tree);
            LONG avail = Srealloc(-1L);
            ULONG need = amiga_vram_for_mode(new_mode);

            if (need > 0 && avail >= 0 && (ULONG)avail < need) {
                malloc_fail_alert();
                objc_draw(tree, ROOT, MAX_DEPTH, x, y, w, h);
                continue;
            }
            if (new_mode == current_mode) {
                objc_draw(tree, ROOT, MAX_DEPTH, x, y, w, h);
                continue;
            }
            form_dial(FMD_FINISH, 0, 0, 0, 0, x, y, w, h);
            *newres = FALCON_REZ;
            *newmode = new_mode;
            return 1;
        }

        /* Preset buttons */
        if (clicked >= AMPRST0 && clicked < AMPRST0 + N_PRESETS) {
            WORD idx = clicked - AMPRST0;
            ami_select_one(tree, AMPRST0, N_PRESETS, idx);
            ami_select_one(tree, AMWID0, N_WIDTHS, preset_w[idx]);
            ami_select_one(tree, AMHGT0, N_HEIGHTS, preset_h[idx]);
            ami_select_one(tree, AMCOL0, N_COLORS, preset_c[idx]);
        }
        /* Width buttons -- RBUTTON handles deselection via IBOX parent */
        else if (clicked >= AMWID0 && clicked < AMWID0 + N_WIDTHS) {
            ami_deselect_all(tree, AMPRST0, N_PRESETS);
        }
        /* Height buttons */
        else if (clicked >= AMHGT0 && clicked < AMHGT0 + N_HEIGHTS) {
            ami_deselect_all(tree, AMPRST0, N_PRESETS);
        }
        /* Color buttons */
        else if (clicked >= AMCOL0 && clicked < AMCOL0 + N_COLORS) {
            ami_deselect_all(tree, AMPRST0, N_PRESETS);
        }

        ami_validate_colors(tree);
        ami_update_info(tree, current_mode);
        objc_draw(tree, ROOT, MAX_DEPTH, x, y, w, h);
    }

    form_dial(FMD_FINISH, 0, 0, 0, 0, x, y, w, h);
    return 0;
}
#endif

/*
 *  change_resolution(): change desktop resolution
 *
 *  note: this is only called when the resolution is changeable;
 *  i.e. it is NOT called in ST high on an ST, or TT high on a TT
 *
 *  returns:    0   user cancelled change
 *              1   user wants to change; newres is updated with new resolution.
 *                  if newres is 3, then it's Falcon-style, and newmode is set
 *                  to the new videomode.
 */
int change_resolution(WORD *newres,WORD *newmode)
{
#ifdef MACHINE_AMIGA
    return change_amiga_rez(newres,newmode);
#endif

#if CONF_WITH_VIDEL
    if (has_videl)
        return change_falcon_rez(newres,newmode);
#endif

#if CONF_WITH_TT_SHIFTER
    if (has_tt_shifter)
        return change_tt_rez(newres);
#endif

    return change_st_rez(newres);
}
