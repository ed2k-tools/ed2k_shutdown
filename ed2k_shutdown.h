/***************************************************************************
                              ed2k_shutdown.c
                             -----------------

    A command line program that does nothing but shut down an eDonkey2000
     command line client or overnet command line client via the core-GUI
     admin port protocol. The core must be listening for a GUI for this
     to work (ie. you need to have set a admin username + password and
     started the core with the - and the ! option:  
     ./donkey - !

    begin                : Tue Feb 04 2003
    copyright            : (C) 2003 by Tim-Philipp Müller
    email                : t.i.m@orange.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#define GNET_EXPERIMENTAL
#include <gnet/gnet.h>

#define VERSION		"0.1"
#define PACKAGE		"ed2k_shutdown"

/********************************************************************************************
 *    ramfile stuff
 ********************************************************************************************/
struct _ramfile
{
	guint     	 size;		// current size
	guint     	 pos; 		// current position
	GByteArray	*data;		// data buffer
} ;

typedef struct _ramfile ramfile;

enum
{
	RAMFILE_SEEK_FROM_HERE=1,
	RAMFILE_SEEK_FROM_START,
	RAMFILE_SEEK_FROM_END
} ;

#define ramfile_get_size(rf)	rf->size
#define ramfile_get_pos(rf)	rf->pos
#define ramfile_set_pos(rf,newpos)	rf->pos=newpos
#define ramfile_get_buffer(rf)	(rf->data)->data


/********************************************************************************************
 *    messages from the GUI to the core
 ********************************************************************************************/

#define ED2K_BYTE      			0xe3	/* = 227, header byte at beginning of every packet */
#define CONT_LOGIN     			100		/* + i4string: username + i4string: password */
#define CONT_STOP      			101		/* core please stop 			*/
#define CONT_CMD       			102
#define GUI_GET_OPTIONS			225


/********************************************************************************************
 *    connection status stuff
 ********************************************************************************************/

#define GUICORECONN_CALLBACK_READ_SIZE	1024

// global, because send_logout() needs this
enum
{
 GUI_CORE_CONN_STATUS_NONE = 0,
 GUI_CORE_CONN_STATUS_CONNECTING,		// We are trying to connect
 GUI_CORE_CONN_STATUS_AUTHENTICATING,		// We are trying to log on (user/pass)
 GUI_CORE_CONN_STATUS_COMMUNICATING,		// We are logged on
 GUI_CORE_CONN_STATUS_CLOSED_DELIBERATELY,	// We've closed the connection deliberately
 GUI_CORE_CONN_STATUS_CLOSED_UNEXPECTEDLY,	// The connection broke unexpectedly
 GUI_CORE_CONN_STATUS_CLOSED_TIMEOUT,		// The connection timed out (no read for X seconds)
 GUI_CORE_CONN_STATUS_CLOSED_TIMEOUT_NO_DATA,	// The connection timed out and we've never received any data
} ;

