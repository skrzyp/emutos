/*
 * conout.c - lowlevel color model dependent screen handling routines
 *
 *
 * Copyright (C) 2004 by Authors (see below)
 * Copyright (C) 2016-2026 The EmuTOS development team
 *
 * Authors:
 *  MAD     Martin Doering
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * NOTE: the code currently assumes that the font width is 8 bits.
 * If we ever add a 16x32 font, the code will need changing!
 */

#include "emutos.h"
#include "lineavars.h"
#include "biosext.h"
#include "tosvars.h"            /* for v_bas_ad */
#include "sound.h"              /* for bell() */
#include "string.h"
#include "conout.h"
#include "has.h"
#include "amiga.h"
#include "../vdi/vdi_defs.h"    /* for phys_work stuff */

#ifdef MACHINE_AMIGA
/* Contiguous planes: copy/move each plane separately (CPU path).
 * scroll_up: dst < src, so memcpy is safe and faster.
 * scroll_down: dst > src within each plane, must use memmove. */
static void scroll_planes_cpu(UBYTE *dst, UBYTE *src, ULONG count, BOOL upward)
{
    int plane;
    ULONG poff = 0;
    for (plane = 0; plane < v_planes; plane++, poff += v_nxpl) {
        if (upward)
            memcpy(dst + poff, src + poff, count);
        else
            memmove(dst + poff, src + poff, count);
    }
}

/*
 * Contiguous planes: scroll using the Amiga blitter (DMA copy).
 * One blit per plane, ascending or descending based on overlap direction.
 * Falls back to CPU memcpy/memmove when the blitter cannot handle the
 * transfer (alignment, size limits, or blitter disabled).
 */
static void scroll_planes(UBYTE *dst, UBYTE *src, ULONG count, BOOL upward)
{
#if !CONF_WITH_BLITTER
    scroll_planes_cpu(dst, src, count, upward);
#else
    int plane;
    LONG words;
    LONG lines;
    WORD width_w, mod;

    if (!blitter_is_enabled)
    {
        scroll_planes_cpu(dst, src, count, upward);
        return;
    }

    /* Blitter needs word-aligned, word-sized transfers */
    if ((count < 2) || (count & 1) || ((ULONG)src & 1) || ((ULONG)dst & 1))
    {
        scroll_planes_cpu(dst, src, count, upward);
        return;
    }

    words = (LONG)(count >> 1);

    /* Split into lines of up to 64 words (BLTSIZE max width, 0=64).
     * For typical Amiga modes, use the full screen width.  Wider modes
     * need a software fallback because the blitter cannot encode them
     * as a single 2-D copy with zero modulo. */
    width_w = v_lin_wr >> 1;        /* screen line width in words */
    if (width_w > 64)
    {
        scroll_planes_cpu(dst, src, count, upward);
        return;
    }

    if ((words % width_w) == 0)
    {
        lines = words / width_w;
    }
    else
    {
        /* Odd size: blit as 1 word wide, N lines tall */
        width_w = 1;
        lines = words;
    }

    /* BLTSIZE height field is 10 bits (max 1024, 0=1024).
     * Fall back for counts exceeding blitter capacity. */
    if (lines > 1024)
    {
        scroll_planes_cpu(dst, src, count, upward);
        return;
    }

    mod = 0;

    /* Set constant registers once before the per-plane loop.
     * Use HOG mode (BLTPRI) for these large blits -- the CPU
     * just waits anyway, and HOG avoids the interleaved-mode
     * penalty of one idle cycle per DMA access. */
    amiga_blit_wait();
    DMACONW = DMAF_SETCLR | DMAF_BLTPRI;
    BLTCON0 = BLTCON0_USEA | BLTCON0_USED | 0xF0;  /* D = A (copy) */
    BLTAFWM = 0xFFFF;
    BLTALWM = 0xFFFF;
    BLTAMOD = mod;
    BLTDMOD = mod;

    for (plane = 0; plane < v_planes; plane++)
    {
        UBYTE *s = src + (ULONG)plane * v_nxpl;
        UBYTE *d = dst + (ULONG)plane * v_nxpl;
        BOOL desc = (s < d);    /* overlapping: need descending */

        if (plane) amiga_blit_wait();

        BLTCON1 = desc ? BLTCON1_DESC : 0;

        if (desc)
        {
            /* Start from end of data */
            BLTAPTH = (void *)(s + count - 2);
            BLTDPTH = (void *)(d + count - 2);
        }
        else
        {
            BLTAPTH = (void *)s;
            BLTDPTH = (void *)d;
        }

        BLTSIZE = ((UWORD)lines << 6) | (width_w & 0x3F);
    }

    amiga_blit_wait();
    DMACONW = DMAF_BLTPRI;     /* back to interleaved mode */
#endif
}
#endif /* MACHINE_AMIGA */

#if CONF_WITH_VIDEL
static const UWORD falcon_default_palette[16] = {
    0xffdf, 0xf800, 0x07c0, 0xffc0, 0x001f, 0xf81f, 0x07df, 0xbdd7,
    0x8c51, 0xa800, 0x0540, 0xad40, 0x0015, 0xa815, 0x0555, 0x0000
};
#endif

#if CONF_WITH_VDI_16BIT
extern Vwk phys_work;           /* attribute area for physical workstation */
#endif

/*
 * char_addr - retrieve the address of the source cell
 *
 *
 * Given an offset value.
 *
 * in:
 *   ch - source cell code
 *
 * out:
 *   pointer to first byte of source cell if code was valid
 */

static UBYTE *char_addr(WORD ch)
{
    UWORD offs;

    /* test against limits */
    if (ch >= v_fnt_st) {
        if (ch <= v_fnt_nd) {
            /* getch offset from offset table */
            offs = v_off_ad[ch];
            offs >>= 3;                 /* convert from pixels to bytes. */

            /* return valid address */
            return (UBYTE*)v_fnt_ad + offs;
        }
    }

    /* invalid code. no address returned */
    return NULL;
}



/*
 * cell_addr - convert cell X,Y to a screen address.
 *
 * convert cell X,Y to a screen address. also clip cartesian coordinates
 * to the limits of the current screen.
 *
 * input:
 *  x       cell X
 *  y       cell Y
 *
 * returns pointer to first byte of cell
 */

static UBYTE *cell_addr(UWORD x, UWORD y)
{
    ULONG disx, disy;

    /* check bounds against screen limits */
    if (x > v_cel_mx)
        x = v_cel_mx;           /* clipped x */

    if (y > v_cel_my)
        y = v_cel_my;           /* clipped y */

#if CONF_WITH_VIDEL
    if (TRUECOLOR_MODE) {       /* chunky pixels */
        disx = v_planes * x;
    }
    else
#endif
    {
        /*
         * v_planes cannot be more than 8, so as long as there are no more
         * than 4000 characters per line, the result will fit in a word ...
         *
         * X displacement = even(X) * v_planes + Xmod2
         */
        disx = v_nxwd_w * (x & ~1);
        if (IS_ODD(x)) {        /* Xmod2 = 0 ? */
            disx++;             /* Xmod2 = 1 */
        }
    }

    /* Y displacement = Y // cell conversion factor */
    disy = (ULONG)v_cel_wr * y;

    /*
     * cell address = screen base address + Y displacement
     * + X displacement + offset from screen-begin (fix)
     */
    return v_bas_ad + disy + disx + v_cur_of;
}



#if CONF_WITH_VIDEL
/*
 * cell_xfer16 - cell_xfer() for Falcon 16-bit graphics
 *
 * see the comments in cell_xfer() for more details
 */
static void cell_xfer16(UBYTE *src, UBYTE *dst)
{
    UWORD *p;
    UWORD fg, fgcol;
    UWORD bg, bgcol;
    WORD fnt_wr, line_wr, i, mask;

    MAYBE_UNUSED(fg);
    MAYBE_UNUSED(bg);

    fnt_wr = v_fnt_wr;
    line_wr = v_lin_wr;

    if (v_stat_0 & M_REVID) {   /* handle reversed foreground and background colours */
        fg = v_col_bg;
        bg = v_col_fg;
    } else {
        fg = v_col_fg;
        bg = v_col_bg;
    }

    /*
     * if we have 16-bit support in VDI and if the VDI workstation is initialized,
     * we use its palette, otherwise, e.g. at boot, we use a default palette.
     */
#if CONF_WITH_VDI_16BIT
    if (phys_work.ext) {
        fgcol = phys_work.ext->palette[fg];
        bgcol = phys_work.ext->palette[bg];
    } else
#endif
    {
        fgcol = falcon_default_palette[fg & 0xf];
        bgcol = falcon_default_palette[bg & 0xf];
    }

    for (i = v_cel_ht; i--; ) {
        for (mask = 0x80, p = (UWORD *)dst; mask; mask >>= 1) {
            *p++ = (*src & mask) ? fgcol : bgcol;
        }
        dst += line_wr;
        src += fnt_wr;
    }
}
#endif



/*
 * cell_xfer - Performs a byte aligned block transfer.
 *
 *
 * This routine performs a byte aligned block transfer for the purpose of
 * manipulating monospaced byte-wide text. the routine maps a single-plane,
 * arbitrarily-long byte-wide image to a multi-plane bit map.
 * all transfers are byte aligned.
 *
 * in:
 * a0.l      points to contiguous source block (1 byte wide)
 * a1.l      points to destination (1st plane, top of block)
 *
 * out:
 * a4      points to byte below this cell's bottom
 */

static void cell_xfer(UBYTE *src, UBYTE *dst)
{
    UBYTE * src_sav, * dst_sav;
    UWORD fg;
    UWORD bg;
    int fnt_wr, line_wr;
    int plane;

#if CONF_WITH_VIDEL
    if (TRUECOLOR_MODE) {
        cell_xfer16(src, dst);
        return;
    }
#endif

    fnt_wr = v_fnt_wr;
    line_wr = v_lin_wr;

    /* check for reversed foreground and background colors */
    if (v_stat_0 & M_REVID) {
        fg = v_col_bg;
        bg = v_col_fg;
    }
    else {
        fg = v_col_fg;
        bg = v_col_bg;
    }

    src_sav = src;
    dst_sav = dst;

    for (plane = v_planes; plane--; ) {
        int i;

        src = src_sav;                  /* reload src */
        dst = dst_sav;                  /* reload dst */

        if (bg & 0x0001) {
            if (fg & 0x0001) {
                /* back:1  fore:1  =>  all ones */
                for (i = v_cel_ht; i--; ) {
                    *dst = 0xff;                /* inject a block */
                    dst += line_wr;
                }
            }
            else {
                /* back:1  fore:0  =>  invert block */
                for (i = v_cel_ht; i--; ) {
                    /* inject the inverted source block */
                    *dst = ~*src;
                    dst += line_wr;
                    src += fnt_wr;
                }
            }
        }
        else {
            if (fg & 0x0001) {
                /* back:0  fore:1  =>  direct substitution */
                for (i = v_cel_ht; i--; ) {
                    *dst = *src;
                    dst += line_wr;
                    src += fnt_wr;
                }
            }
            else {
                /* back:0  fore:0  =>  all zeros */
                for (i = v_cel_ht; i--; ) {
                    *dst = 0x00;                /* inject a block */
                    dst += line_wr;
                }
            }
        }

        bg >>= 1;                       /* next background color bit */
        fg >>= 1;                       /* next foreground color bit */
        dst_sav += v_nxpl;              /* top of block in next plane */
    }
}



/*
 * neg_cell - negates
 *
 * This routine negates the contents of an arbitrarily-tall byte-wide cell
 * composed of 1 to 8 Atari-style bit-planes, or of Falcon-style 16-bit
 * graphics.
 * Cursor display can be accomplished via this procedure.  Since a second
 * negation restores the original cell condition, there is no need to save
 * the contents beneath the cursor block.
 *
 * input:
 *  cell    points to destination (1st plane, top of block)
 */

static void neg_cell(UBYTE *cell)
{
    int plane, len;
    int cell_len = v_cel_ht;
    int lin_wr = v_lin_wr;

    v_stat_0 |= M_CRIT;                 /* start of critical section. */

#if CONF_WITH_VIDEL
    if (TRUECOLOR_MODE) {               /* chunky pixels */
        for (len = cell_len; len--; ) {
            WORD i;
            UWORD *addr;
            for (i = 8, addr = (UWORD *)cell; i--; addr++)
                *addr = ~*addr;
            cell += lin_wr;
        }
    }
    else
#endif
    {
        for (plane = v_planes; plane--; ) {
            UBYTE * addr = cell;        /* top of current dest plane */

            /* reset cell length counter */
            for (len = cell_len; len--; ) {
                *addr = ~*addr;
                addr += lin_wr;
            }
            cell += v_nxpl;             /* a1 -> top of block in next plane */
        }
    }

    v_stat_0 &= ~M_CRIT;                /* end of critical section. */
}



/*
 * next_cell - Return the next cell address.
 *
 * sets next cell address given the current position and screen constraints
 *
 * returns:
 *     false - no wrap condition exists
 *     true  - CR LF required (position has not been updated)
 */

static BOOL next_cell(void)
{
    /* check bounds against screen limits */
    if (v_cur_cx == v_cel_mx) {         /* increment cell ptr */
        if (!(v_stat_0 & M_CEOL)) {
            /* overwrite in effect */
            return 0;                   /* no wrap condition exists */
                                        /* don't change cell parameters */
        }

        /* call carriage return routine */
        /* call line feed routine */
        return 1;                       /* indicate that CR LF is required */
    }

    v_cur_cx += 1;                      /* next cell to right */

#if CONF_WITH_VIDEL
    if (TRUECOLOR_MODE) {               /* chunky pixels */
        v_cur_ad += 16;
        return 0;
    }
#endif

    /* if X is even, move to next word in the plane */
    if (IS_ODD(v_cur_cx)) {
        /* x is odd */
        v_cur_ad += 1;                  /* a1 -> new cell */
        return 0;                       /* indicate no wrap needed */
    }

    /* new cell (1st plane), added offset to next word in plane */
    v_cur_ad += v_nxwd - 1;

    return 0;                           /* indicate no wrap needed */
}



/*
 * invert_cell - negates the cells bits
 *
 * This routine negates the contents of an arbitrarily-tall byte-wide cell
 * composed of an arbitrary number of (Atari-style) bit-planes.
 *
 * Wrapper for neg_cell().
 *
 * in:
 * x - cell X coordinate
 * y - cell Y coordinate
 */

void invert_cell(int x, int y)
{
    /* fetch x and y coords and invert cursor. */
    neg_cell(cell_addr(x, y));
}



/*
 * move_cursor - move the cursor.
 *
 * move the cursor and update global parameters
 * erase the old cursor (if necessary) and draw new cursor (if necessary)
 *
 * in:
 * d0.w    new cell X coordinate
 * d1.w    new cell Y coordinate
 */

void move_cursor(int x, int y)
{
    /* update cell position */

    /* clamp x,y to valid ranges */
    if (x < 0)
        x = 0;
    else if (x > v_cel_mx)
        x = v_cel_mx;

    if (y < 0)
        y = 0;
    else if (y > v_cel_my)
        y = v_cel_my;

    v_cur_cx = x;
    v_cur_cy = y;

    /* is cursor visible? */
    if (!(v_stat_0 & M_CVIS)) {
        /* not visible */
        v_cur_ad = cell_addr(x, y);             /* just set new coordinates */
        return;                                 /* and quit */
    }

    /* is cursor flashing? */
    if (v_stat_0 & M_CFLASH) {
        v_stat_0 &= ~M_CVIS;                    /* yes, make invisible...semaphore. */

        /* is cursor presently displayed ? */
        if (!(v_stat_0 & M_CSTATE)) {
            /* not displayed */
            v_cur_ad = cell_addr(x, y);         /* just set new coordinates */

            /* show the cursor when it moves */
            neg_cell(v_cur_ad);                 /* complement cursor. */
            v_stat_0 |= M_CSTATE;
            v_cur_tim = v_period;               /* reset the timer. */

            v_stat_0 |= M_CVIS;                 /* end of critical section. */
            return;
        }
    }

    /* move the cursor after all special checks failed */
    neg_cell(v_cur_ad);                         /* erase present cursor */

    v_cur_ad = cell_addr(x, y);                 /* fetch x and y coords. */
    neg_cell(v_cur_ad);                         /* complement cursor. */

    /* do not flash the cursor when it moves */
    v_cur_tim = v_period;                       /* reset the timer. */

    v_stat_0 |= M_CVIS;                         /* end of critical section. */
}



/*
 * ascii_out - prints an ascii character on the screen
 *
 * in:
 *
 * ch.w      ascii code for character
 */

void ascii_out(int ch)
{
    UBYTE * src, * dst;
    BOOL visible;                       /* was the cursor visible? */

    src = char_addr(ch);                /* a0 -> get character source */
    if (src == NULL)
        return;                         /* no valid character */

    dst = v_cur_ad;                     /* a1 -> get destination */

    visible = v_stat_0 & M_CVIS;        /* test visibility bit */
    if (visible) {
        v_stat_0 &= ~M_CVIS;                    /* start of critical section */
    }

    /* put the cell out (this covers the cursor) */
    cell_xfer(src, dst);

    /* advance the cursor and update cursor address and coordinates */
    if (next_cell()) {
        UBYTE * cell;
        UWORD y = v_cur_cy;

        /* perform cell carriage return. */
        cell = v_bas_ad + (ULONG)v_cel_wr * y;
        v_cur_cx = 0;                   /* set X to first cell in line */

        /* perform cell line feed. */
        if (y < v_cel_my) {
            cell += v_cel_wr;           /* move down one cell */
            v_cur_cy = y + 1;           /* update cursor's y coordinate */
        }
        else {
            scroll_up(0);               /* scroll from top of screen */
        }
        v_cur_ad = cell;                /* update cursor address */
    }

    /* if visible */
    if (visible) {
        neg_cell(v_cur_ad);             /* display cursor. */
        v_stat_0 |= M_CSTATE;           /* set state flag (cursor on). */
        v_stat_0 |= M_CVIS;             /* end of critical section. */

        /* do not flash the cursor when it moves */
        if (v_stat_0 & M_CFLASH) {
            v_cur_tim = v_period;       /* reset the timer. */
        }
    }
}



#if CONF_WITH_VIDEL
/*
 * blank_out16 - blank_out() for Falcon 16-bit graphics
 *
 * see the header comments in blank_out() for more details
 */
static void blank_out16(int topx, int topy, int botx, int boty)
{
    UWORD *addr;
    UWORD bgcol;
    WORD i, j;
    WORD offs, rows, width;

    width = (botx - topx + 1) * 8;          /* in words */

    /* calculate the offset from the end of row to next row start */
    offs = v_lin_wr/sizeof(WORD) - width;   /* in words */

    rows = (boty - topy + 1) * v_cel_ht;    /* in pixels */

    /* set standard background colour */
    bgcol = falcon_default_palette[v_col_bg & 0xf];

#if CONF_WITH_VDI_16BIT
    /*
     * if we're already in 16-bit mode, we can get the pixel value of
     * the background colour from the physical workstation's palette instead
     */
    if (phys_work.ext)
        bgcol = phys_work.ext->palette[v_col_bg];
#endif

    addr = (UWORD *)cell_addr(topx, topy);  /* running pointer to screen */
    for (i = 0; i < rows; i++) {
        for (j = 0; j < width; j++) {
            *addr++ = bgcol;
        }
        addr += offs;
    }
}
#endif



/*
 * blank_out - Fills region with the background color.
 *
 * Fills a cell-word aligned region with the background color.
 *
 * The rectangular region is specified by a top/left cell x,y and a
 * bottom/right cell x,y, inclusive.  Routine assumes top/left x is
 * even and bottom/right x is odd for cell-word alignment. This is,
 * because this routine is heavily optimized for speed, by always
 * blanking as much space as possible in one go.
 *
 * in:
 *   topx - top/left cell x position (must be even)
 *   topy - top/left cell y position
 *   botx - bottom/right cell x position (must be odd)
 *   boty - bottom/right cell y position
 */

void blank_out(int topx, int topy, int botx, int boty)
{
    UWORD color;
    int pairs, row, rows;
    UBYTE *addr;

#if CONF_WITH_VIDEL
    if (TRUECOLOR_MODE) {
        blank_out16(topx, topy, botx, boty);
        return;
    }
#endif

    color = v_col_bg;                   /* bg color value */

    addr = cell_addr(topx, topy);       /* running pointer to screen */

    /*
     * # of cell-pairs per row in region - 1
     *
     * e.g. topx = 2, botx = 5, so pairs = 2
     */
    pairs = (botx - topx + 1) / 2;      /* pairs of characters */

    /*
     * # of lines in region - 1
     *
     * see comments re cell-pairs above
     */
    rows = (boty - topy + 1) * v_cel_ht;

#ifdef MACHINE_AMIGA
    /* Contiguous planes: fill each plane using the Amiga blitter.
     * D-only mode: no source or old-dest DMA, just constant fill.
     * This handles both multiplane and monochrome (v_planes=1).
     *
     * Blitmode() disables all Amiga blitter use, including BIOS console
     * acceleration, so that benchmarking and debugging can compare against
     * the software path. */
    {
        UWORD fill_len = pairs * v_nxwd;    /* bytes to fill per row per plane */
        UBYTE plane_byte[MAX_AMIGA_PLANES];
        UWORD i;

#if CONF_WITH_BLITTER
        if (blitter_is_enabled) {
            WORD width_w = fill_len >> 1;   /* words per row */

        /* fill_len is always even (v_nxwd=2 on Amiga); width_w <= 64
         * for all standard resolutions.  BLTSIZE encodes 64 as 0 and
         * its height field is limited to 1024 lines. */
            if ((fill_len >= 2) && !(fill_len & 1) && width_w <= 64
             && rows <= 1024)
            {
                WORD bmod = v_lin_wr - fill_len;

                amiga_blit_wait();
                DMACONW = DMAF_SETCLR | DMAF_BLTPRI;   /* HOG mode */
                BLTCON1 = 0;
                BLTDMOD = bmod;

                for (i = 0; i < v_planes; i++, color >>= 1)
                {
                    if (i) amiga_blit_wait();

                    BLTCON0 = BLTCON0_USED
                           | ((color & 1) ? 0xFF : 0x00);  /* D = all 1s or 0s */
                    BLTDPTH = (void *)(addr + (ULONG)i * v_nxpl);
                    BLTSIZE = ((UWORD)rows << 6) | (width_w & 0x3F);
                }

                amiga_blit_wait();
                DMACONW = DMAF_BLTPRI;     /* back to interleaved */
                return;
            }
        }
#endif

        /* CPU fallback: pre-compute per-plane fill bytes */
        {
            UWORD c = color;
            for (i = 0; i < v_planes; i++) {
                plane_byte[i] = (c & 0x0001) ? 0xff : 0x00;
                c >>= 1;
            }
        }
        /* Planes-outer for contiguous plane locality */
        if (fill_len == (UWORD)v_lin_wr) {
            /* Full width: rows within each plane are contiguous */
            ULONG total = (ULONG)fill_len * rows;
            for (i = 0; i < v_planes; i++)
                memset(addr + (ULONG)i * v_nxpl, plane_byte[i], total);
        } else {
            for (i = 0; i < v_planes; i++) {
                UBYTE *p = addr + (ULONG)i * v_nxpl;
                for (row = rows; row--;) {
                    memset(p, plane_byte[i], fill_len);
                    p += v_lin_wr;
                }
            }
        }
    }
#else
    if (v_planes > 1) {
        /* Interleaved planes: optimized for handling 2 planes at once */
        ULONG pair_planes[4];        /* bits on screen for 8 planes max */
        int pair;
        int offs;
        UWORD i;

        /* calculate the BYTE offset from the end of one row to next start */
        offs = v_lin_wr - pairs * v_nxwd;

        /* Precalculate the pairs of plane data */
        for (i = 0; i < v_planes / 2; i++) {
            /* set the high WORD of our LONG for the current plane */
            if (color & 0x0001)
                pair_planes[i] = 0xffff0000;
            else
                pair_planes[i] = 0x00000000;
            color >>= 1;        /* get next bit */

            /* set the low WORD of our LONG for the current plane */
            if (color & 0x0001)
                pair_planes[i] |= 0x0000ffff;
            color >>= 1;        /* get next bit */
        }

        /* do all rows in region */
        for (row = rows; row--;) {
            /* loop through all cell pairs */
            for (pair = pairs; pair--;) {
                for (i = 0; i < v_planes / 2; i++) {
                    *(ULONG*)addr = pair_planes[i];
                    addr += sizeof(ULONG);
                }
            }
            addr += offs;       /* skip non-region area with stride advance */
        }
    }
    else {
        /* Monochrome mode */
        int pair;
        int offs;
        UWORD pl = (color & 0x0001) ? 0xffff : 0x0000;

        /* calculate the BYTE offset from the end of one row to next start */
        offs = v_lin_wr - pairs * v_nxwd;

        for (row = rows; row--;) {
            for (pair = pairs; pair--;) {
                *(UWORD*)addr = pl;
                addr += sizeof(UWORD);
            }
            addr += offs;
        }
    }
#endif
}



/*
 * scroll_up - Scroll upwards
 *
 *
 * Scroll copies a source region as wide as the screen to an overlapping
 * destination region on a one cell-height offset basis.  Two entry points
 * are provided:  Partial-lower scroll-up, partial-lower scroll-down.
 * Partial-lower screen operations require the cell y # indicating the
 * top line where scrolling will take place.
 *
 * After the copy is performed, any non-overlapping area of the previous
 * source region is "erased" by calling blank_out which fills the area
 * with the background color.
 *
 * in:
 *   top_line - cell y of cell line to be used as top line in scroll
 */

void scroll_up(UWORD top_line)
{
    ULONG count;
    UBYTE * src, * dst;

    /* screen base addr + cell y nbr * cell wrap */
    dst = v_bas_ad + (ULONG)top_line * v_cel_wr;

    /* form source address from cell wrap + base address */
    src = dst + v_cel_wr;

    /* form # of bytes to move */
    count = (ULONG)v_cel_wr * (v_cel_my - top_line);

#ifdef MACHINE_AMIGA
    scroll_planes(dst, src, count, TRUE);
#else
    memmove(dst, src, count);
#endif

    /* exit thru blank out, bottom line cell address y to top/left cell */
    blank_out(0, v_cel_my , v_cel_mx, v_cel_my);
}



/*
 * scroll_down - Scroll (partially) downwards
 */

void scroll_down(UWORD start_line)
{
    ULONG count;
    UBYTE * src, * dst;

    /* screen base addr + offset of start line */
    src = v_bas_ad + (ULONG)start_line * v_cel_wr;

    /* form destination from source + cell wrap */
    dst = src + v_cel_wr;

    /* form # of bytes to move */
    count = (ULONG)v_cel_wr * (v_cel_my - start_line);

#ifdef MACHINE_AMIGA
    scroll_planes(dst, src, count, FALSE);
#else
    memmove(dst, src, count);
#endif

    /* exit thru blank out */
    blank_out(0, start_line , v_cel_mx, start_line);
}
