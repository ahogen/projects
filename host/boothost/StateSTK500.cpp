/****************************************************************************
*
*   Copyright (c) 2005 - 2008 Dave Hylands     <dhylands@gmail.com>
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
*   @file   StateSTK500.cpp
*
*   @brief  Implements the state machine for dealing with a minimal subset
*           of the STK500 protocol. In particular, it only supports enough
*           to talk with the boot loader and not necessarily an AVRISP or
*           STK500.
*
****************************************************************************/

// ---- Include Files -------------------------------------------------------

#include <stdint.h>
#include <sys/unistd.h>
#include <string.h>
#include <time.h>

#include "stk500-command.h"
#include "StateSTK500.h"
#include "Log.h"
#include "AvrInfo.h"

// ---- Public Variables ----------------------------------------------------
// ---- Private Constants and Types -----------------------------------------

#define PACKET_SIZE 512

typedef enum
{
    IDLE,
    SYNCING_RSP,
    GET_PARAM_HW_VER_RSP,
    GET_PARAM_SW_MAJOR_RSP,
    GET_PARAM_SW_MINOR_RSP,
    GET_SIGNATURE_RSP,
    ENTER_PGM_MODE_RSP,
    LOAD_WRITE_ADDR_RSP,
    WRITE_PAGE_RSP,
    LOAD_READ_ADDR_RSP,
    VERIFY_PAGE_RSP,
    LEAVE_PGM_MODE_RSP,
    LOGGING,
    RESTART,
    
} Stk500State_t;

typedef enum
{
    PacketLogging,
    WaitingForINSYNC,
    CollectingPacketData,
    WaitingForOK,

} PacketState_t;

struct DeviceMap
{
    uint8_t     signature[ 3 ];
    DeviceInfo  deviceInfo;
};

// ---- Private Variables ---------------------------------------------------

static  Stk500State_t   gState = IDLE;
static  PacketState_t   gPacketState = WaitingForINSYNC;
static  size_t          gPacketSizeExpected = 0;
static  size_t          gPacketSize = 0;
static  uint8_t         gPacketData[ PACKET_SIZE ];
static  bool            gLogError = true;
static  struct timespec gSyncTime;

static  uint8_t         gHwVer;
static  uint8_t         gSwMajor;
static  uint8_t         gSwMinor;

static  uint8_t         gSignature[ 3 ];

// ---- Private Function Prototypes -----------------------------------------

static  void            ProcessPacket( uint8_t *packetData, size_t packetSize );
static  StatePtrProxy   StateProcessChar( char ch );

// ---- Functions -----------------------------------------------------------

/***************************************************************************/
/**
*   Enters Programming Mode
*/

void EnterProgrammingMode( void )
{
    uint8_t buf[2] = { Cmnd_STK_ENTER_PROGMODE, Sync_CRC_EOP };

    LogDebug( "EnterProgrammingMode\n" );

    SendData( buf, sizeof( buf ));

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = 0;

} // EnterProgrammingMode

/***************************************************************************/
/**
*   Issues a command to retrieve a parameter
*/

void GetParam( uint8_t param )
{
    uint8_t buf[3] = { Cmnd_STK_GET_PARAMETER, 0, Sync_CRC_EOP };

    LogDebug( "GetParam( 0x%02x )\n", param );

    buf[ 1 ] = param;

    SendData( buf, sizeof( buf ));

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = 1;

} // GetParam

/***************************************************************************/
/**
*   Issues a command to retrieve the device signature
*/

void GetSignature( void )
{
    uint8_t buf[2] = { Cmnd_STK_READ_SIGN, Sync_CRC_EOP };

    LogDebug( "GetSignature\n" );

    SendData( buf, sizeof( buf ));

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = 3;

} // GetSignature

/***************************************************************************/
/**
*   Leaves Programming Mode
*/

void LeaveProgrammingMode( void )
{
    uint8_t buf[2] = { Cmnd_STK_LEAVE_PROGMODE, Sync_CRC_EOP };

    LogDebug( "LeaveProgrammingMode\n" );

    SendData( buf, sizeof( buf ));

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = 0;

} // LeaveProgrammingMode

/***************************************************************************/
/**
*   ProgramPage
*
*   Sends down a single page to program into flash.
*/

void ProgramPage( const void *pageData, size_t pageSizeBytes )
{
    uint8_t buf[ 4 ];

    LogDebug( "ProgramPage: %d bytes\n", pageSizeBytes );

    buf[ 0 ] = Cmnd_STK_PROG_PAGE;
    buf[ 1 ] = ( pageSizeBytes >> 8 ) & 0xFF;
    buf[ 2 ] = pageSizeBytes & 0xFF;
    buf[ 3 ] = 'F';

    SendData( buf, sizeof( buf ));
    SendData( pageData, pageSizeBytes );
    SendByte( Sync_CRC_EOP );

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = 0;

} // ProgramPage

/***************************************************************************/
/**
*   ReadPage
*
*   Sends down a request to read a page.
*/

void ReadPage( size_t pageSizeBytes )
{
    uint8_t buf[ 5 ];

    LogDebug( "ReadPage: %d bytes\n", pageSizeBytes );

    buf[ 0 ] = Cmnd_STK_READ_PAGE;
    buf[ 1 ] = ( pageSizeBytes >> 8 ) & 0xFF;
    buf[ 2 ] = pageSizeBytes & 0xFF;
    buf[ 3 ] = 'F';
    buf[ 4 ] = Sync_CRC_EOP; 
    
    SendData( buf, sizeof( buf ));

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = pageSizeBytes;

} // ReadPage

/***************************************************************************/
/**
*   SetLoadAddress 
*
*   For EEPROM its a byte address, for FLASH it's a word address.
*/

void SetLoadAddress( size_t loadAddr )
{
    uint8_t buf[4] = { Cmnd_STK_LOAD_ADDRESS, 0, 0, Sync_CRC_EOP };

    LogDebug( "SetLoadAddress: 0x%04x\n", loadAddr );

    // The load address is sent in little endian format

    buf[ 1 ] = loadAddr & 0xFF;
    buf[ 2 ] = ( loadAddr >> 8 ) & 0xFF;

    SendData( buf, sizeof( buf ));

    gPacketState = WaitingForINSYNC;
    gPacketSizeExpected = 0;

} // SetLoadAddress

/***************************************************************************/
/**
*   Starts the STK500 download sequence. In this case, the host must 
*   initiate the download by sending down an appropriate handshale.
*/

StatePtrProxy StartSTK500( void )
{
    LogDebug( "StartSTK500\n" );

    if ( gDownloadFileName == NULL )
    {
        return StateLog;
    }

    gState = IDLE;
    gPacketState = PacketLogging;
    gPacketSizeExpected = 0;

    return StateProcessChar;

} // StartSTK500

/***************************************************************************/
/**
*   This is the main handler for characters coming in. Since the responses
*   are more packet oriented, it makes sense to funnel everything through
*   a single routine.
*/

StatePtrProxy StateProcessChar( char ch )
{
    //Log( "StateProcessChar 0x%02x gPacketState: %d\n", ch, gPacketState );

    switch ( gPacketState )
    {
        case PacketLogging:
        {
            // We're waiting for the null character to be sent by the target
                
            if ( ch == '\0' )
            {
                Log( "Bootloader detected\n" );

                // The "secret" handshake is to send down GET_SYNC/EOP and get
                // back a INSYNC/OK response

                uint8_t buf[2] = { Cmnd_STK_GET_SYNC, Sync_CRC_EOP };

                SendData( buf, sizeof( buf ));

                clock_gettime(CLOCK_MONOTONIC, &gSyncTime);
                gState = SYNCING_RSP;
                gPacketState = WaitingForINSYNC;
                gPacketSizeExpected = 0;
            }
            else
            {
                // Log the character but don't switch states

                StateLog( ch );
            }
            break;
        }

        case WaitingForINSYNC:
        {
            if ( ch == Resp_STK_INSYNC )
            {
                gPacketSize = 0;
                if ( gPacketSizeExpected == 0 )
                {
                    gPacketState = WaitingForOK;
                }
                else
                {
                    gPacketState = CollectingPacketData;
                }
                gLogError = true;

                break;
            }

            if ( ch == '\0' )
            {
                if ( gState == RESTART )
                {
                    // We've finished downloading the program and the target
                    // restarted. In this case when we see the '\0' we
                    // switch into the logging state.

                    Log( "Starting...\n\n" );

                    return StateLog;
                }

                // Otherwise, due to the timing of things, we may receive some
                // extra 0x00's while we're waiting for the InSync response
                // so we jut ignore them.

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                struct timespec elapsed;
                elapsed.tv_sec = now.tv_sec - gSyncTime.tv_sec;
                if (now.tv_nsec >= gSyncTime.tv_nsec)
                {
                    elapsed.tv_nsec = now.tv_nsec - gSyncTime.tv_nsec;
                }
                else
                {
                    elapsed.tv_nsec = gSyncTime.tv_nsec - now.tv_nsec;
                    elapsed.tv_sec++;
                }
                if (elapsed.tv_sec > 0 || elapsed.tv_nsec > 500000000)
                {
                    // A half second has gone by with no response, resend
                    // the handshale

                    gPacketState = PacketLogging;
                    return StateProcessChar(ch);
                }
                break; 
            }
            if ( ch != 0x00 )
            {
    #if 1
                // Log Character but don't switch states
    
                StateLog( ch );
    #else
                LogError( "InSync: unexpected character received: 0x%02x\n", ch );
                LogError( "Switching to logging mode\n" );
    
                return StateLog;
    #endif
            }
            break;
        }

        case CollectingPacketData:
        {
            if ( gPacketSize < gPacketSizeExpected )
            {
                gPacketData[ gPacketSize++ ] = ch;
            }
            if ( gPacketSize >= gPacketSizeExpected )
            {
                gPacketState = WaitingForOK;
            }
            break;
        }

        case WaitingForOK:
        {
            if ( ch == Resp_STK_OK )
            {
                 // We've successfully parsed a packet. Go and process it.

                ProcessPacket( gPacketData, gPacketSize );
            }
            else
            {
                LogError( "WaitingForOK: unexpected character received: 0x%02x\n", ch );
                LogError( "Switching to logging mode\n" );

                return StateLog;
            }
            gPacketState = WaitingForINSYNC;
            break;
        }

        default:
        {
            LogError( "Unexpected state: %d\n", gPacketState );
            LogError( "Switching to logging mode\n" );

            return StateLog;
        }
    }

    return StateProcessChar;

} // StateProcessChar

/***************************************************************************/
/**
*   Processes a received packet
*/

void ProcessPacket( uint8_t *packetData, size_t packetSize )
{
    LogDebug( "ProcessPacket: packetSize:%d gState:%d\n", packetSize, gState );

    switch ( gState )
    {
        case IDLE:
        {
            // Do nothing

            break;
        }

        case SYNCING_RSP:
        {
            // We're in sync now.

            GetParam( Parm_STK_HW_VER );
            gState = GET_PARAM_HW_VER_RSP;
            break;
        }

        case GET_PARAM_HW_VER_RSP:
        {
            gHwVer = packetData[ 0 ];

            LogVerbose( "HW Ver: 0x%02x\n", gHwVer );

            GetParam( Parm_STK_SW_MAJOR );
            gState = GET_PARAM_SW_MAJOR_RSP;
            break;
        }

        case GET_PARAM_SW_MAJOR_RSP:
        {
            gSwMajor = packetData[ 0 ];

            LogVerbose( "SW Major: 0x%02x\n", gSwMajor );

            GetParam( Parm_STK_SW_MINOR );
            gState = GET_PARAM_SW_MINOR_RSP;
            break;
        }

        case GET_PARAM_SW_MINOR_RSP:
        {
            gSwMinor = packetData[ 0 ];
            LogVerbose( "SW Minor: 0x%02x\n", gSwMinor );

            GetSignature();
            gState = GET_SIGNATURE_RSP;
            break;
        }

        case GET_SIGNATURE_RSP:
        {
            gSignature[ 0 ] = packetData[ 0 ];
            gSignature[ 1 ] = packetData[ 1 ];
            gSignature[ 2 ] = packetData[ 2 ];

            LogVerbose( "Signature: %02x %02x %02x\n",
                        gSignature[ 0 ],
                        gSignature[ 1 ],
                        gSignature[ 2 ] );

            // Lookup the signature

            const AvrInfo_t *avrInfo = NULL;
            if ( gSignature[ 0 ] == 0x1e )
            {
                uint16_t signature = (uint16_t)gSignature[ 1 ] << 8
                                   | (uint16_t)gSignature[ 2 ];
                avrInfo = FindAvrInfoBySignature(signature);
            }
            if ( avrInfo == NULL )
            {
                LogError("Unrecognized device signature: %02x %02x %02x\n",
                         gSignature[ 0 ],
                         gSignature[ 1 ],
                         gSignature[ 2 ]);
                LogError( "Switching to logging mode\n" );
                gState = LOGGING;
                break;
            }
            gDeviceInfo.name = avrInfo->m_string;
            gDeviceInfo.flashSize = avrInfo->m_flashSize;
            gDeviceInfo.bootSize = avrInfo->m_bootSize;
            gDeviceInfo.pageSize = avrInfo->m_pageSize;
            gDeviceInfo.pageShift = avrInfo->m_pageShift;

            Log( "Found: %s\n", gDeviceInfo.name );

            EnterProgrammingMode();
            gState = ENTER_PGM_MODE_RSP;
            break;
        }

        case ENTER_PGM_MODE_RSP:
        {
            gDownloadSegment = gDownloadInfo->head;
            gDownloadOffset = 0;

            Log( "\nProgramming - " );

            SetLoadAddress(( gDownloadSegment->address + gDownloadOffset ) / 2 );

            gState = LOAD_WRITE_ADDR_RSP;
            break;
        }

        case LOAD_WRITE_ADDR_RSP:
        {
            Log( "#" );

            ProgramPage( &gDownloadSegment->data[ gDownloadOffset ], gDeviceInfo.pageSize );

            gState = WRITE_PAGE_RSP;
            break;
        }

        case WRITE_PAGE_RSP:
        {
            gDownloadOffset += gDeviceInfo.pageSize;

            if ( gDownloadOffset >= gDownloadSegment->lenThisSegment )
            {
                gDownloadSegment = gDownloadSegment->next;
                gDownloadOffset = 0;
            }
            if ( gDownloadSegment == NULL )
            {
                // We're done

                Log( "\n  Verifying - " );

                gDownloadSegment = gDownloadInfo->head;
                gDownloadOffset = 0;

                gState = LOAD_READ_ADDR_RSP;
            }
            else
            {
                gState = LOAD_WRITE_ADDR_RSP;
            }

            SetLoadAddress(( gDownloadSegment->address + gDownloadOffset ) / 2 );
            break;
        }

        case LOAD_READ_ADDR_RSP:
        {
            Log( "#" );

            ReadPage( gDeviceInfo.pageSize );

            gState = VERIFY_PAGE_RSP;
            break;
        }

        case VERIFY_PAGE_RSP:
        {
            if ( memcmp( packetData, &gDownloadSegment->data[ gDownloadOffset ], gDeviceInfo.pageSize ) != 0 )
            {
                LogError( "Verify failed at 0x%04x\n", gDownloadSegment->address + gDownloadOffset );
                gState = LOGGING;
            }
            else
            {
                gDownloadOffset += gDeviceInfo.pageSize;

                if ( gDownloadOffset >= gDownloadSegment->lenThisSegment )
                {
                    gDownloadSegment = gDownloadSegment->next;
                    gDownloadOffset = 0;
                }
                if ( gDownloadSegment == NULL )
                {
                    // We're done

                    Log( "\n\n" );

                    LeaveProgrammingMode();
                    gState = LEAVE_PGM_MODE_RSP;
                }
                else
                {
                    SetLoadAddress(( gDownloadSegment->address + gDownloadOffset ) / 2 );
                    gState = LOAD_READ_ADDR_RSP;
                }
            }
            break;
        }

        case LEAVE_PGM_MODE_RSP:
        {
            // So now the bootloader will reset itself. We stuck up the very 
            // first character, which is a 0x00

            gState = RESTART;
            break;
        }

        default:
        {
            break;
        }
    }

} // ProcessPacket

