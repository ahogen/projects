/****************************************************************************
*
*   Copyright (c) 2006 Dave Hylands     <dhylands@gmail.com>
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License version 2 as
*   published by the Free Software Foundation.
*
*   Alternatively, this software may be distributed under the terms of BSD
*   license.
*
*   See README and COPYING for more details.
*
****************************************************************************/
/**
*
*  sertest.c
*
*  PURPOSE:
*
*   This implements a sample program for accessing the serial port.
*
*****************************************************************************/

/* ---- Include Files ---------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/unistd.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>
#include <termios.h>

/* ---- Public Variables ------------------------------------------------- */

int gFd = -1;

int gVal;

/* ---- Private Constants and Types -------------------------------------- */
/* ---- Private Variables ------------------------------------------------ */

struct option gLongOption[] =
{
    // option       A  Flag   V  (has_arg, flag, val)
    // -----------  -  ----  ---
    { "baud",       1, NULL, 'b' },
    { "debug",      0, NULL, 'd' },
    { "help",       0, NULL, 'h' },
    { "port",       1, NULL, 'p' },
    { "verbose",    0, NULL, 'v' },
    { 0 },

};

struct
{
    speed_t     speed;
    unsigned    baudRate;
} gBaudTable[] =
{
    { B50,          50 },
    { B75,          75 },
    { B110,        110 },
    { B134,        134 },
    { B150,        150 },
    { B200,        200 },
    { B300,        300 },
    { B600,        600 },
    { B1200,      1200 },
    { B1800,      1800 },
    { B2400,      2400 },
    { B4800,      4800 },
    { B9600,      9600 },
    { B19200,    19200 },
    { B38400,    38400 },
    { B57600,    57600 },
    { B115200,  115200 },
    { B230400,  230400 }
};

#define ARRAY_LEN(x)    ( sizeof( x ) / sizeof( x[ 0 ]))

int gVerbose = 0;
int gDebug = 0;

int gPortFd = -1;

/* ---- Private Function Prototypes -------------------------------------- */

void *ReadSerialThread( void *param );
void *ReadStdinThread( void *param );
char *StrMaxCpy( char *dst, const char *src, size_t maxLen );
char *StrMaxCat( char *dst, const char *src, size_t maxLen );
void  Usage( void );

/* ---- Functions -------------------------------------------------------- */

#if defined(__CYGWIN__)
// Cygwin seems to be missing cfmakeraw, so we provide a copy here.
static void cfmakeraw(struct termios *termios_p)
{
    termios_p->c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    termios_p->c_cflag &= ~(CSIZE|PARENB);
    termios_p->c_cflag |= CS8;
}
#endif /* defined(__CYGWIN__) */



/***************************************************************************
*
*  main
*
****************************************************************************/

int main( int argc, char **argv )
{
    int         sig;
    int         rc;
    int         opt;
    char        devName[ 40 ];
    const char *baudStr = NULL;
    const char *portStr = "ttyUSB0";
    speed_t     baudRate;
    sigset_t    termSig;
    pthread_t   readSerialThreadId;
    pthread_t   readStdinThreadId;
    struct termios stdin_tio;
    struct termios stdin_tio_org;

    struct termios attr;

    // Parse the command line options

    while (( opt = getopt_long( argc, argv, "b:dhp:v", gLongOption, NULL )) > 0 )
    {
        switch ( opt )
        {
            case 'b':
            {
                baudStr = optarg;
                break;
            }

            case 'd':
            {
                gDebug = 1;
                break;
            }

            case 'p':
            {
                portStr = optarg;
                break;
            }

            case 'v':
            {
                gVerbose = 1;
                break;
            }
            case '?':
            case 'h':
            {
                Usage();
                return 1;
            }
        }
    }

    devName[ 0 ] = '\0';
    if ( portStr[ 0 ] != '/' )
    {
        StrMaxCpy( devName, "/dev/", sizeof( devName ));
    }
    StrMaxCat( devName, portStr, sizeof( devName ));


    baudRate = B0;
    if ( baudStr == NULL )
    {
        baudRate = B38400;
    }
    else
    {
        int baudIdx;
        int testBaud = atoi( baudStr );

        for ( baudIdx = 0; baudIdx < ARRAY_LEN( gBaudTable ); baudIdx++ ) 
        {
            if ( gBaudTable[ baudIdx ].baudRate == testBaud )
            {
                baudRate = gBaudTable[ baudIdx ].speed;
                break;
            }
        }

        if ( baudRate == B0 )
        {
            fprintf( stderr, "Unrecognized baud rate: '%s'\n", baudStr );
            exit( 1 );
        }
    }

    // Open the serial port initially using O_NONBLOCK so that we won't block waiting for
    // carrier detect.

    if (( gPortFd = open( devName, O_RDWR | O_EXCL | O_NONBLOCK )) < 0 )
    {
        fprintf( stderr, "Unable to open serial port '%s': %s\n", devName, strerror( errno ));
        exit( 2 );
    }

    // Now that the serial port is open, we can turn off the non-blocking behaviour (for us we want
    // the reads to have blocking semantics).

    fcntl( gPortFd, F_SETFL, fcntl( gPortFd, F_GETFL ) & ~O_NONBLOCK );

    if ( tcgetattr( gPortFd, &attr ) < 0 )
    {
        fprintf( stderr, "Call to tcgetattr failed: %s\n", strerror( errno ));
        exit( 3 );
    }

    cfmakeraw( &attr );

    // CLOCAL - Disable modem control lines
    // CREAD  - Enable Receiver

    attr.c_cflag |= ( CLOCAL | CREAD );

    cfsetispeed( &attr, baudRate );
    cfsetospeed( &attr, baudRate );

    if ( tcsetattr( gPortFd, TCSAFLUSH, &attr ) < 0 )
    {
        fprintf( stderr, "Call to tcsetattr failed: %s\n", strerror( errno ));
        exit( 4 );
    }

    // Put stdin & stdout in unbuffered mode.

    setbuf( stdin, NULL );
    setbuf( stdout, NULL );

    sigemptyset( &termSig );
    sigaddset( &termSig, SIGINT );
    sigaddset( &termSig, SIGTERM );

    pthread_sigmask( SIG_BLOCK, &termSig, NULL );

    // Put stdin in raw mode (i.e. turn off canonical mode). Canonical mode
    // causes the driver to wait for the RETURN character so that line editing
    // can take place. We also want to turn off ECHO.

    if ( tcgetattr( fileno( stdin ), &stdin_tio_org ) < 0 )
    {
        fprintf( stderr, "Unable to retrieve terminal settings: %s\n", strerror( errno ));
        exit( 5 );
    }

    stdin_tio = stdin_tio_org;
    stdin_tio.c_lflag &= ~( ICANON | ECHO );
    stdin_tio.c_cc[VTIME] = 0;
    stdin_tio.c_cc[VMIN] = 1;

    if ( tcsetattr( fileno( stdin ), TCSANOW, &stdin_tio ) < 0 )
    {
        fprintf( stderr, "Unable to update terminal settings: %s\n", strerror( errno ));
        exit( 6 );
    }

    // Kick off the serial port reader thread.

    rc = pthread_create( &readSerialThreadId, NULL, ReadSerialThread, NULL );
    if ( rc != 0 )
    {
        fprintf( stderr, "Error creating ReadSerialThread: %s\n", strerror( rc ));
        exit( 7 );
    }

    // Kick off the stdin reader thread

    rc = pthread_create( &readStdinThreadId, NULL, ReadStdinThread, NULL );
    if ( rc != 0 )
    {
        fprintf( stderr, "Error creating ReadStdinThread: %s\n", strerror( rc ));
        exit( 7 );
    }

    // Wait for a termmination signal

    if (( rc = sigwait( &termSig, &sig )) != 0 )
    {
        fprintf( stderr, "sigwait failed\n" );
    }
    else
    {
        fprintf( stderr, "Exiting...\n" );
    }

    pthread_cancel( readSerialThreadId );
    pthread_cancel( readStdinThreadId );

    // Restore stdin back to the way it was when we started

    if ( tcsetattr( fileno( stdin ), TCSANOW, &stdin_tio_org ) < 0 )
    {
        fprintf( stderr, "Unable to update terminal settings: %s\n", strerror( errno ));
        exit( 6 );
    }

    // Unblock the termination signals so the user can kill us if we hang up
    // waiting for the reader threads to exit.

    pthread_sigmask( SIG_UNBLOCK, &termSig, NULL );

    pthread_join( readSerialThreadId, NULL );
    pthread_join( readStdinThreadId, NULL );

    close( gPortFd );

    if ( gVerbose )
    {
        fprintf( stderr, "Done\n" );
    }

    exit( 0 );
    return 0;   // Get rid of warning about not returning anything
}

/***************************************************************************/
/**
*   Thread which processes the incoming serial data.
*/

void *ReadSerialThread( void *param )
{
    while ( 1 )
    {
        char    ch;
        int     bytesRead;

        if (( bytesRead  = read( gPortFd, &ch, 1 )) < 0 )
        {
            fprintf( stderr, "Serial port read failed: %s\n", strerror( errno ));
            exit( 1 );
        }

        if ( gDebug )
        {
            if (( ch < ' ' ) || ( ch > '~' ))
            {
                fprintf( stderr, "Serial Read: 0x%02x '.'\n", ch );
            }
            else
            {
                fprintf( stderr, "Serial Read: 0x%02x '%c'\n", ch, ch );
            }
        }

        putc( ch, stdout );
    }

    return NULL;

} // ReadSerialThread

/***************************************************************************/
/**
*   Thread which processes the incoming serial from stdin.
*/

void *ReadStdinThread( void *param )
{
    while ( 1 )
    {
        char    ch;
        int     chInt = fgetc( stdin );
        if ( chInt < 0 )
        {
            fprintf( stderr, "Error reading stdin...\n" );
            break;
        }
        ch = (char)chInt;

        if ( gDebug )
        {
            if (( ch < ' ' ) || ( ch > '~' ))
            {
                fprintf( stderr, "stdin Read: 0x%02x '.'\n", ch );
            }
            else
            {
                fprintf( stderr, "stdin Read: 0x%02x '%c'\n", ch, ch );
            }

        }

        if ( write( gPortFd, &ch, 1 ) != 1 )
        {
            fprintf( stderr, "write to serial port failed: %s\n", strerror( errno ));
            break;
        }
    }

    return NULL;

} // ReadStdinThread

/***************************************************************************/
/**
*  Concatenates source to the destination, but makes sure that the 
*  destination string (including terminating null), doesn't exceed maxLen.
*
*  @param   dst      (mod) String to concatnate onto.
*  @param   src      (in)  String to being added to the end of @a dst.
*  @param   maxLen   (in)  Maximum length that @a dst is allowed to be.
*
*  @return  A pointer to the destination string.
*/

char *StrMaxCat( char *dst, const char *src, size_t maxLen )
{
	size_t	dstLen = strlen( dst );

	if ( dstLen < maxLen )
	{
		StrMaxCpy( &dst[ dstLen ], src, maxLen - dstLen );
	}

	return dst;

} /* StrMaxCat */

/***************************************************************************/
/**
*   Copies the source to the destination, but makes sure that the 
*   destination string (including terminating null), doesn't exceed 
*   maxLen.
*
*   @param  dst     (out) Place to store the string copy.
*   @param  src     (in)  String to copy.
*   @param  maxLen  (in)  Maximum number of characters to copy into @a dst.
*
*   @return A pointer to the destination string.
*/

char *StrMaxCpy( char *dst, const char *src, size_t maxLen )
{
	if ( maxLen < 1 )
	{
		/*
		 * There's no room in the buffer?
		 */

		return "";
	}

	if ( maxLen == 1 )
	{
		/*
		 * There's only room for the terminating null character
		 */

		dst[ 0 ] = '\0';
		return dst;
	}

	/*
	 * The Visual C++ version of strncpy writes to every single character
	 * of the destination buffer, so we use a length one character smaller
	 * and write in our own null (if required).
     *
     * This allows the caller to store a sentinel in the last byte of the
     * buffer to detect overflows (if desired).
	 */

	strncpy( dst, src, maxLen - 1 );
	if (( strlen( src ) + 1 ) >= maxLen )
	{
		/*
		 * The string exactly fits, or probably overflows the buffer.
		 * Write in the terminating null character since strncpy doesn't in
		 * this particular case.
		 *
		 * We don't do this arbitrarily so that the caller can use a sentinel 
		 * in the very end of the buffer to detect buffer overflows.
		 */

		dst[ maxLen - 1 ] = '\0';
	}

	return dst;

} /* StrMaxCpy */

/***************************************************************************
*
*  Usage
*
****************************************************************************/

void Usage()
{
    fprintf( stderr, "Usage: sertest [option(s)]\n" );
    fprintf( stderr, "  Download a program via serial/i2c\n" );
    fprintf( stderr, "\n" );
    fprintf( stderr, "  -b, --baud=baud   Set the baudrate used\n" );
    fprintf( stderr, "  -d, --debug       Turn on debug output\n" );
    fprintf( stderr, "  -h, --help        Display this message\n" );
    fprintf( stderr, "  -p, --port=port   Set the I/O port\n" );
    fprintf( stderr, "  -v, --verbose     Turn on verbose messages\n" );

} // Usage


