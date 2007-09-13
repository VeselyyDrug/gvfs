/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* inotify-helper.c - GVFS Monitor based on inotify.

   Copyright (C) 2007 John McCutchan

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Authors: 
		 John McCutchan <john@johnmccutchan.com>
*/

#include "config.h"
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
/* Just include the local header to stop all the pain */
#include "local_inotify.h"
#if 0
#ifdef HAVE_SYS_INOTIFY_H
/* We don't actually include the libc header, because there has been
 * problems with libc versions that was built without inotify support.
 * Instead we use the local version.
 */
#include "local_inotify.h"
#elif defined (HAVE_LINUX_INOTIFY_H)
#include <linux/inotify.h>
#endif
#endif
#include <gio/glocalfile.h>
#include <gio/gfilemonitorpriv.h>
#include <gio/gdirectorymonitorpriv.h>
#include "inotify-helper.h"
#include "inotify-missing.h"
#include "inotify-path.h"
#include "inotify-diag.h"

static gboolean ih_debug_enabled = FALSE;
#define IH_W if (ih_debug_enabled) g_warning 

static void ih_event_callback (ik_event_t *event, inotify_sub *sub);
static void ih_not_missing_callback (inotify_sub *sub);

/* We share this lock with inotify-kernel.c and inotify-missing.c
 *
 * inotify-kernel.c takes the lock when it reads events from
 * the kernel and when it processes those events
 *
 * inotify-missing.c takes the lock when it is scanning the missing
 * list.
 *
 * We take the lock in all public functions
 */
G_LOCK_DEFINE (inotify_lock);

static GDirectoryMonitorEvent ih_mask_to_EventFlags (guint32 mask);

/**
 * Initializes the inotify backend.  This must be called before
 * any other functions in this module.
 *
 * @returns TRUE if initialization succeeded, FALSE otherwise
 */
gboolean
_ih_startup (void)
{
	static gboolean initialized = FALSE;
	static gboolean result = FALSE;

	G_LOCK(inotify_lock);
	
	if (initialized == TRUE) {
		G_UNLOCK(inotify_lock);
		return result;
	}

	result = _ip_startup (ih_event_callback);
	if (!result) {
		g_warning( "Could not initialize inotify\n");
		G_UNLOCK(inotify_lock);
		return FALSE;
	}
	_im_startup (ih_not_missing_callback);
	_id_startup ();

	IH_W ("started gvfs inotify backend\n");

	initialized = TRUE;
	G_UNLOCK(inotify_lock);
	return TRUE;
}

/**
 * Adds a subscription to be monitored.
 */
gboolean
_ih_sub_add (inotify_sub * sub)
{
	G_LOCK(inotify_lock);
	
	if (!_ip_start_watching (sub))
	{
		_im_add (sub);
	}

	G_UNLOCK(inotify_lock);
	return TRUE;
}

/**
 * Cancels a subscription which was being monitored.
 */
gboolean
_ih_sub_cancel (inotify_sub * sub)
{
	G_LOCK(inotify_lock);

	if (!sub->cancelled)
	{
		IH_W("cancelling %s\n", sub->dirname);
		sub->cancelled = TRUE;
		_im_rm (sub);
		_ip_stop_watching (sub);
	}

	G_UNLOCK(inotify_lock);
	return TRUE;
}


static void ih_event_callback (ik_event_t *event, inotify_sub *sub)
{
	gchar *fullpath;
	GDirectoryMonitorEvent eflags;
	GFile* parent;
	GFile* child;

	eflags = ih_mask_to_EventFlags (event->mask);
	parent = g_file_new_for_path (sub->dirname);
	if (event->name)
	{
		fullpath = g_strdup_printf ("%s/%s", sub->dirname, event->name);
	} else {
		fullpath = g_strdup_printf ("%s/", sub->dirname);
	}
	child = g_file_new_for_path (fullpath);
	g_free(fullpath);

	if (G_IS_DIRECTORY_MONITOR(sub->user_data))
	{
		GDirectoryMonitor* monitor = G_DIRECTORY_MONITOR(sub->user_data);
		g_directory_monitor_emit_event (monitor, 
						child, NULL, eflags);
	} else if (G_IS_FILE_MONITOR(sub->user_data))
	{
		GFileMonitor* monitor = G_FILE_MONITOR(sub->user_data);

		g_file_monitor_emit_event (monitor,
					   child, NULL, eflags);
	}

	g_object_unref (child);
	g_object_unref (parent);

}

static void ih_not_missing_callback (inotify_sub *sub)
{
	gchar *fullpath;
	GDirectoryMonitorEvent eflags;
	guint32 mask;
	GFile* parent;
	GFile* child;
	parent = g_file_new_for_path (sub->dirname);

	if (sub->filename)
	{
		fullpath = g_strdup_printf ("%s/%s", sub->dirname, sub->filename);
		g_warning ("Missing callback called fullpath = %s\n", fullpath);
		if (!g_file_test (fullpath, G_FILE_TEST_EXISTS)) {
			g_free (fullpath);
			return;
		}
		mask = IN_CREATE;
	} else {
		fullpath = g_strdup_printf ("%s", sub->dirname);
		mask = IN_CREATE|IN_ISDIR;
	}

	eflags = ih_mask_to_EventFlags (mask);
	child = g_file_new_for_path (fullpath);
	g_free(fullpath);

	if (G_IS_DIRECTORY_MONITOR(sub->user_data))
	{
		GDirectoryMonitor* monitor = G_DIRECTORY_MONITOR(sub->user_data);
		g_directory_monitor_emit_event (monitor, child, NULL, eflags);
	} else if (G_IS_FILE_MONITOR(sub->user_data))
	{
		GFileMonitor* monitor = G_FILE_MONITOR(sub->user_data);
		g_file_monitor_emit_event (monitor,
					   child, NULL, eflags);
	}

	g_object_unref (child);
	g_object_unref (parent);
}

/* Transforms a inotify event to a GVFS event. */
static GDirectoryMonitorEvent
ih_mask_to_EventFlags (guint32 mask)
{
	mask &= ~IN_ISDIR;
	switch (mask)
	{
	case IN_MODIFY:
		return G_DIRECTORY_MONITOR_EVENT_CHANGED;
	break;
	case IN_ATTRIB:
		return G_DIRECTORY_MONITOR_EVENT_ATTRIBUTE_CHANGED;
	break;
	case IN_MOVE_SELF:
	case IN_MOVED_FROM:
	case IN_DELETE:
	case IN_DELETE_SELF:
		return G_DIRECTORY_MONITOR_EVENT_DELETED;
	break;
	case IN_CREATE:
	case IN_MOVED_TO:
		return G_DIRECTORY_MONITOR_EVENT_CREATED;
	break;
	case IN_UNMOUNT:
		return G_DIRECTORY_MONITOR_EVENT_UNMOUNTED;
	case IN_Q_OVERFLOW:
	case IN_OPEN:
	case IN_CLOSE_WRITE:
	case IN_CLOSE_NOWRITE:
	case IN_ACCESS:
	case IN_IGNORED:
	default:
		return -1;
	break;
	}
}
