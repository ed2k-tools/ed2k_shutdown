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

#define GNET_EXPERIMENTAL
#include <gnet.h>

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* package stuff */

#define VERSION		"0.2"
#define PACKAGE		"ed2k_shutdown"


/* protocol stuff */

#define ED2K_BYTE      			0xe3	/* = 227, header byte at beginning of every packet */
#define CONT_LOGIN     			100		/* + i4string: username + i4string: password */
#define CONT_STOP      			101		/* core please stop 			*/
#define CONT_CMD       			102
#define GUI_GET_OPTIONS     225

/* connection stuff */

#define CONNECT_TIMEOUT	   10*1000		 /* 10 seconds */
#define FIRST_READ_TIMEOUT 15*1000     /* 15 seconds */
#define READ_TIMEOUT        5*1000     /*  5 seconds */
#define SHUTDOWN_TIMEOUT    3*1000     /*  3 seconds */

typedef enum
{
 CONN_STATUS_NONE = 0,
 CONN_STATUS_CONNECTING,		// We are trying to connect
 CONN_STATUS_AUTHENTICATING,		// We are trying to log on (user/pass)
 CONN_STATUS_COMMUNICATING,		// We are logged on
 CONN_STATUS_CLOSED_DELIBERATELY,	// We've closed the connection deliberately
 CONN_STATUS_CLOSED_UNEXPECTEDLY,	// The connection broke unexpectedly
 CONN_STATUS_CLOSED_TIMEOUT,		// The connection timed out (no read for X seconds)
 CONN_STATUS_CLOSED_TIMEOUT_NO_DATA,	// The connection timed out and we've never received any data
} ConnStatus;


struct _ConnHelper
{
	guint         packetlen;
	guint8       *packet;
	gboolean      got_header;

	const gchar  *user;
	const gchar  *pass;

	const gchar  *command; /* send logout if this is NULL */
	gboolean      sentcommand;

	GConn        *conn;
	ConnStatus    status;

	guint         outstanding_writes;
};

typedef struct _ConnHelper ConnHelper;



/* functions */

static void     send_login                    (GConn *conn, const gchar *user, const gchar *pass, ConnHelper *ch);

static void     send_shutdown_command         (GConn *conn, ConnHelper *ch);

static void     send_advanced_command         (GConn *conn, ConnHelper *ch);

static void     onConnectionEventRead         (GConn *conn, GConnEvent *event, ConnHelper *ch);

static void     onConnectionEventConnected    (GConn *conn, GConnEvent *event, ConnHelper *ch);

static void     onConnectionEventClosed       (GConn *conn, GConnEvent *event, ConnHelper *ch);

static void     onConnectionEvent             (GConn *conn, GConnEvent *event, ConnHelper* ch);



/***************************************************************************
 *
 *   onConnectionEventRead
 *
 *   The GUI-core connection has some data for us (we are not really
 *    interested in that data at all though and just channel it
 *    directly to /dev/null by doing nothing with it)
 *
 ***************************************************************************/

static void
onConnectionEventRead (GConn *conn, GConnEvent *event, ConnHelper *ch)
{
	g_assert ((event) && (event->buffer) && event->length > 0);

	if (!ch->got_header)
	{
		const guint32 *fourbytes = (guint32*)(event->buffer + 1);

		g_assert (event->length == 5);         /* we have asked for exactly the header */

		if (*((guint8*)(event->buffer)) != 0xE3)    /* ed2k header byte */
		{
			g_error ( "Invalid packet on GUI-core connection - closing connection.\n");
			/* not reached, g_error() will abort. */
		}

		ch->packetlen  = GUINT32_FROM_LE(*fourbytes);
		ch->got_header = TRUE;

//  g_print ("Got Header. Asking for %u more bytes (body)\n", ch->packetlen);

		gnet_conn_readn (conn, ch->packetlen);  /* get body of packet                  */
		gnet_conn_timeout (conn, READ_TIMEOUT); /* valid data received. Renew timeout. */

		/* Received our first bits of data? */
		if (ch->status == CONN_STATUS_AUTHENTICATING)
			ch->status = CONN_STATUS_COMMUNICATING;
	}
	else /* now getting the body of the ed2k-packet */
	{
		g_assert(ch->packet == NULL);

		ch->packet = g_memdup(event->buffer, event->length);

		if (ch->sentcommand == FALSE)
		{
			g_print ( ">>>>> Logged in okay - sending %s command\n", (ch->command != NULL) ? "advanced" : "shutdown");

			if (ch->command == NULL)
				send_shutdown_command(conn, ch);
			else
				send_advanced_command(conn, ch);

			ch->sentcommand = TRUE;
		}

		g_free(ch->packet);
		ch->packet = NULL;

		gnet_conn_readn(conn,5);                /* get header of next packet */
		gnet_conn_timeout (conn, READ_TIMEOUT); /* Valid data received. Renew timeout. */

		ch->got_header = FALSE;
		ch->packetlen = 0;
	}
}

/***************************************************************************
 *
 *   onConnectionEventConnected
 *
 *   The GUI-core connection just got connected
 *
 ***************************************************************************/

static void
onConnectionEventConnected (GConn *conn, GConnEvent *event, ConnHelper *ch)
{
	ch->status = CONN_STATUS_AUTHENTICATING;

	ch->packet     = NULL;
	ch->packetlen  = 0;
	ch->got_header = FALSE;

	send_login (conn, ch->user, ch->pass, ch);

	gnet_conn_readn(conn,5);                     /* get header of next packet */
	gnet_conn_timeout(conn, FIRST_READ_TIMEOUT); /* Set initial timeout.      */
}

/***************************************************************************
 *
 *   onConnectionEventClosed
 *
 *   The GUI-core connection just got closed for some reason
 *
 ***************************************************************************/

static void
onConnectionEventClosed (GConn *conn, GConnEvent *event, ConnHelper *ch)
{
	switch (ch->status)
	{
		case CONN_STATUS_CLOSED_DELIBERATELY:
		case CONN_STATUS_CLOSED_TIMEOUT:
		{
			if (ch->command == NULL)
			{
				g_print (">>>>> Looks like we've shut down the core alright\n");
			}
			else
			{
				sleep(1); /* make sure the stuff gets really sent out */
				g_print (">>>>> Sent advanced command '%s'\n", ch->command);
			}
			exit(EXIT_SUCCESS);
		}

		case CONN_STATUS_CONNECTING:
			g_error(">>>>> Connect to core failed - could not connect!?\n"); /* aborts */

		case CONN_STATUS_AUTHENTICATING:
			g_error(">>>>> Connect to core failed - couldn't log in.\n"); /* aborts */

		case CONN_STATUS_CLOSED_TIMEOUT_NO_DATA:
			g_error(">>>>> Connect to core failed - no data received within 5 seconds!?\n"); /* aborts */

		default:
			g_error(">>>>> GUI-core connection broke unexpectedly before we sent logout! (core crashed/killed?)\n"); /* aborts */
	}

	g_assert_not_reached();
}


/***************************************************************************
 *
 *   get_conn_event_string
 *
 ***************************************************************************/
#if 0
static const gchar *
get_conn_event_string (GConnEvent *event)
{
	switch(event->type)
	{
		case GNET_CONN_ERROR:        return "GNET_CONN_ERROR";
		case GNET_CONN_CONNECT:      return "GNET_CONN_CONNECT";
		case GNET_CONN_CLOSE:        return "GNET_CONN_CLOSE";
		case GNET_CONN_TIMEOUT:      return "GNET_CONN_TIMEOUT";
		case GNET_CONN_READ:         return "GNET_CONN_READ";
		case GNET_CONN_WRITE:        return "GNET_CONN_WRITE";
		case GNET_CONN_READABLE:     return "GNET_CONN_READABLE";
		case GNET_CONN_WRITABLE:     return "GNET_CONN_WRITABLE";
		default:                     return "???";
	}
}
#endif

/***************************************************************************
 *
 *   onConnectionEvent
 *
 *   The callback function for the GUI-core connection
 *
 ***************************************************************************/

static void
onConnectionEvent (GConn *conn, GConnEvent *event, ConnHelper* ch)
{
	g_assert(conn!=NULL);
	g_assert(event != NULL);

//	g_print ("in %s, event->type = %s\n", __FUNCTION__, get_conn_event_string(event));

	switch (event->type)
	{
		case GNET_CONN_CONNECT:
			onConnectionEventConnected (conn, event, ch);
			break;

		case GNET_CONN_READ:
			onConnectionEventRead (conn, event, ch);
			break;

		case GNET_CONN_CLOSE:
		case GNET_CONN_ERROR:
		case GNET_CONN_TIMEOUT:
			onConnectionEventClosed (conn, event, ch);
			break;

		case GNET_CONN_WRITE:
		{
			g_assert (ch->outstanding_writes > 0);

			ch->outstanding_writes--;

			/* in 'send advanced command mode', disconnect when all
			 *  outstanding writes have been processed. */
			if (ch->command != NULL  &&  ch->outstanding_writes == 0   &&  ch->status == CONN_STATUS_COMMUNICATING)
			{
				ch->status = CONN_STATUS_CLOSED_DELIBERATELY;
				gnet_conn_disconnect(conn);
				onConnectionEventClosed (conn, event, ch);
			}
		}
		break;

		case GNET_CONN_WRITABLE:
		case GNET_CONN_READABLE:
			g_return_if_reached();
	}

	return;
}


/******************************************************************************
 *
 *   send_login
 *
 ******************************************************************************/

static void
send_login (GConn *conn, const gchar *user, const gchar *pass, ConnHelper *ch)
{
	guint16  userlen, userlen_LE;
	guint16  passlen, passlen_LE;
	guint32  packetlen, datalen_LE;

	g_assert (user != NULL);
	g_assert (pass != NULL);
	g_assert (conn != NULL);

	userlen   = strlen(user);
	passlen   = strlen(pass);
	packetlen = 5+1+2+userlen+2+passlen;

	userlen_LE = GUINT16_TO_LE(userlen);
	passlen_LE = GUINT16_TO_LE(passlen);
	datalen_LE = GUINT32_TO_LE(packetlen-5);

	/* Note: this section is optimised
	 *  for clarity, not speed - as hard
	 *  to believe as it may seem to some ;) */
	if (userlen > 0 && passlen > 0)
	{
		guint8  packetbuf[packetlen];

		/* 5 bytes header:
		 *  - byte  0:   header byte
		 *  - bytes 1-4: data len without header (little endian) */
		packetbuf[0] = 0xe3;

		memcpy(packetbuf+1, &datalen_LE, 4);

		/* command: login */
		packetbuf[5] = CONT_LOGIN;

		/* username string (2 bytes = stringlen + string without terminator) */
		memcpy(packetbuf+6, &userlen_LE, 2);
		memcpy(packetbuf+6+2, user, userlen);

		/* password string (2 bytes = stringlen + string without terminator) */
		memcpy(packetbuf+6+2+userlen, &passlen_LE, 4);
		memcpy(packetbuf+6+2+userlen+2, pass, passlen);

		gnet_conn_write(conn, packetbuf, packetlen);
		ch->outstanding_writes++;
	}
}


/******************************************************************************
 *
 *   send_shutdown_command
 *
 ******************************************************************************/

static void
send_shutdown_command (GConn *conn, ConnHelper *ch)
{
	guint8  packetbuf[6] = { 0xe3, 0x01, 0x00, 0x00, 0x00, CONT_STOP };

	g_assert(ch != NULL);
	g_assert(conn != NULL);

	gnet_conn_write(conn, packetbuf, 6);
	gnet_conn_timeout(conn, SHUTDOWN_TIMEOUT);

	ch->outstanding_writes++;

	ch->status = CONN_STATUS_CLOSED_DELIBERATELY;
}


/******************************************************************************
 *
 *   send_advanced_command
 *
 ******************************************************************************/

static void
send_advanced_command (GConn *conn, ConnHelper *ch)
{
	guint16  cmdlen, cmdlen_LE;
	guint32  packetlen, datalen_LE;

	g_assert(ch->command != NULL);
	g_assert(conn        != NULL);

	cmdlen = strlen(ch->command);
	cmdlen_LE = GUINT16_TO_LE(cmdlen);

	packetlen  = 5 + 1 + 2 + cmdlen;
	datalen_LE = GUINT32_TO_LE(packetlen-5);

	if (1)
	{
		guint8  packetbuf[packetlen];

		packetbuf[0] = 0xe3;
		memcpy(packetbuf+1, &datalen_LE, 4);
		packetbuf[5] = CONT_CMD;
		memcpy(packetbuf+1+4+1, &cmdlen_LE, 2);
		memcpy(packetbuf+1+4+1+2, ch->command, cmdlen);

		gnet_conn_write(conn, packetbuf, packetlen);
		gnet_conn_timeout(conn, SHUTDOWN_TIMEOUT);

		ch->outstanding_writes++;
	}
}

/******************************************************************************
 *
 *   main
 *
 ******************************************************************************/

gint
main (gint argc, gchar **argv)
{
	const gchar *host;
	ConnHelper  *ch;
	GInetAddr	  *ia;
	GMainLoop	  *mainloop;
	GConn       *conn;
	guint        port;

	g_assert (GNET_CHECK_VERSION(2,0,0) == TRUE);

	if (argc < 5 || argc > 6)
	{
		printf ("\n%s v%s for eDonkey2000 and Overnet cores  - build %s\n", PACKAGE, VERSION, __DATE__);
		printf ("(c) 2003 Tim-Philipp Muller <tim@edonkey2000.com>\n\n");
		printf ("USAGE: ed2k_shutdown <host where core is running> <aport> <username> <password>\n\n");
		printf ("       e.g. ed2k_shutdown localhost 4663 bob bobpass\n\n");
		printf ("\n   -- or --\n\n\n");
		printf ("USAGE: ed2k_shutdown <host> <aport> <username> <password> <command in quotes>\n\n");
		printf ("       e.g. ed2k_shutdown localhost 4663 bob bobpass \"dllink ed2k://|file|foo.txt|61756|6bfe2dca94571e698d95e9bdcf5c63f0|/\"\n\n");
		exit(EXIT_FAILURE);
	}

	host = argv[1];
	port = atoi(argv[2]);

	if (port == 0)
		g_error("Port does not look sane. Type 'ed2k_shutdown --help' for help.\n"); /* aborts */


	ia = gnet_inetaddr_new (host, port);

	if (ia == NULL)
		g_error("Could not resolve hostname '%s'.\n", argv[1]); /* aborts */

	ch = g_new0(ConnHelper,1);

	ch->user    = argv[3];
	ch->pass    = argv[4];
	ch->status  = CONN_STATUS_CONNECTING;

	if (argc == 5)
	{
		ch->command = NULL; /* NULL means: shutdown core */
	}
	else
	{
		ch->command = argv[5];
	}

	conn = gnet_conn_new_inetaddr (ia, (GConnFunc) onConnectionEvent, ch);

	if (conn == NULL)
		g_error("Could not create GConn structure for some reason.");

	ch->conn = conn;

	gnet_conn_timeout(conn, CONNECT_TIMEOUT);
	gnet_conn_connect (conn);

	mainloop = g_main_new (TRUE);

	g_print ( ">>>>> Connecting to core on %s:%u\n", host, port);

	g_main_run (mainloop);

	exit (EXIT_SUCCESS);
}
