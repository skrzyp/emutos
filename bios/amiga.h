/*
 * amiga.h - Amiga specific functions
 *
 * Copyright (C) 2013-2026 The EmuTOS development team
 *
 * Authors:
 *  VRI   Vincent Rivière
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef AMIGA_H
#define AMIGA_H

#ifdef MACHINE_AMIGA

#define MAX_AMIGA_PLANES 8  /* max bitplanes in Copper list (AGA: 8, OCS/ECS lores: 6) */

/* Amiga chipset types */
#define CHIPSET_OCS     0       /* Original Chip Set (A500, A2000) */
#define CHIPSET_ECS     1       /* Enhanced Chip Set (A500+, A600, A3000) */
#define CHIPSET_AGA     2       /* Advanced Graphics Architecture (A1200, A4000) */

struct IDE
{
    UBYTE filler00[4];
    UBYTE features; /* Read: error */
    UBYTE filler06[3];
    UBYTE sector_count;
    UBYTE filler0a[3];
    UBYTE sector_number;
    UBYTE filler0e[3];
    UBYTE cylinder_low;
    UBYTE filler12[3];
    UBYTE cylinder_high;
    UBYTE filler16[3];
    UBYTE head;
    UBYTE filler1a[3];
    UBYTE command; /* Read: status */
    UBYTE filler1e[4091];
    UBYTE control; /* Read: Alternate status */
    UBYTE filler1019[3];
    UBYTE address; /* Write: Not used */
    UBYTE filler02[4067];
    UWORD data;
};

#define ide_interface ((volatile struct IDE*)0x00da0000)

extern const UBYTE scancode_atari_from_amiga[128];
extern UWORD amiga_screen_width;
extern UWORD amiga_screen_width_in_bytes;
extern UWORD amiga_screen_height;
extern ULONG amiga_interlace_offset;
extern UWORD amiga_screen_planes;
extern const UBYTE *amiga_screenbase;
extern UWORD *copper_list;
extern UWORD *amiga_sprite_ptr;     /* current sprite DMA pointer (data or null) */
extern UWORD amiga_palette_shadow[32];
extern int has_gayle;
extern UBYTE amiga_chipset;     /* CHIPSET_OCS / CHIPSET_ECS / CHIPSET_AGA */

/* Amiga OCS blitter registers */
#define DMACONR *(volatile UWORD*)0xdff002
#define DMACONW *(volatile UWORD*)0xdff096
#define DMAF_SETCLR  0x8000  /* same as SETBITS in amiga.c */
#define DMAF_BLTPRI  0x0400
#define BLTCON0 *(volatile UWORD*)0xdff040
#define BLTCON1 *(volatile UWORD*)0xdff042
#define BLTAFWM *(volatile UWORD*)0xdff044
#define BLTALWM *(volatile UWORD*)0xdff046
#define BLTCPTH *(void* volatile*)0xdff048
#define BLTBPTH *(void* volatile*)0xdff04c
#define BLTAPTH *(void* volatile*)0xdff050
#define BLTDPTH *(void* volatile*)0xdff054
#define BLTSIZE *(volatile UWORD*)0xdff058
#define BLTCMOD *(volatile UWORD*)0xdff060
#define BLTBMOD *(volatile UWORD*)0xdff062
#define BLTAMOD *(volatile UWORD*)0xdff064
#define BLTDMOD *(volatile UWORD*)0xdff066
#define BLTBDAT *(volatile UWORD*)0xdff072
#define BLTADAT *(volatile UWORD*)0xdff074

#define BLTCON0_USEA  0x0800
#define BLTCON0_USEB  0x0400
#define BLTCON0_USEC  0x0200
#define BLTCON0_USED  0x0100
#define BLTCON1_DESC  0x0002  /* area mode: descending (right-to-left) */

/* BLTCON1 line mode bits (bit 1 is SING in line mode, DESC in area mode) */
#define BLTCON1_LINE  0x0001  /* bit 0: enable line mode */
#define BLTCON1_AUL   0x0004  /* bit 2: invert "always" axis direction */
#define BLTCON1_SUL   0x0008  /* bit 3: invert "sometimes" axis direction */
#define BLTCON1_SUD   0x0010  /* bit 4: X is major axis (else Y) */
#define BLTCON1_SIGN  0x0040  /* bit 6: sign of DDA accumulator */

extern const UBYTE amiga_minterm[16];
extern UWORD amiga_raster_mask[64];
void amiga_blit_wait(void);

void amiga_machine_detect(void);
const char *amiga_machine_name(void);
void amiga_autoconfig(void);
#if CONF_WITH_ALT_RAM
void amiga_add_alt_ram(void);
ULONG amiga_detect_ram(void *start, void *end, ULONG step);
#endif
UWORD amiga_max_planes(UWORD width);
ULONG amiga_initial_vram_size(void);
void amiga_screen_init(void);
WORD amiga_check_moderez(WORD moderez);
void amiga_get_current_mode_info(UWORD *planes, UWORD *hz_rez, UWORD *vt_rez);
void amiga_setphys(const UBYTE *addr);
const UBYTE *amiga_physbase(void);
WORD amiga_setcolor(WORD colorNum, WORD color);
void amiga_setrez(WORD rez, WORD videlmode);
WORD amiga_vsetmode(WORD mode);
LONG amiga_vgetsize(WORD mode);
void amiga_kbd_init(void);
void amiga_ikbd_writeb(UBYTE b);
void amiga_extra_vbl(void);

/* Hardware sprite cursor */
void amiga_set_sprite_shape(WORD xhot, WORD yhot, WORD bg_col, WORD fg_col,
                            const UWORD *maskdata);
void amiga_move_sprite(WORD x, WORD y);
void amiga_show_sprite(void);
void amiga_hide_sprite(void);
void amiga_update_sprite_colors(void);
void amiga_clock_init(void);
ULONG amiga_getdt(void);

#if CONF_WITH_UAE
typedef ULONG uaelib_demux_t(ULONG fnum, ...);
extern uaelib_demux_t* uaelib_demux;
#define has_uaelib (uaelib_demux != NULL)

void amiga_uae_init(void);
void kprintf_outc_uae(int c);
#endif

void amiga_shutdown(void);
BOOL amiga_can_shutdown(void);

void amiga_floppy_init(void);
BOOL amiga_flop_detect_drive(WORD dev);
WORD amiga_floprw(UBYTE *buf, WORD rw, WORD dev, WORD sect, WORD track, WORD side, WORD count);
LONG amiga_flop_mediach(WORD dev);

void amiga_rs232_init(void);
BOOL amiga_rs232_can_write(void);
void amiga_rs232_writeb(UBYTE b);
void amiga_rs232_rbf_interrupt(void);

/* The following functions are defined in amiga2.S */

void amiga_init_keyboard_interrupt(void);
void amiga_vbl(void);
void amiga_int_5(void);

#endif /* MACHINE_AMIGA */

#endif /* AMIGA_H */
