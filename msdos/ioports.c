/*
 * Emulation of processor ioports.
 *
 * Copyright 1995 Morten Welinder
 * Copyright 1998 Andreas Mohr, Ove Kaaven
 */

/* Known problems:
   - only a few ports are emulated.
   - real-time clock in "cmos" is bogus.  A nifty alarm() setup could
     fix that, I guess.
*/

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "windef.h"
#include "vga.h"
#include "dosexe.h"
#include "options.h"
#include "miscemu.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(int)

static WORD tmr_8253_countmax[3] = {0xffff, 0x12, 1}; /* [2] needs to be 1 ! */
/* if byte_toggle is TRUE, then hi byte has already been written */
static BOOL16 tmr_8253_byte_toggle[3] = {FALSE, FALSE, FALSE};
static WORD tmr_8253_latch[3] = {0, 0, 0};

/* 4th contents are dummy */
static BOOL16 tmr_8253_latched[4] = {FALSE, FALSE, FALSE, FALSE};
static BYTE tmr_8253_ctrlbyte_ch[4] = {0x06, 0x44, 0x86, 0};

static int dummy_ctr = 0;

static BYTE parport_8255[4] = {0x4f, 0x20, 0xff, 0xff};

static BYTE cmosaddress;

static BYTE cmosimage[64] =
{
  0x27, 0x34, 0x31, 0x47, 0x16, 0x15, 0x00, 0x01,
  0x04, 0x94, 0x26, 0x02, 0x50, 0x80, 0x00, 0x00,
  0x40, 0xb1, 0x00, 0x9c, 0x01, 0x80, 0x02, 0x00,
  0x1c, 0x00, 0x00, 0xad, 0x02, 0x10, 0x00, 0x00,
  0x08, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x03, 0x19,
  0x00, 0x1c, 0x19, 0x81, 0x00, 0x0e, 0x00, 0x80,
  0x1b, 0x7b, 0x21, 0x00, 0x00, 0x00, 0x05, 0x5f
};

#if defined(linux) && defined(__i386__)
# define DIRECT_IO_ACCESS
#else
# undef DIRECT_IO_ACCESS
#endif  /* linux && __i386__ */

#ifdef DIRECT_IO_ACCESS

extern int iopl(int level);

static char do_direct_port_access = 0;
static char port_permissions[0x10000];

#define IO_READ  1
#define IO_WRITE 2

#endif  /* DIRECT_IO_ACCESS */

static void IO_FixCMOSCheckSum(void)
{
	WORD sum = 0;
	int i;

	for (i=0x10; i < 0x2d; i++)
		sum += cmosimage[i];
	cmosimage[0x2e] = sum >> 8; /* yes, this IS hi byte !! */
	cmosimage[0x2f] = sum & 0xff;
	TRACE("calculated hi %02x, lo %02x\n", cmosimage[0x2e], cmosimage[0x2f]);
}

static void set_timer_maxval(unsigned timer, unsigned maxval)
{
    switch (timer) {
        case 0: /* System timer counter divisor */
            DOSVM_SetTimer(maxval);
            break;
        case 1: /* RAM refresh */
            FIXME("RAM refresh counter handling not implemented !");
            break;
        case 2: /* cassette & speaker */
            /* speaker on ? */
            if (((BYTE)parport_8255[1] & 3) == 3)
            {
                TRACE("Beep (freq: %d) !\n", 1193180 / maxval );
                Beep(1193180 / maxval, 20);
            }
            break;
    }
}

/**********************************************************************
 *	    IO_port_init
 */

/* set_IO_permissions(int val1, int val)
 * Helper function for IO_port_init
 */
#ifdef DIRECT_IO_ACCESS
static void set_IO_permissions(int val1, int val, char rw)
{
	int j;
	if (val1 != -1) {
		if (val == -1) val = 0x3ff;		
		for (j = val1; j <= val; j++)
			port_permissions[j] |= rw;		

		do_direct_port_access = 1;

		val1 = -1;
	} else if (val != -1) {		
		do_direct_port_access = 1;

		port_permissions[val] |= rw;
	}

}

/* do_IO_port_init_read_or_write(char* temp, char rw)
 * Helper function for IO_port_init
 */

static void do_IO_port_init_read_or_write(char* temp, char rw)
{
	int val, val1, i, len;
	if (!strcasecmp(temp, "all")) {
		MESSAGE("Warning!!! Granting FULL IO port access to"
			" windoze programs!\nWarning!!! "
			"*** THIS IS NOT AT ALL "
			"RECOMMENDED!!! ***\n");
		for (i=0; i < sizeof(port_permissions); i++)
			port_permissions[i] |= rw;

	} else if (!(!strcmp(temp, "*") || *temp == '\0')) {
		len = strlen(temp);
		val = -1;
		val1 = -1;		
		for (i = 0; i < len; i++) {
			switch (temp[i]) {
			case '0':
				if (temp[i+1] == 'x' || temp[i+1] == 'X') {
					sscanf(temp+i, "%x", &val);
					i += 2;
				} else {
					sscanf(temp+i, "%d", &val);
				}
				while (isxdigit(temp[i]))
					i++;
				i--;
				break;
			case ',':
			case ' ':
			case '\t':
				set_IO_permissions(val1, val, rw);
				val1 = -1; val = -1;
				break;
			case '-':
				val1 = val;
				if (val1 == -1) val1 = 0;
				break;
			default:
				if (temp[i] >= '0' && temp[i] <= '9') {
					sscanf(temp+i, "%d", &val);
					while (isdigit(temp[i]))
						i++;
				}
			}
		}
		set_IO_permissions(val1, val, rw);		
	}
}

static inline BYTE inb( WORD port )
{
    BYTE b;
    __asm__ __volatile__( "inb %w1,%0" : "=a" (b) : "d" (port) );
    return b;
}

static inline WORD inw( WORD port )
{
    WORD w;
    __asm__ __volatile__( "inw %w1,%0" : "=a" (w) : "d" (port) );
    return w;
}

static inline DWORD inl( WORD port )
{
    DWORD dw;
    __asm__ __volatile__( "inl %w1,%0" : "=a" (dw) : "d" (port) );
    return dw;
}

static inline void outb( BYTE value, WORD port )
{
    __asm__ __volatile__( "outb %b0,%w1" : : "a" (value), "d" (port) );
}

static inline void outw( WORD value, WORD port )
{
    __asm__ __volatile__( "outw %w0,%w1" : : "a" (value), "d" (port) );
}

static inline void outl( DWORD value, WORD port )
{
    __asm__ __volatile__( "outl %0,%w1" : : "a" (value), "d" (port) );
}

#endif  /* DIRECT_IO_ACCESS */

void IO_port_init()
{
#ifdef DIRECT_IO_ACCESS
	char temp[1024];

	/* Can we do that? */
	if (!iopl(3)) {
		iopl(0);

		PROFILE_GetWineIniString( "ports", "read", "*",
					 temp, sizeof(temp) );
		do_IO_port_init_read_or_write(temp, IO_READ);
		PROFILE_GetWineIniString( "ports", "write", "*",
					 temp, sizeof(temp) );
		do_IO_port_init_read_or_write(temp, IO_WRITE);
	}
#endif  /* DIRECT_IO_ACCESS */
    IO_FixCMOSCheckSum();
}


/**********************************************************************
 *	    IO_inport
 *
 * Note: The size argument has to be handled correctly _externally_ 
 * (as we always return a DWORD)
 */
DWORD IO_inport( int port, int size )
{
    DWORD res = 0;

    TRACE("%d-byte value from port 0x%02x\n", size, port );

#ifdef DIRECT_IO_ACCESS    
    if ((do_direct_port_access)
        /* Make sure we have access to the port */
        && (port_permissions[port] & IO_READ))
    {
        iopl(3);
        switch(size)
        {
        case 1: res = inb( port ); break;
        case 2: res = inw( port ); break;
        case 4: res = inl( port ); break;
        default:
            ERR("invalid data size %d\n", size);
        }
        iopl(0);
        return res;
    }
#endif

    switch (port)
    {
    case 0x3ba:
    case 0x3da:
        res = (DWORD)VGA_ioport_in( port );
        break;
    case 0x40:
    case 0x41:
    case 0x42:
    {
        BYTE chan = port & 3;
        WORD tempval = 0;

        if (tmr_8253_latched[chan] == TRUE)
        {
            tempval = tmr_8253_latch[chan];
            tmr_8253_latched[chan] = FALSE;
        }
        else
        {
            dummy_ctr -= 1+(int) (10.0*rand()/(RAND_MAX+1.0));
        if (chan == 0) /* System timer counter divisor */
                /* FIXME: DOSVM_GetTimer() returns quite rigid values */
				tempval = dummy_ctr+(WORD)DOSVM_GetTimer();
        else
        {   /* FIXME: intelligent hardware timer emulation needed */
            tempval = dummy_ctr;
        }
        }
        switch (tmr_8253_ctrlbyte_ch[chan] & 0x30)
        {
        case 0x00:
            break; /* correct ? */
        case 0x10: /* read lo byte */
            res = (BYTE)tempval;
            break;
        case 0x30: /* read lo byte, then hi byte */
            tmr_8253_byte_toggle[chan] ^= TRUE; /* toggle */
            if (tmr_8253_byte_toggle[chan] == TRUE)
            {
                res = (BYTE)tempval;
                break;
            }
            /* else [fall through if read hi byte !] */
        case 0x20: /* read hi byte */
            res = (BYTE)tempval>>8;
            break;
        }
        break;
    }
    case 0x60:
        res = INT_Int09ReadScan();
#if 0 /* what's this port got to do with parport ? */
        res = (DWORD)parport_8255[0];
#endif
        break;
    case 0x61:
        res = (DWORD)parport_8255[1];
        break;
    case 0x62:
        res = (DWORD)parport_8255[2];
        break;
    case 0x70:
        res = (DWORD)cmosaddress;
        break;
    case 0x71:
        res = (DWORD)cmosimage[cmosaddress & 0x3f];
        break;
    case 0x200:
    case 0x201:
        res = 0xffffffff; /* no joystick */
        break;
    default:
        WARN("Direct I/O read attempted from port %x\n", port);
        res = 0xffffffff;
        break;
    }
    TRACE("  returning ( 0x%lx )\n", res );
    return res;
}


/**********************************************************************
 *	    IO_outport
 */
void IO_outport( int port, int size, DWORD value )
{
    TRACE("IO: 0x%lx (%d-byte value) to port 0x%02x\n",
                 value, size, port );

#ifdef DIRECT_IO_ACCESS
    if ((do_direct_port_access)
        /* Make sure we have access to the port */
        && (port_permissions[port] & IO_WRITE))
    {
        iopl(3);
        switch(size)
        {
        case 1: outb( LOBYTE(value), port ); break;
        case 2: outw( LOWORD(value), port ); break;
        case 4: outl( value, port ); break;
        default:
            WARN("Invalid data size %d\n", size);
        }
        iopl(0);
        return;
    }
#endif

    switch (port)
    {
    case 0x3c8:
    case 0x3c9:
        VGA_ioport_out( port, (BYTE)value );
        break;
    case 0x20:
        DOSVM_PIC_ioport_out( port, (BYTE)value );
        break;
    case 0x40:
    case 0x41:
    case 0x42:
    {
        BYTE chan = port & 3;
        WORD oldval = 0;

        if ( ((tmr_8253_ctrlbyte_ch[chan] & 0x30) != 0x30) ||
/* we need to get the oldval before any lo/hi byte change has been made */
             (tmr_8253_byte_toggle[chan] == FALSE) )
            oldval = tmr_8253_countmax[chan];
        switch (tmr_8253_ctrlbyte_ch[chan] & 0x30)
        {
        case 0x00:
            break; /* correct ? */
        case 0x10: /* write lo byte */
            tmr_8253_countmax[chan] =
                (tmr_8253_countmax[chan] & 0xff00) | (BYTE)value;
            break;
        case 0x30: /* write lo byte, then hi byte */
            tmr_8253_byte_toggle[chan] ^= TRUE; /* toggle */
            if (tmr_8253_byte_toggle[chan] == TRUE)
            {
                tmr_8253_countmax[chan] =
                    (tmr_8253_countmax[chan] & 0xff00) | (BYTE)value;
                break;
            }
            /* else [fall through if write hi byte !] */
        case 0x20: /* write hi byte */
            tmr_8253_countmax[chan] =
                (tmr_8253_countmax[chan] & 0xff)|((BYTE)value << 8);
            break;
        }
        if (
            /* programming finished ? */
            ( ((tmr_8253_ctrlbyte_ch[chan] & 0x30) != 0x30) ||
              (tmr_8253_byte_toggle[chan] == FALSE) )
            /* update to new value ? */
            && (tmr_8253_countmax[chan] != oldval)
            )
            set_timer_maxval(chan, tmr_8253_countmax[chan]);
    }
    break;          
    case 0x43:
        {
          BYTE chan = ((BYTE)value & 0xc0) >> 6;

        /* ctrl byte for specific timer channel */
          tmr_8253_ctrlbyte_ch[chan] = (BYTE)value;
          if (chan == 3) {
            FIXME("8254 timer readback not implemented yet\n");
            }
          else
          if (((BYTE)value&0x30)==0) { /* latch timer */
            tmr_8253_latched[chan] = TRUE;
            dummy_ctr -= 1+(int) (10.0*rand()/(RAND_MAX+1.0));
            if (chan == 0) /* System timer divisor */
              tmr_8253_latch[0] = dummy_ctr+(WORD)DOSVM_GetTimer();
            else
            {   /* FIXME: intelligent hardware timer emulation needed */
              tmr_8253_latch[chan] = dummy_ctr;
            }
          }
        if ((value & 0x30) == 0x30) /* write lo byte, then hi byte */
            tmr_8253_byte_toggle[((BYTE)value & 0xc0) >> 6] = FALSE; /* init */
        }
        break;
    case 0x61:
        parport_8255[1] = (BYTE)value;
        if ((((BYTE)parport_8255[1] & 3) == 3) && (tmr_8253_countmax[2] != 1))
        {
            TRACE("Beep (freq: %d) !\n", 1193180 / tmr_8253_countmax[2]);
            Beep(1193180 / tmr_8253_countmax[2], 20);
        }
        break;
    case 0x70:
        cmosaddress = (BYTE)value & 0x7f;
        break;
    case 0x71:
        cmosimage[cmosaddress & 0x3f] = (BYTE)value;
        break;
    default:
        WARN("Direct I/O write attempted to port %x\n", port );
        break;
    }
}
