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

/***************************************************************************
 *                                                                         *
 *   Note: this code was put together from code snippets of the            *
 *   ed2k-gtk-gui. It's ugly and contains a lot of cruft, but it works     *
 *   (for me anyway). It's a 30-minute hack, so don't complain.            *
 *   patches welcome ;).                                                   *
 *                                                                         *
 *   http://ed2k-tools.sourceforge.net/                                    *
 *                                                                         *
 ***************************************************************************/



#include "ed2k_shutdown.h"

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

static void    		 guicoreconn_send_packet (ramfile *packet);

static gboolean		 packet_append (ramfile **packet, gchar *buffer, gint length);
static gboolean		 packet_is_valid (ramfile *packet);
static guint32		 packet_get_data_size (ramfile *packet);
static ramfile		*packet_get_next_packet_data (ramfile *packet);
static gboolean		 packet_is_complete (ramfile *packet);

static ramfile		*ramfile_new (guint len);
static void				 ramfile_free (ramfile *rf);
static guint8			 ramfile_read8 (ramfile *rf);
static guint32		 ramfile_read32 (ramfile *rf);
static guint16		 ramfile_read16 (ramfile *rf);
static void				 ramfile_write8 (ramfile *rf, const gint8 val);
static void				 ramfile_write16 (ramfile *rf, const gint16 val);
static void				 ramfile_write32 (ramfile *rf, const gint32 val);
static gboolean		 ramfile_seek (ramfile *rf, gint inc, gint seektype);
static guint8			*ramfile_read (ramfile *rf, guint8 *buf, gint len);
static guint8			*ramfile_peek (ramfile *rf, guint8 *buf, gint pos, gint len);
static void				 ramfile_write (ramfile *rf, guint8 *buf, gint len);
static void				 ramfile_write_i4_string (ramfile *rf, const gchar *string);
static void				 send_message_without_data (guint8 msg);
static void				 send_login (const gchar *user, const gchar *pass);
static void				 send_logout (void);
static void				 send_get_options (void);

static gboolean		 guicoreconn_callback_function (GConn *conn, GConnStatus status, gchar *buffer, gint length, gpointer data);
static void    		 guicoreconn_close_and_cleanup (void);


/* variables */

gchar				*shutdown_host = NULL;	 
gchar				*shutdown_user = NULL;	
gchar				*shutdown_pass = NULL;
gint 				 shutdown_port;
gint 				 only_send_shutdown = 0;

GConn				*guicoreconn = NULL;					// the GUI-core connection

guint   		 guicoreconn_status = GUI_CORE_CONN_STATUS_NONE;	// status
gboolean		 guicoreconn_packet_being_written = FALSE;
GSList  		*guicoreconn_write_queue = NULL;

#define GUICORECONN_READ_TIMEOUT	0
#define GUICORECONN_WRITE_TIMEOUT	0
#define GUICORECONN_CONNECT_TIMEOUT	10*1000		// does gnet lib use this now? (see docs)

// if we haven't received data from the core for 90s, a timeout function is called
#define GUICORECONN_TIMEOUT_INITIAL	15*1000
#define GUICORECONN_TIMEOUT		90*1000

// guicoreconn_callback_function
//
// The callback function for the GUI-core connection

static gboolean
guicoreconn_callback_function (GConn *conn, GConnStatus status, gchar *buffer, gint length, gpointer destroywidget)
{
	static ramfile		*packet = NULL;
	static guint8		 packet_read_buf [GUICORECONN_CALLBACK_READ_SIZE];

	g_return_val_if_fail (conn!=NULL, FALSE);

	switch (status)
	{
		// We are connected to the core!
		case GNET_CONN_STATUS_CONNECT:
		{
			guicoreconn_status = GUI_CORE_CONN_STATUS_AUTHENTICATING;
			if (packet) ramfile_free (packet);
			packet = NULL;

			gnet_conn_readany (conn, packet_read_buf, GUICORECONN_CALLBACK_READ_SIZE, GUICORECONN_READ_TIMEOUT);
			gnet_conn_timeout (conn, GUICORECONN_TIMEOUT_INITIAL);

			send_login (shutdown_user, shutdown_pass);

			if (only_send_shutdown==1)
			{
				send_get_options();
				gnet_conn_timeout (conn, 5*1000);
			}
		}
		break;

		case GNET_CONN_STATUS_CLOSE:
		{
			guicoreconn_close_and_cleanup();
		}
		break;

		case GNET_CONN_STATUS_READ:
		{
			if (packet_append (&packet, buffer, length))
			{
				if (!packet_is_valid (packet))
				{
					g_printerr ( "Invalid packet on GUI-core connection - closing connection.\n");
					guicoreconn_close_and_cleanup ();	// XXX = treat it as a crash?
				} 
				else 
				{
					while ((packet) && packet_is_complete(packet))
					{
						ramfile *next_packet;
						next_packet = packet_get_next_packet_data (packet);
						if (packet) 
							ramfile_free (packet);
						packet = next_packet;

						// only send the command once
						if ((only_send_shutdown==1)
							&& guicoreconn_status != GUI_CORE_CONN_STATUS_COMMUNICATING)
						{
							g_printerr ( ">>>>> Logged in okay - sending %s command\n", (only_send_shutdown==1) ? "shutdown" : "advanced");
							if (only_send_shutdown==1) 
								send_logout();
						} 
						else 
						{
							gnet_conn_timeout (conn, GUICORECONN_TIMEOUT); /* Valid data received. Renew timeout */
						}

						switch (guicoreconn_status)
						{
							default:
							case GUI_CORE_CONN_STATUS_AUTHENTICATING:
							{
								guicoreconn_status = GUI_CORE_CONN_STATUS_COMMUNICATING;
								break;
							}
	
							case GUI_CORE_CONN_STATUS_COMMUNICATING:
							case GUI_CORE_CONN_STATUS_CLOSED_DELIBERATELY:
								; // do nothing, keep status as it is
							break;
						}
					}
				}
			} else guicoreconn_close_and_cleanup ();
		}
		return TRUE;

		case GNET_CONN_STATUS_WRITE:			// data has been successfully written
		{
			guicoreconn_packet_being_written = FALSE;
			if (guicoreconn_write_queue)
			{
				ramfile *packet = (ramfile*) guicoreconn_write_queue->data;
				guicoreconn_write_queue = g_slist_remove (guicoreconn_write_queue, (gpointer) packet);
				guicoreconn_send_packet (packet);
			}
			if (buffer) g_free (buffer);

			if (only_send_shutdown==1)
			{
				g_printerr ( ">>>>> Sent command.\n");
			}
		}
		break;

		case GNET_CONN_STATUS_TIMEOUT:
		{
			if (guicoreconn_status == GUI_CORE_CONN_STATUS_AUTHENTICATING)
			{
				guicoreconn_status = GUI_CORE_CONN_STATUS_CLOSED_TIMEOUT_NO_DATA;
			} else guicoreconn_status = GUI_CORE_CONN_STATUS_CLOSED_TIMEOUT;
			guicoreconn_close_and_cleanup ();
		}
		break;

		case GNET_CONN_STATUS_ERROR:
		{
			guicoreconn_close_and_cleanup ();
		}
		break;


	}
	return FALSE;	// always return false unless status is read and we want to read more
}



// guicoreconn_close_and_cleanup
//
// Clean up stuff after the connection has been closed or close it 'not deliberately'
//	if not already closed.
// Then take the appropriate action (ie. 'connect to..'-dialog, try to reconnect, etc.)

static void
guicoreconn_close_and_cleanup (void)
{
	const gchar *errmsg = "";
	
		// Do not change status if we've closed the connection deliberately. Deliberately stays deliberately.
	if (guicoreconn_status == GUI_CORE_CONN_STATUS_CLOSED_DELIBERATELY
			|| guicoreconn_status == GUI_CORE_CONN_STATUS_CLOSED_TIMEOUT)
	{
		g_printerr (">>>>> Seems like we've shut down the core alright\n");
		exit(EXIT_SUCCESS);
  }

	else if (guicoreconn_status == GUI_CORE_CONN_STATUS_CONNECTING)
	{
		errmsg = ">>>>> Connect to core failed - could not connect!?\n";
	}

	else if (guicoreconn_status == GUI_CORE_CONN_STATUS_AUTHENTICATING)
	{
		errmsg = ">>>>> Connect to core failed - couldn't log in.\n";
	}

	else if (guicoreconn_status == GUI_CORE_CONN_STATUS_CLOSED_TIMEOUT_NO_DATA)
	{
		errmsg = ">>>>> Connect to core failed - no data received within 5 seconds!?\n";
	}

	else
	{
		errmsg = ">>>>> GUI-core connection broke unexpectedly! (core crashed/killed?)\n";
	}

	g_printerr (errmsg);

	exit (EXIT_FAILURE);
}



/******************************************************************************
 *
 *  guicoreconn_send_packet
 *
 *
 */

static void
guicoreconn_send_packet (ramfile *packet)
{
	guint8		*np;
	guint32		 data_size, intel_size;

	if (!guicoreconn)
		return;

	if (!gnet_conn_is_connected(guicoreconn))
		return;

	if (guicoreconn_packet_being_written)
	{
		guicoreconn_write_queue = g_slist_append (guicoreconn_write_queue, (gpointer) packet);
		return;
	}

	guicoreconn_packet_being_written = TRUE;

	data_size = ramfile_get_size(packet);
	intel_size = GUINT32_TO_LE(data_size);		// to little endian
	// this buffer will be freed by connection callback when write was successful
	np = g_new (guint8, data_size+5);
	g_return_if_fail (np!=NULL);			// mem alloc problem

	np[0] = ED2K_BYTE;
	memcpy (np+1, &intel_size, 4);
	memcpy (np+1+4, ramfile_get_buffer(packet), data_size);

	gnet_conn_write (guicoreconn, np, data_size+5, GUICORECONN_WRITE_TIMEOUT);
	ramfile_free(packet);
	
}



// functions

// packet_append
//
// returns FALSE on error, otherwise TRUE
//

static gboolean
packet_append (ramfile **packet, gchar *buffer, gint length)
{
	g_return_val_if_fail (packet!=NULL, FALSE);
	g_return_val_if_fail (buffer!=NULL, FALSE);

	if (*packet==NULL)
	{
		*packet = ramfile_new (length);
		g_return_val_if_fail (*packet!=NULL, FALSE);
	}
	ramfile_write(*packet, buffer, length);
	return TRUE;
}

static gboolean
packet_is_valid (ramfile *packet)
{
	g_return_val_if_fail (packet!=NULL, FALSE);
	if (ramfile_get_size(packet)>=1)
	{
		guint8	donkeybyte;
		ramfile_peek(packet, &donkeybyte, 0, sizeof(guint8));
		if (donkeybyte != ED2K_BYTE)
			return FALSE;
	}
	return TRUE;
}

static guint32
packet_get_data_size (ramfile *packet)
{
	guint32	datalen;
	g_return_val_if_fail (packet!=NULL, 0);
	if (ramfile_get_size(packet)<5)
		return 0;
	ramfile_peek(packet, (guint8*)&datalen, 1, 4);
	return GUINT32_FROM_LE(datalen);
}

static gboolean
packet_is_complete (ramfile *packet)
{
	// CHECK: do we need an _assertion_ here?
	g_return_val_if_fail (packet!=NULL, FALSE);
	// minimal length of a complete packet is 6 (5 bytes header + 1 byte command)
	if (ramfile_get_size(packet)>=6)
	{
		guint32	datasize;
		datasize = packet_get_data_size(packet);
		if (datasize>0)
		{
			return (ramfile_get_size(packet) >= 5+datasize);
		} // else return FALSE
	}
	return FALSE;
}


// packet_get_next_packet_data
//
// If a packet is complete, it might have some data at
//    the end which belongs to the next packet. This
//    routine will get that data and put it at the
//    beginning of a new packet and return that.
// If there is no superfluous data, it will return NULL
//
// call this only after packet_is_complete() has checked out fine!

static ramfile *
packet_get_next_packet_data (ramfile *packet)
{
	ramfile	*nextp;
	guint32	 datasize, toomuch;

	g_return_val_if_fail (packet!=NULL, NULL);

	datasize = packet_get_data_size (packet);

	if (datasize==0)
		return NULL;

	// minimal length of a complete packet is
	//   1 byte + chunks*4 bytes [command chunk lengths]
	//      + chunks*1 byte [the command chunks]
	toomuch = ramfile_get_size(packet) - (5+datasize);

	if (toomuch==0)
		return NULL;

//	// strictly, it should be < instead of <= (?) --- XXX - shouldn't it be >= ???
//	// If toomuch was smaller than GUICORECONN_CALLBACK_READ_SIZE, then
//	g_return_val_if_fail (toomuch<=GUICORECONN_CALLBACK_READ_SIZE, NULL);

	nextp = ramfile_new (1);
	ramfile_write (nextp, ramfile_get_buffer(packet)+5+datasize, toomuch);
	return nextp;
}


// ramfile_new
//
// creates new ramfile
//   len: initial len
//
// returns pointer if successful, otherwise NULL

//static ncount = 0;
//static fcount = 0;

static ramfile *
ramfile_new (guint len)
{
	ramfile *rf = g_new (ramfile,1);
//	g_print ("%s (%u)\n", __FUNCTION__, len);
	g_return_val_if_fail (rf!=NULL, NULL);
	rf->data = g_byte_array_new ();
	g_return_val_if_fail (rf->data!=NULL, NULL);
	rf->data = g_byte_array_set_size (rf->data, len);
	g_return_val_if_fail (rf->data!=NULL, NULL);
	rf->pos = 0;
	rf->size = len;
//	ncount++;
//	g_print ("ramfile - new=%u - freed=%u\n",ncount,fcount);
	return rf;
}



static void
ramfile_free (ramfile *rf)
{
	g_return_if_fail (rf!=NULL);
//	if (rf==NULL) return;
	g_return_if_fail (rf->data!=NULL);
	g_byte_array_free(rf->data, TRUE);
	g_free (rf);
//	fcount++;
//	g_print ("ramfile - new=%u - freed=%u\n",ncount,fcount);
}

// ramfile_read8
//
// reads in one byte
// returns 0xFF on error
static guint8
ramfile_read8 (ramfile *rf)
{
	g_return_val_if_fail (rf!=NULL, 0xff);
	g_return_val_if_fail (rf->data!=NULL, 0xff);
	if (rf->pos > rf->size) return 0xff;
	return (rf->data)->data[rf->pos++];
}

// ramfile_read16
//
// reads in two bytes
// returns 0xFFFF on error

static guint16	
ramfile_read16 (ramfile *rf)
{
	guint16	ret;
	g_return_val_if_fail (rf!=NULL, 0xffff);
	g_return_val_if_fail (rf->data!=NULL, 0xffff);
	if (rf->pos+2 > rf->size) return 0xffff;
	memcpy (&ret, (rf->data)->data+rf->pos, 2);
	rf->pos += 2;
	return GUINT16_FROM_LE(ret);
}

// ramfile_read32
//
// reads in four bytes
// returns 0xFFFFFFFF on error

static guint32	
ramfile_read32 (ramfile *rf)
{
	gint32 ret;
	g_return_val_if_fail (rf!=NULL, 0xffffffff);
	g_return_val_if_fail (rf->data!=NULL, 0xffffffff);
	if (rf->pos+4 > rf->size) return 0xffffffff;
	memcpy (&ret, (rf->data)->data+rf->pos, 4);
	rf->pos += 4;
	return GUINT32_FROM_LE(ret);
}


// ramfile_write8
//

static void
ramfile_write8 (ramfile *rf, const gint8 val)
{
	gint8	val_le = val;
	ramfile_write (rf, (guint8*)&val_le, 1);
}

// ramfile_write16
//

static void
ramfile_write16 (ramfile *rf, const gint16 val)
{
	gint16	val_le = GINT16_TO_LE(val);
	ramfile_write (rf, (guint8*)&val_le, 2);
}


// ramfile_write32
//

static void
ramfile_write32 (ramfile *rf, const gint32 val)
{
	gint32	val_le = GUINT32_TO_LE(val);
	ramfile_write (rf, (guint8*)&val_le, 4);
}

static gboolean
ramfile_seek (ramfile *rf, gint inc, gint seektype)
{
	gint newpos;
	g_return_val_if_fail (rf!=NULL, FALSE);
	g_return_val_if_fail (rf->data!=NULL, FALSE);
	switch (seektype)
	{
		case RAMFILE_SEEK_FROM_HERE:
		{
			newpos = rf->pos+inc;
		}
		break;

		case RAMFILE_SEEK_FROM_START:
		{
			newpos = inc;
		}
		break;

		case RAMFILE_SEEK_FROM_END:
		{
			newpos = rf->size - inc;
		}
		break;

		default:	return FALSE;		// wrong seek type
	}	

	if (newpos > rf->size) return FALSE;		// can't seek to pos that doesn't exist
	rf->pos = newpos;
	return TRUE;
}


// ramfile_read
//
// reads in bytes from ramfile
//   rf:  ramfile
//   buf: buffer where data is stored to
//        if NULL, buffer will be allocated (0-terminated) and returned (user must free it later)
//
// returns allocated buffer if buf was set NULL, otherwise buf (or NULL if len=0)
//

static guint8 *
ramfile_read (ramfile *rf, guint8 *buf, gint len)
{
	g_return_val_if_fail (rf!=NULL, NULL);
	// CHECK: shouldn't we return buf here instead 0 ?
	if (len==0) return NULL;
	if (rf->pos+len > rf->size) return NULL;
	if (!buf) buf = (guint8*) g_malloc0 (len+1);
	g_return_val_if_fail (buf!=NULL, NULL);
	memcpy (buf, (rf->data)->data+rf->pos, len);
	rf->pos += len;
	return buf;
}

// ramfile_peek
//
// reads in bytes from ramfile from position pos, without changing the current position
//   rf:  ramfile
//   buf: buffer where data is stored to
//        if NULL, buffer will be allocated (0-terminated) and returned (user must free it later)
//
// returns allocated buffer if buf was set NULL, otherwise buf (or NULL if len=0)
//

static guint8 *
ramfile_peek (ramfile *rf, guint8 *buf, gint pos, gint len)
{
	g_return_val_if_fail (rf!=NULL, NULL);
	// CHECK: shouldn't we return buf here instead 0 ?
	if (len==0) return NULL;
	if (pos+len > rf->size) return NULL;
	if (!buf) buf = (guint8*) g_malloc0 (len+1);
	g_return_val_if_fail (buf!=NULL, NULL);
	memcpy (buf, (rf->data)->data+pos, len);
	return buf;
}


static void
ramfile_write (ramfile *rf, guint8 *buf, gint len)
{
	g_return_if_fail (rf!=NULL);
	if (len==0) return;
	g_return_if_fail (buf!=NULL);
	// note: special case might be 2 bytes before end but 4 to write...
	if (rf->pos+len > rf->size)
	{
		g_byte_array_set_size (rf->data, rf->size+len+64); // 64, so save unnecessary allocs
		rf->size = rf->pos+len;		// only increase len if we write over/at the end
	}
	memcpy ((rf->data)->data+rf->pos, buf, len);
	rf->pos += len;
}

static void
ramfile_write_i4_string (ramfile *rf, const gchar *string)
{
	g_return_if_fail (rf!=NULL);
	if (string)
	{
		guint16	len = strlen(string);
		ramfile_write16 (rf, len);
		ramfile_write (rf, (guint8*) string, len);
	} else ramfile_write_i4_string (rf, "");
}


static void
send_message_without_data (guint8 msg)
{
	ramfile	*packet = ramfile_new(1);
//	g_print ("%s - msg=%u\n", __FUNCTION__, msg);
	ramfile_write8 (packet,msg);
	guicoreconn_send_packet (packet);
}

static void
send_login (const gchar *user, const gchar *pass)
{
	ramfile	*packet = ramfile_new(1+2+1+2+1);
	g_return_if_fail (user!=NULL);
	g_return_if_fail (pass!=NULL);
	g_return_if_fail (packet!=NULL);

	ramfile_write8 (packet, CONT_LOGIN);
	ramfile_write_i4_string (packet, user);
	ramfile_write_i4_string (packet, pass);
	guicoreconn_send_packet (packet);
}

static void
send_logout (void)
{
	send_message_without_data (CONT_STOP);
	guicoreconn_status = GUI_CORE_CONN_STATUS_CLOSED_DELIBERATELY;
}

static void
send_get_options (void)
{
	send_message_without_data(GUI_GET_OPTIONS);
}


/******************************************************************************
 * 
 *   main
 *
 */

int
main (int argc, char *argv[])
{
	GInetAddr	*ia;
	gint     	 ret;
	gboolean 	 okay = FALSE;
	GMainLoop	*shutdown_mainloop;

	/* just being hyper-careful... */
	g_return_val_if_fail (sizeof(gfloat)==4, EXIT_FAILURE);
	g_return_val_if_fail (sizeof(gint  )==4, EXIT_FAILURE);
	g_return_val_if_fail (sizeof(guint )==4, EXIT_FAILURE);
	g_return_val_if_fail (sizeof(gchar )==1, EXIT_FAILURE);
	g_return_val_if_fail (sizeof(gshort)==2, EXIT_FAILURE);

	if (argc != 5)
	{
		printf ("\n%s v%s for eDonkey2000 and Overnet cores  - build %s\n", PACKAGE, VERSION, __DATE__);
		printf ("(c) 2003 Tim-Philipp Muller <tim@edonkey2000.com>\n\n");
		printf ("USAGE: ed2k_shutdown <host where core is running> <aport> <username> <password>\n\n");
		printf ("       e.g. ed2k_shutdown localhost 4663 bob bobpass\n\n");
		exit(EXIT_FAILURE);
	}

	shutdown_host = g_strdup(argv[1]);
	shutdown_port = atoi(argv[2]);
	shutdown_user = g_strdup(argv[3]);
	shutdown_pass = g_strdup(argv[4]);

	only_send_shutdown = 1;

	ia = gnet_inetaddr_new (shutdown_host, shutdown_port);
	if (ia)
	{
		guicoreconn = gnet_conn_new_inetaddr (ia, guicoreconn_callback_function, NULL);
		if (guicoreconn)
		{
			gnet_inetaddr_delete (ia);
			okay = TRUE;
		}
	}

	if (!okay)
	{
		g_printerr ( ">>>>> Connect to core failed - couldn't initiate valid connection structure with default values.\n");
		exit (EXIT_FAILURE);
	}

	guicoreconn_status = GUI_CORE_CONN_STATUS_CONNECTING;
	gnet_conn_connect (guicoreconn, GUICORECONN_CONNECT_TIMEOUT);

	shutdown_mainloop = g_main_new (TRUE);

	g_print ( ">>>>> Connecting to core...\n");

	g_main_run (shutdown_mainloop);

	exit (EXIT_SUCCESS);
}
