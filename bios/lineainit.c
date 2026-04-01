/*
 * lineainit.c - linea graphics initialization
 *
 * Copyright (C) 2001-2024 by Authors:
 *
 * Authors:
 *  MAD  Martin Doering
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "lineavars.h"
#include "screen.h"
#include "bios.h"
#include "vdiext.h"

/*
 * Precomputed value of log2(8/v_planes), used to derive v_planes_shift.
 * Only the indexes 1, 2 and 4 are meaningful (see set_screen_shift()).
 */
static const UBYTE shift_offset[5] = {0, 3, 2, 0, 1};

/*
 * Current shift value, used to speed up calculations; changed when v_planes
 * changes.  To get the address of a pixel x in a scan line in a bit-plane
 * resolution, use the formula: (x&0xfff0)>>v_planes_shift
 */
UBYTE v_planes_shift;

/*
 * Bitplane memory layout abstraction
 * ===================================
 * Atari ST/STe/TT: interleaved bitplanes.  All planes for a 16-pixel
 * group are stored consecutively: [P0W0][P1W0][P2W0][P3W0][P0W1]...
 *   v_nxwd = v_planes * 2   (bytes to next 16-pixel group in same plane)
 *   v_nxpl = 2              (bytes to next plane at same pixel position)
 *   v_lin_wr = width/8 * v_planes  (bytes per scanline, all planes)
 *
 * Amiga OCS/ECS: contiguous bitplanes.  Each plane is a complete
 * rectangular bitmap: all of plane 0, then all of plane 1, etc.
 *   v_nxwd = 2              (bytes to next 16-pixel group in same plane)
 *   v_nxpl = width/8 * height  (bytes to next plane, full plane size)
 *   v_lin_wr = width/8      (bytes per scanline, one plane only)
 *
 * These variables parameterize all VDI, Line-A, and BIOS drawing code
 * to work with either layout.
 */
/* v_nxpl is ULONG because contiguous plane sizes on Amiga can exceed
 * 32 KB (e.g. 640x512x4 = 40960 bytes/plane).
 * On Amiga mono (v_planes==1), v_nxpl is set to 0: all plane-advancing
 * loops iterate exactly once, so the nxpl increment is never used. */
ULONG v_nxpl;
WORD v_nxwd;            /* bytes to next word in same plane */

/*
 * set_screen_shift() - sets v_planes_shift from the current value of v_planes
 *
 * . v_planes==8 (used by both Falcon & TT) has a shift value of 0
 *
 * . v_planes==16 indicates Falcon 16-bit mode which does not use bit planes,
 *   so we also set v_planes_shift to 0 (it should not be accessed)
 */
void set_screen_shift(void)
{
    /* For contiguous planes (v_nxwd==2), each plane's pixel addressing is
     * like monochrome, so the shift is always 3 regardless of plane count.
     * This check must come first: on Amiga with 5+ planes, v_planes > 4
     * is true but shift=0 would be wrong for contiguous layout. */
    if (v_nxwd == 2)
        v_planes_shift = 3;    /* contiguous: mono-like addressing */
    else if (v_planes > 4)
        v_planes_shift = 0;
    else
        v_planes_shift = shift_offset[v_planes];
}

/*
 * linea_init - init linea variables
 */
void linea_init(void)
{
    screen_get_current_mode_info(&v_planes, &V_REZ_HZ, &V_REZ_VT);

    /* update resolution-dependent values (sets v_nxwd, v_nxpl, v_lin_wr) */
    update_rez_dependent();

    /* precalculate shift value -- must be after update_rez_dependent()
     * because set_screen_shift() uses v_nxwd to detect contiguous layout */
    set_screen_shift();

    KDEBUG(("linea_init(): %dx%d %d-plane (v_lin_wr=%d)\n",
            V_REZ_HZ, V_REZ_VT, v_planes, v_lin_wr));
}
