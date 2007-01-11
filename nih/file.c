/* libnih
 *
 * file.c - file watching
 *
 * Copyright © 2007 Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#ifdef HAVE_SYS_INOTIFY_H
# include <sys/inotify.h>
#else
# include <nih/inotify.h>
#endif /* HAVE_SYS_INOTIFY_H */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/io.h>
#include <nih/file.h>
#include <nih/logging.h>
#include <nih/error.h>


/**
 * DIR_EVENTS:
 *
 * The standard set of inotify events used for the dir_watcher function.
 **/
#define DIR_EVENTS (IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE | IN_MOVE_SELF)


/* Prototypes for static functions */
static void nih_file_reader        (void *data, NihIo *io,
				    const char *buf, size_t len);
static int  nih_dir_add_file_watch (NihDirWatch *watch, const char *path);
static void nih_dir_watcher        (NihDirWatch *dir_watch,
				    NihFileWatch *file_watch, uint32_t events,
				    uint32_t cookie, const char *name);


/**
 * file_watches:
 *
 * List of all file watches, in no particular order.  Each item is an
 * NihFileWatch structure.
 **/
static NihList *file_watches = NULL;

/**
 * inotify_fd:
 *
 * inotify file descriptor we use for all watches.
 **/
static int inotify_fd = -1;


/**
 * nih_file_init:
 *
 * Initialise the file watches list and inotify file descriptor.
 **/
static inline void
nih_file_init (void)
{
	if (! file_watches)
		NIH_MUST (file_watches = nih_list_new (NULL));

	if (inotify_fd == -1) {
		inotify_fd = inotify_init ();
		if (inotify_fd < 0)
			return;

		NIH_MUST (nih_io_reopen (NULL, inotify_fd, NIH_IO_STREAM,
					 nih_file_reader, NULL, NULL, NULL));
	}
}


/**
 * nih_file_add_watch:
 * @parent: parent of watch,
 * @path: path to watch,
 * @events: events to watch for,
 * @watcher: function to call,
 * @data: data to pass to @watcher.
 *
 * Begins watching @path for the list of @events given which should be a
 * bitmask as described in inotify(7).  When any of the listed events
 * occur, @watcher is called.
 *
 * The watch structure is allocated using nih_alloc() and stored in a linked
 * list, a default destructor is set that removes the watch from the list
 * and terminates the inotify watch.  Removal of the watch can be performed
 * by freeing it.
 *
 * If @parent is not NULL, it should be a pointer to another allocated
 * block which will be used as the parent for this block.  When @parent
 * is freed, the returned string will be freed too.  If you have clean-up
 * that would need to be run, you can assign a destructor function using
 * the nih_alloc_set_destructor() function.
 *
 * Returns: new NihFileWatch structure or NULL on raised error.
 **/
NihFileWatch *
nih_file_add_watch (const void     *parent,
		    const char     *path,
		    uint32_t        events,
		    NihFileWatcher  watcher,
		    void           *data)
{
	NihFileWatch *watch;

	nih_assert (path != NULL);
	nih_assert (events != 0);
	nih_assert (watcher != NULL);

	nih_file_init ();
	if (inotify_fd < 0)
		nih_return_system_error (NULL);

	watch = nih_new (parent, NihFileWatch);
	if (! watch)
		nih_return_system_error (NULL);

	nih_list_init (&watch->entry);
	nih_alloc_set_destructor (watch, (NihDestructor)nih_file_remove_watch);

	watch->wd = inotify_add_watch (inotify_fd, path, events);
	if (watch->wd < 0) {
		nih_error_raise_system ();
		nih_free (watch);
		return NULL;
	}

	watch->path = nih_strdup (watch, path);
	watch->events = events;

	watch->watcher = watcher;
	watch->data = data;

	nih_list_add (file_watches, &watch->entry);

	return watch;
}

/**
 * nih_file_remove_watch:
 * @watch: watch to remove.
 *
 * Remove the watch on the path and events mask associated with the @wwatch
 * given and remove it from the list of watches.
 *
 * The structure itself is not freed.
 **/
void
nih_file_remove_watch (NihFileWatch *watch)
{
	nih_assert (watch != NULL);

	if (watch->wd < 0)
		return;

	inotify_rm_watch (inotify_fd, watch->wd);
	watch->wd = -1;

	nih_list_remove (&watch->entry);
}


/**
 * nih_file_reader:
 * @data: ignored,
 * @io: watch on file descriptor,
 * @buf: buffer bytes available,
 * @len: length of @buf.
 *
 * This function is called whenever data has been read from the inotify
 * file descriptor and is waiting to be processed.  It reads data from the
 * buffer in inotify_event sized chunks, also reading the name afterwards
 * if expected.
 **/
static void
nih_file_reader (void       *data,
		 NihIo      *io,
		 const char *buf,
		 size_t      len)
{
	struct inotify_event *event;
	size_t                sz;

	nih_assert (io != NULL);
	nih_assert (buf != NULL);
	nih_assert (len > 0);

	while (len > 0) {
		/* Wait until there's a complete event waiting
		 * (should always be true, but better to be safe than sorry)
		 */
		sz = sizeof (struct inotify_event);
		if (len < sz)
			return;

		/* Never read an event without its name (again should always be
		 * true
		 */
		event = (struct inotify_event *) buf;
		sz += event->len;
		if (len < sz)
			return;

		/* Read the data (allocates the event structure, etc.)
		 * Force this, otherwise we won't get called until the
		 * next inotify event.
		 */
		NIH_MUST (event = (struct inotify_event *)nih_io_read (
				  NULL, io, &sz));
		len -= sz;

		/* Only call the first watcher that matches.  This is a
		 * limitation of the inotify API, we can't hold multiple
		 * watches on a path and have different watch descriptors
		 * or masks for it.
		 */
		NIH_LIST_FOREACH (file_watches, iter) {
			NihFileWatch *watch = (NihFileWatch *)iter;

			if (watch->wd != event->wd)
				continue;

			watch->watcher (watch->data, watch, event->mask,
					event->cookie,
					event->len ? event->name : NULL);
			break;
		}

		nih_free (event);
	}
}


/**
 * nih_dir_walk:
 * @path: path to walk,
 * @types: object types to call @visitor for,
 * @filter: path filter for both @visitor and iteration,
 * @visitor: function to call for each path,
 * @data: data to pass to @visitor.
 *
 * Iterates the directory tree starting at @path, calling @visitor for
 * each file, directory or other object found.  Sub-directories are
 * descended into, and the same @visitor called for those.
 *
 * @visitor is not called for @path itself.
 *
 * @filter can be used to restrict both the sub-directories iterated and
 * the objects that @visitor is called for.  It is passed the full path
 * of the object, and if it returns TRUE, the object is ignored.
 *
 * @visitor is additionally only called for objects whose type is given
 * in @types, a bitmask of file modes and types as used by stat().
 * Leaving S_IFDIR out of @types only prevents @visitor being called for
 * directories, it does not prevent iteration into sub-directories.
 *
 * Returns: zero on success, negative value on error.
 **/
int
nih_dir_walk (const char    *path,
	      mode_t         types,
	      NihFileFilter  filter,
	      NihFileVisitor visitor,
	      void          *data)
{
	DIR           *dir;
	struct dirent *ent;
	int            ret = 0;

	nih_assert (path != NULL);
	nih_assert (types != 0);
	nih_assert (visitor != NULL);

	dir = opendir (path);
	if (! dir)
		nih_return_system_error (-1);

	while ((ent = readdir (dir)) != NULL) {
		struct stat  statbuf;
		char        *subpath;

		/* Always ignore '.' and '..' */
		if ((! strcmp (ent->d_name, "."))
		    || (! strcmp (ent->d_name, "..")))
			continue;

		NIH_MUST (subpath = nih_sprintf (NULL, "%s/%s",
						 path, ent->d_name));

		/* Check the filter */
		if (filter && filter (subpath)) {
			nih_free (subpath);
			continue;
		}

		/* Not much we can do here if we can't at least stat it */
		if (stat (subpath, &statbuf) < 0) {
			nih_free (subpath);
			continue;
		}

		/* Call the handler if types match. */
		if (statbuf.st_mode & types) {
			ret = visitor (data, subpath);
			if (ret < 0) {
				nih_free (subpath);
				break;
			}
		}

		/* Iterate into sub-directories */
		if (S_ISDIR (statbuf.st_mode)) {
			ret = nih_dir_walk (subpath, types, filter,
					    visitor, data);
			if (ret < 0) {
				nih_free (subpath);
				break;
			}
		}

		nih_free (subpath);
	}

	closedir (dir);

	return ret;
}


/**
 * nih_dir_add_watch:
 * @parent: parent of watch,
 * @path: path to watch,
 * @subdirs: recurse into sub-directories,
 * @filter: filter function,
 * @create_handler: function to call when a file is created,
 * @modify_handler: function to call when a file is changed,
 * @delete_handler: function to call when a file is deleted,
 * @data: data to pass to functions.
 *
 * Begins watching the @path directory for any changes to its contents
 * and, if @subdirs is TRUE, any sub-directories as well.
 *
 * This abstracts almost all inotify handling to three basic operations;
 * files being created or added to the directory, files being modified
 * and files being deleted or removed from the directory.
 *
 * If @subdirs is TRUE, operations on sub-directories are handled
 * automatically using the same watch structure.
 *
 * If @filter is given, it is called on any file before the handler
 * function is called and must return FALSE for the function to be
 * called.
 *
 * If you need finer-grained control, use nih_file_add_watch() instead.
 *
 * The directory watch structure is allocated using nih_alloc(), all
 * associated child watches are allocated as children and retain their
 * destructor, so the watch can be destroyed by treeing it which will
 * automatically cancel any watches.
 *
 * If the directory being watched is deleted or renamed, @delete_handler
 * is called with a NULL name.
 *
 * If @parent is not NULL, it should be a pointer to another allocated
 * block which will be used as the parent for this block.  When @parent
 * is freed, the returned string will be freed too.  If you have clean-up
 * that would need to be run, you can assign a destructor function using
 * the nih_alloc_set_destructor() function.
 *
 * Returns: new NihDirWatch structure or NULL on raised error.
 **/
NihDirWatch *
nih_dir_add_watch (const void       *parent,
		   const char       *path,
		   int               subdirs,
		   NihFileFilter     filter,
		   NihCreateHandler  create_handler,
		   NihModifyHandler  modify_handler,
		   NihDeleteHandler  delete_handler,
		   void             *data)
{
	NihDirWatch *watch;
	struct stat  statbuf;

	nih_assert (path != NULL);

	if (stat (path, &statbuf) < 0)
		nih_return_system_error (NULL);

	if (! S_ISDIR (statbuf.st_mode)) {
		errno = ENOTDIR;
		nih_return_system_error (NULL);
	}

	watch = nih_new (parent, NihDirWatch);
	if (! watch)
		nih_return_system_error (NULL);

	watch->path = nih_strdup (watch, path);
	watch->subdirs = subdirs;

	watch->filter = filter;
	watch->create_handler = create_handler;
	watch->modify_handler = modify_handler;
	watch->delete_handler = delete_handler;
	watch->data = data;

	/* Add a file watch for the top-level */
	if (! nih_file_add_watch (watch, path, DIR_EVENTS,
				  (NihFileWatcher)nih_dir_watcher, watch)) {
		nih_free (watch);

		return NULL;
	}

	/* Only warn if we have a problem iterating */
	if (subdirs
	    && (nih_dir_walk (path, S_IFDIR, watch->filter,
			      (NihFileVisitor)nih_dir_add_file_watch,
			      watch) < 0)) {
		nih_free (watch);

		return NULL;
	}

	return watch;
}

/**
 * nih_dir_add_file_watch:
 * @watch: parent NihDirWatch,
 * @path: path under @watch to be added.
 *
 * This function is called for each sub-directory iterated while creating
 * an NihDirWatch.  It creates an NihFileWatch for @path as a child of
 * @watch, should that fail, it only emits a warning.
 *
 * Returns: zero on success, negative value on error.
 **/
static int
nih_dir_add_file_watch (NihDirWatch *watch,
			const char  *path)
{
	if (! nih_file_add_watch (watch, path, DIR_EVENTS,
				  (NihFileWatcher)nih_dir_watcher, watch)) {
		NihError *err;

		err = nih_error_get ();
		nih_warn ("%s: %s: %s", path,
			  _("Unable to watch directory"), err->message);
		nih_free (err);
	}

	return 0;
}

/**
 * nih_dir_watcher:
 * @dir_watch: NihDirWatch for top-level,
 * @file_watch: NihFileWatch for which an event occurred,
 * @events: event mask that occurred,
 * @cookie: rename cookie,
 * @name: optional filename.
 *
 * This function is called for every inotify event within the directory tree
 * being watched by @dir_watch; a combination of the information in
 * @file_watch (which is for the actual sub-directory) and @name is used
 * to call the most appropriate @dir_watch handler function.
 **/
static void
nih_dir_watcher (NihDirWatch  *dir_watch,
		 NihFileWatch *file_watch,
		 uint32_t      events,
		 uint32_t      cookie,
		 const char   *name)
{
	char *path;

	nih_assert (dir_watch != NULL);
	nih_assert (file_watch != NULL);
	nih_assert (events != 0);

	/* Find out whether the event is that the directory we're watching
	 * has gone away or been moved.  We treat being moved specially
	 * because we automatically rearrange watches for such things.
	 */
	if ((events & IN_IGNORED) || (events & IN_MOVE_SELF)) {
		nih_debug ("Ceasing watch on %s", file_watch->path);

		if (! strcmp (file_watch->path, dir_watch->path)) {
			/* Top-level directory has gone away, call the
			 * delete handler with the special NULL argument,
			 * then free the watch.
			 */
			if (dir_watch->delete_handler)
				dir_watch->delete_handler (dir_watch->data,
							   dir_watch, NULL);

			nih_free (dir_watch);
		} else {
			int found = 0;

			/* A lower level directory has gone away; we need to
			 * free it up, but we don't want to remove the watch
			 * if someone else has taken it up!
			 */
			NIH_LIST_FOREACH (file_watches, iter) {
				NihFileWatch *fw = (NihFileWatch *)iter;

				if ((fw != file_watch)
				    && (fw->wd == file_watch->wd))
					found = 1;
			}

			if (found) {
				nih_alloc_set_destructor (file_watch, NULL);
				nih_list_free (&file_watch->entry);
			} else {
				nih_file_remove_watch (file_watch);
				nih_free (file_watch);
			}
		}

		return;
	}

	/* Every other event must come with a name. */
	nih_assert (name != NULL);
	NIH_MUST (path = nih_sprintf (dir_watch, "%s/%s",
				      file_watch->path, name));

	/* Filter out unwanted paths */
	if (dir_watch->filter && dir_watch->filter (path))
		goto finish;


	if ((events & IN_CREATE) || (events & IN_MOVED_TO)) {
		struct stat statbuf;

		if (dir_watch->create_handler)
			dir_watch->create_handler (dir_watch->data, dir_watch,
						   path);

		/* If we're watching an entire directory tree, make sure we
		 * add a watch for this new sub directory and anything under
		 * it too.
		 */
		if (dir_watch->subdirs
		    && (stat (path, &statbuf) == 0)
		    && S_ISDIR (statbuf.st_mode)) {
			nih_dir_add_file_watch (dir_watch, path);

			if (nih_dir_walk (path, S_IFDIR, dir_watch->filter,
					  (NihFileVisitor)nih_dir_add_file_watch,
					  dir_watch) < 0)
				nih_free (nih_error_get ());
		}

	} else if (events & IN_MODIFY) {
		if (dir_watch->modify_handler)
			dir_watch->modify_handler (dir_watch->data, dir_watch,
						   path);

	} else if ((events & IN_DELETE) || (events & IN_MOVED_FROM)) {
		if (dir_watch->delete_handler)
			dir_watch->delete_handler (dir_watch->data, dir_watch,
						   path);
	}

finish:
	nih_free (path);
}


/**
 * nih_file_map:
 * @path: path to open,
 * @flags: open mode,
 * @length: pointer to store file length.
 *
 * Opens the file at @path and maps it into memory, returning the mapped
 * pointer and the length of the file (required to unmap it later).  The
 * file is opened with the @flags given.
 *
 * Returns: memory mapped file or NULL on raised error.
 **/
void *
nih_file_map (const char *path,
	      int         flags,
	      size_t     *length)
{
	struct stat  statbuf;
	char        *map;
	int          fd, prot;

	nih_assert (path != NULL);
	nih_assert (length != NULL);

	nih_assert (((flags & O_ACCMODE) == O_RDONLY)
		    || ((flags & O_ACCMODE) == O_RDWR));

	fd = open (path, flags);
	if (fd < 0)
		nih_return_system_error (NULL);

	prot = PROT_READ;
	if ((flags & O_ACCMODE) == O_RDWR)
		prot |= PROT_WRITE;

	if (fstat (fd, &statbuf) < 0)
		goto error;

	*length = statbuf.st_size;

	map = mmap (NULL, *length, prot, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		goto error;

	close (fd);
	return map;
error:
	nih_error_raise_system ();
	close (fd);
	return NULL;
}

/**
 * nih_file_unmap:
 * @map: memory mapped file,
 * @length: length of file.
 *
 * Unmap a file previously mapped with nih_file_map().
 *
 * Returns: zero on success, NULL on raised error.
 **/
int
nih_file_unmap (void   *map,
		size_t  length)
{
	nih_assert (map != NULL);

	if (munmap (map, length) < 0)
		nih_return_system_error (-1);

	return 0;
}
