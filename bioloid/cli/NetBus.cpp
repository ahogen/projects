/****************************************************************************
*
*   Copyright (c) 2009 Dave Hylands     <dhylands@gmail.com>
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
*   @file   NetBus.cpp
*
*   @brief  This file implements the NetBus class, which talks to the
*           bioloid bus using a network interface.
*
****************************************************************************/

// ---- Include Files -------------------------------------------------------

#include "Log.h"
#include "DumpMem.h"
#include "NetBus.h"
#include "Str.h"
#include "StrToken.h"
#include "Error.h"

#if defined( WIN32 )
#include <WinSock2.h>
#endif

// ---- Public Variables ----------------------------------------------------
// ---- Private Constants and Types -----------------------------------------
// ---- Private Variables ---------------------------------------------------
// ---- Private Function Prototypes -----------------------------------------
// ---- Functions -----------------------------------------------------------

/**
 * @addtogroup bioloid
 * @{
 */

//***************************************************************************
/**
*   Constructor
*/

NetBus::NetBus()
    : m_dataBytes( 0 ),
      m_debug( false ),
      m_initialized( false )
{
}

//***************************************************************************
/**
*   Destructor
*
*   virtual
*/

NetBus::~NetBus()
{
}

//***************************************************************************
/**
*   Adds a byte to the buffer of data to send.
*/

void NetBus::BufferByte( uint8_t data )
{
    m_data[ m_dataBytes++ ] = data;

    if ( m_dataBytes >= sizeof( m_data ))
    {
        WriteBuffer();
    }
}

//***************************************************************************
/**
*   Sets the serial port that will be used for talking with the bioloid
*   devices.
*/

bool NetBus::Open( const char *hostStr )
{
    uint32_t    err;
    char        errStr[ 200 ];

    if ( !m_initialized )
    {
#if defined( WIN32 )
        WSADATA  wsaData;

        if (( err = WSAStartup( 0x0202, &wsaData )) != 0  )
        {
            LogError( "Error initializing Windows Sockets: %s\n", GetErrorStr( err, errStr, sizeof( errStr )));
            return false;
        }
#endif
        m_initialized = true;
    }

    char   *endPtr;

    // Extract the hostname and portname.

    char    token[ 200 ];

    char *hostName = NULL;

    // NextToken parses skips leading delimiters

    if ( *hostStr == ':' )
    {
        hostName = "";
        hostStr++;
    }

    // If it's all digits, then assume it's a port number

    if ( strspn( hostStr, "0123456789" ) == strlen( hostStr ))
    {
        hostName = "";
    }

    StrTokenizer    tokenizer( hostStr, token, sizeof( token ));

    if ( hostName == NULL )
    {
        hostName = tokenizer.NextToken( ":" );
    }

    if ( hostName == NULL )
    {
        LogError( "No hostname specified" );
        return false;
    }

    // Check to see if the servername is only made up of numbers and dots.

    if ( strspn( hostName, ".0123456789" ) == strlen( hostName ))
    {
        int   seg;
        char *segStr  = hostName;
        char *endPtr;

        // The hostname is only made up of numbers and dots. We'll parse
        // it ourselves.
        //
        // We expect it to be of the form a.b.c.d, where a, b, c, and d are
        // all positive integers in the range 0 thru 255.

        for ( seg = 0; seg < 4; seg++ ) 
        {
            long  longByte = strtol( segStr, &endPtr, 10 );

            if (( longByte < 0 ) || ( longByte > 255 ))
            {
                LogError( "Expecting a positive number between 0 and 255, found: '%s'\n", segStr );
                return false;
            }
            if (( endPtr == segStr )
            ||  (( *endPtr == '.' ) && ( seg == 3 ))
            ||  (( *endPtr != '.' ) && ( seg <  3 ))) 
            {
                LogError( "Expecting IP address of the form a.b.c.d, found: '%s'\n", hostName );
                return false;
            }

            m_sockAddr.sin_addr.S_un.S_addr <<= 8; 
            m_sockAddr.sin_addr.S_un.S_addr += (unsigned char)longByte;

            segStr = endPtr + 1;
        }
        m_sockAddr.sin_addr.S_un.S_addr = htonl( m_sockAddr.sin_addr.S_un.S_addr );
    }
    else
    {
        // The server name was specified symbolically, see if we can translate it

        struct hostent      *hostent;

        hostent = gethostbyname( hostName );
        if ( hostent == NULL )
        {
            LogError( "Unable to translate hostname '%s' into an IP address.\n", hostName );
            return false;
        }
        m_sockAddr.sin_addr.S_un.S_addr = *((long *)hostent->h_addr_list[ 0 ]);
    }

    // Now parse the port (if present)

    const char *portStr = tokenizer.Remainder();

    if ( *portStr == '\0' )
    {
        // No port specified

        m_sockAddr.sin_port = htons( m_defaultPort );
    }
    else
    {
        long	longPort;

        if ( strspn( portStr, "0123456789" ) == strlen( portStr ))
        {
            // The port string is all numeric

            longPort = strtol( portStr, &endPtr, 0 );
            if ( *endPtr != '\0' )
            {
                LogError( "Expecting a positive numeric port specifier; Found '%s'.\n", portStr );
                return false;
            }
        }
        else
        {
            struct servent *servent;

            servent = getservbyname( portStr, "tcp" );
            if ( servent == NULL )
            {
                LogError( "Unable to translate service '%s' into a port number.", portStr );
                return false;
            }
            longPort = ntohs( servent->s_port );
        }

        if (( longPort <= 0 ) || ( longPort > 65535 ))
        {
            LogError( "Expecting port to be between 1 and 65535; Found: %ld\n", longPort );
            return false;
        }
        m_sockAddr.sin_port = htons( (short)longPort );
    }
    m_sockAddr.sin_family = AF_INET;

    // We now have an IP address and port number

    // Open a socket

    m_socket = socket( AF_INET, SOCK_STREAM, 0 );
    if ( m_socket == INVALID_SOCKET )
    {
        err = WSAGetLastError();
        LogError( "Error calling socket: %s\n", GetErrorStr( err, errStr, sizeof( errStr )));
        return false;
    }

    // Connect to the server

    if ( connect( m_socket, (struct sockaddr *)&m_sockAddr, sizeof( m_sockAddr )) != 0 )
    {
        err = WSAGetLastError();
        LogError( "Error calling connect: %s\n", GetErrorStr( err, errStr, sizeof( errStr )));
        return false;
    }

    StrPrintf( m_connectionInfo, sizeof( m_connectionInfo ),
               "%s (%d.%d.%d.%d:%d)", hostStr,
               m_sockAddr.sin_addr.S_un.S_un_b.s_b1,
               m_sockAddr.sin_addr.S_un.S_un_b.s_b2,
               m_sockAddr.sin_addr.S_un.S_un_b.s_b3,
               m_sockAddr.sin_addr.S_un.S_un_b.s_b4,
               ntohs( m_sockAddr.sin_port ));

    Log( "Connected to %s\n", m_connectionInfo );
}

//***************************************************************************
/**
*   Writes all of the buffered bytes to the serial port.
*/

void NetBus::WriteBuffer()
{
    size_t  bytesWritten;

    if ( m_debug )
    {
        DumpMem( "W", 0, m_data, m_dataBytes );
    }

#if 0
    if (( bytesWritten = m_serialPort->Write( m_data, m_dataBytes )) != m_dataBytes )
    {
        LogError( "Error writing %d bytes to serial port", m_dataBytes );
    }
#endif

    m_dataBytes = 0;
}

//***************************************************************************
/**
*   Reads a byte.
*
*   virtual
*/

bool NetBus::ReadByte( uint8_t *ch )
{
    bool rc;

#if 1
    rc = true;
#else
    rc = m_serialPort->Read( ch, 1 ) == 1;
#endif

    if ( rc )
    {
        if ( m_dataBytes < sizeof( m_data ))
        {
            m_data[ m_dataBytes++ ] = *ch;
        }
    }

    return rc;
}

//***************************************************************************
/**
*   Reads a packet. Returns true if a packet was read successfully,
*   false if a timeout or error occurred.
*
*   virtual
*/

bool NetBus::ReadStatusPacket( BioloidPacket *pkt )
{
    bool    rc;

    m_dataBytes = 0;

    rc = BioloidBus::ReadStatusPacket( pkt );

    if ( m_debug )
    {
        if ( m_dataBytes > 0 )
        {
            DumpMem( "R", 0, m_data, m_dataBytes );
        }
#if 0
        if ( !rc )
        {
            LogError( "Packet Error\n" );
        }
#endif
    }

    return rc;
}

//***************************************************************************
/**
*   Sends a byte. This will automatically accumulate the byte into 
*   the checksum)
*
*   virtual
*/

void NetBus::SendByte( uint8_t data )
{
    m_checksum += data;

    BufferByte( data );
}

//***************************************************************************
/**
*   Send the checksum. Since the checksum byte is the last byte of the
*   packet, this function is made virtual to allow bus drivers to
*   buffer the packet bytes until the entire packet is ready to send.
*
*   virtual
*/

void NetBus::SendCheckSum()
{
    SendByte( ~m_checksum );

    WriteBuffer();
}

//***************************************************************************
/**
*   Sends the command header, which is common to all of the commands.
*   2 is added to paramLen (to cover the length and cmd bytes). This
*   way the caller is only responsible for figuring out how many extra
*   parameter bytes are being sent.
*
*   virtual
*/

void NetBus::SendCmdHeader
(
    Bioloid::ID_t       id, 
    uint8_t             paramLen,
    Bioloid::Command    cmd
)
{
    m_dataBytes = 0;

    BioloidBus::SendCmdHeader( id, paramLen, cmd );
}

/** @} */
