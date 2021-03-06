/*
 * ra_plugin.c : the main RA module for local repository access
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include "ra_local.h"
#include "svn_ra.h"
#include "svn_fs.h"
#include "svn_delta.h"
#include "svn_repos.h"
#include "svn_pools.h"

#define APR_WANT_STRFUNC
#include <apr_want.h>

/*----------------------------------------------------------------*/

/** Callbacks **/

/* A device to record the targets of commits, and ensuring that proper
   commit closure happens on them (namely, revision setting and wc
   property setting).  This is passed to the `commit hook' routine by
   svn_fs_get_editor.  (   ) */
struct commit_cleanup_baton
{
  /* Allocation for this baton, as well as all committed_targets */
  apr_pool_t *pool;

  /* The filesystem that we just committed to. */
  svn_fs_t *fs;

  /* If non-null, store the new revision here. */
  svn_revnum_t *new_rev;

  /* If non-null, store the repository date of the commit here. */
  const char **committed_date;

  /* If non-null, store the repository author of the commit here. */
  const char **committed_author;
};


/* An instance of svn_ra_local__commit_hook_t.
 * 
 * BATON is `struct commit_cleanup_baton *'.  Loop over all committed
 * target paths in BATON->committed_targets, invoking
 * BATON->close_func() on each one with NEW_REV.
 *
 * Set *(BATON->new_rev) to NEW_REV, and copy COMMITTED_DATE and
 * COMMITTED_AUTHOR to *(BATON->committed_date) and
 * *(BATON->committed_date) respectively, allocating the new storage
 * in BATON->pool.
 *
 * This routine is originally passed as a "hook" to the filesystem
 * commit editor.  When we get here, the track-editor has already
 * stored committed targets inside the baton.
 */
static svn_error_t *
cleanup_commit (svn_revnum_t new_rev,
                const char *committed_date,
                const char *committed_author,
                void *baton)
{
  struct commit_cleanup_baton *cb = baton;

  /* Store the new revision information in the baton. */
  if (cb->new_rev)
    *(cb->new_rev) = new_rev;

  if (cb->committed_date)
    *(cb->committed_date) = committed_date 
                            ? apr_pstrdup (cb->pool, committed_date) 
                            : NULL;

  if (cb->committed_author)
    *(cb->committed_author) = committed_author
                              ? apr_pstrdup (cb->pool, committed_author) 
                              : NULL;

  return SVN_NO_ERROR;
}



/* The reporter vtable needed by do_update() */

static const svn_ra_reporter_t ra_local_reporter = 
{
  svn_repos_set_path,
  svn_repos_delete_path,
  svn_repos_finish_report,
  svn_repos_abort_report
};



/*----------------------------------------------------------------*/

/** The RA plugin routines **/


static svn_error_t *
svn_ra_local__open (void **session_baton,
                    svn_stringbuf_t *repos_URL,
                    const svn_ra_callbacks_t *callbacks,
                    void *callback_baton,
                    apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *session;
  void *a, *auth_baton;
  svn_ra_username_authenticator_t *authenticator;

  /* Allocate and stash the session_baton args we have already. */
  session = apr_pcalloc (pool, sizeof(*session));
  session->pool = pool;
  session->repository_URL = repos_URL;
  
  /* Get the username by "pulling" it from the callbacks. */
  SVN_ERR (callbacks->get_authenticator (&a,
                                         &auth_baton, 
                                         svn_ra_auth_username, 
                                         callback_baton, pool));

  authenticator = (svn_ra_username_authenticator_t *) a;

  SVN_ERR (authenticator->get_username (&(session->username),
                                        auth_baton, FALSE, pool));

  /* Look through the URL, figure out which part points to the
     repository, and which part is the path *within* the
     repository. */
  SVN_ERR_W (svn_ra_local__split_URL (&(session->repos_path),
                                      &(session->fs_path),
                                      session->repository_URL,
                                      session->pool),
             "Unable to open an ra_local session to URL");

  /* Open the filesystem at located at environment `repos_path' */
  SVN_ERR (svn_repos_open (&(session->repos),
                           session->repos_path->data,
                           session->pool));

  /* Cache the filesystem object from the repos here for
     convenience. */
  session->fs = svn_repos_fs (session->repos);

  /* Stuff the callbacks/baton here. */
  session->callbacks = callbacks;
  session->callback_baton = callback_baton;

  /* ### ra_local is not going to bother to store the username in the
     working copy.  This means that the username will always be
     fetched from getuid() or from a commandline arg, which is fine.

     The reason for this decision is that in ra_local, authentication
     and authorization are blurred; we'd have to use authorization as
     a *test* to decide if the authentication was valid.  And we
     certainly don't want to track every subsequent svn_fs_* call's
     error, just to decide if it's legitmate to store a username! */

  *session_baton = session;
  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__close (void *session_baton)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  /* Close the repository, which will free any memory used by it. */
  SVN_ERR (svn_repos_close (baton->repos));
  
  /* NULL out the FS cache so no one is tempted to use it again. */
  baton->fs = NULL;

  return SVN_NO_ERROR;
}




static svn_error_t *
svn_ra_local__get_latest_revnum (void *session_baton,
                                 svn_revnum_t *latest_revnum)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_fs_youngest_rev (latest_revnum, baton->fs, baton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__get_dated_revision (void *session_baton,
                                  svn_revnum_t *revision,
                                  apr_time_t tm)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_repos_dated_revision (revision, baton->repos, tm, baton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__get_commit_editor (void *session_baton,
                                 const svn_delta_editor_t **editor,
                                 void **edit_baton,
                                 svn_revnum_t *new_rev,
                                 const char **committed_date,
                                 const char **committed_author,
                                 svn_stringbuf_t *log_msg)
{
  svn_ra_local__session_baton_t *sess_baton = session_baton;
  struct commit_cleanup_baton *cb
    = apr_pcalloc (sess_baton->pool, sizeof (*cb));

  /* Construct a commit cleanup baton */
  cb->pool = sess_baton->pool;
  cb->fs = sess_baton->fs;
  cb->new_rev = new_rev;
  cb->committed_date = committed_date;
  cb->committed_author = committed_author;
                                         
  /* Get the repos commit-editor */     
  SVN_ERR (svn_ra_local__get_editor (editor, edit_baton, sess_baton,
                                     log_msg, cleanup_commit, cb,
                                     sess_baton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__do_checkout (void *session_baton,
                           svn_revnum_t revision,
                           svn_boolean_t recurse,
                           const svn_delta_edit_fns_t *editor,
                           void *edit_baton)
{
  svn_revnum_t revnum_to_fetch;
  svn_ra_local__session_baton_t *sbaton = 
    (svn_ra_local__session_baton_t *) session_baton;
  
  if (! SVN_IS_VALID_REVNUM(revision))
    SVN_ERR (svn_ra_local__get_latest_revnum (sbaton, &revnum_to_fetch));
  else
    revnum_to_fetch = revision;

  SVN_ERR (svn_ra_local__checkout (sbaton->fs,
                                   revnum_to_fetch,
                                   recurse,
                                   sbaton->repository_URL,
                                   sbaton->fs_path,
                                   editor, edit_baton, sbaton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__do_update (void *session_baton,
                         const svn_ra_reporter_t **reporter,
                         void **report_baton,
                         svn_revnum_t update_revision,
                         svn_stringbuf_t *update_target,
                         svn_boolean_t recurse,
                         const svn_delta_edit_fns_t *update_editor,
                         void *update_baton)
{
  svn_delta_edit_fns_t *pipe_editor;
  struct svn_pipe_edit_baton *pipe_edit_baton;
  svn_revnum_t revnum_to_update_to;
  svn_stringbuf_t *switch_path;
  svn_ra_local__session_baton_t *sbaton = session_baton;

  /* ### fix the update_target param at some point */
  const char *target;
  target = update_target ? update_target->data : NULL;

  /* We want dir_delta to run on -identical- fs paths. */
  switch_path = 
    svn_stringbuf_create_from_string (sbaton->fs_path, sbaton->pool);
  if (target)
    svn_path_add_component_nts (switch_path, target);
  
  if (! SVN_IS_VALID_REVNUM(update_revision))
    SVN_ERR (svn_ra_local__get_latest_revnum (sbaton, &revnum_to_update_to));
  else
    revnum_to_update_to = update_revision;

  /* Wrap UPDATE_EDITOR with a custom "pipe" editor that pushes extra
     'entry' properties into the stream, whenever {open_root,
     open_file, open_dir, add_file, add_dir} are called.  */
  SVN_ERR (svn_ra_local__get_update_pipe_editor 
           (&pipe_editor,
            &pipe_edit_baton,
            update_editor,
            update_baton,
            sbaton,
            svn_stringbuf_create_from_string (sbaton->fs_path, sbaton->pool),
            sbaton->pool));

  /* Pass back our reporter */
  *reporter = &ra_local_reporter;

  /* Build a reporter baton. */
  return svn_repos_begin_report (report_baton,
                                 revnum_to_update_to,
                                 sbaton->username,
                                 sbaton->repos, 
                                 sbaton->fs_path->data,
                                 target, 
                                 switch_path->data,
                                 TRUE, /* send text-deltas */
                                 recurse,
                                 pipe_editor, pipe_edit_baton,
                                 sbaton->pool);
}


static svn_error_t *
svn_ra_local__do_switch (void *session_baton,
                         const svn_ra_reporter_t **reporter,
                         void **report_baton,
                         svn_revnum_t update_revision,
                         svn_stringbuf_t *update_target,
                         svn_boolean_t recurse,
                         svn_stringbuf_t *switch_url,
                         const svn_delta_edit_fns_t *update_editor,
                         void *update_baton)
{
  svn_delta_edit_fns_t *pipe_editor;
  struct svn_pipe_edit_baton *pipe_edit_baton;
  svn_revnum_t revnum_to_update_to;
  const svn_string_t *switch_repos_path, *switch_fs_path;
  svn_ra_local__session_baton_t *sbaton = session_baton;
  svn_stringbuf_t *pipe_anchor;

  /* ### fix the update_target param at some point */
  const char *target;
  target = update_target ? update_target->data : NULL;
  
  /* Pull the relevant fs-path portion out of switch_url. */
  SVN_ERR_W (svn_ra_local__split_URL (&switch_repos_path, &switch_fs_path,
                                      switch_url, sbaton->pool),
             "The 'switch' URL is invalid.");

  /* Sanity check:  the switch_url better be in the same repository as
     the original session url! */
  if (! svn_string_compare (sbaton->repos_path, switch_repos_path))
    return svn_error_createf (SVN_ERR_RA_ILLEGAL_URL, 0, NULL, sbaton->pool,
                              "'%s'\n"
                              "is not the same repository as\n"
                              "'%s'", switch_repos_path->data,
                              sbaton->repos_path->data);

  if (! SVN_IS_VALID_REVNUM(update_revision))
    SVN_ERR (svn_ra_local__get_latest_revnum (sbaton, &revnum_to_update_to));
  else
    revnum_to_update_to = update_revision;

  /* Make sure the pipe editor is anchored in the same way as the
     update editor. */

  /* Assume that we should anchor the pipe editor on the switch path
     directly.  This is normal when switching a directory, since
     tgt-anchor is the directory itself, and tgt-target is NULL. */
  pipe_anchor = svn_stringbuf_create_from_string (switch_fs_path, 
                                                  sbaton->pool);
  if (update_target)
    {
      /* If the target is defined, then we must be switching a file.
         
         The pipe editor needs to be anchored on the target's parent
         directory.  But here's the catch: the pipe-editor is going to
         receive open_file(src-basename), because there's no
         delete/add happening.  Somehow the pipe-editor needs to fetch
         the CR from *tgt*-basename.  So we stash it in the
         pipe-editor's own baton. ### do this.
       */
      svn_path_remove_component (pipe_anchor);
    }  

  /* Wrap UPDATE_EDITOR with a custom "pipe" editor that pushes extra
     'entry' properties into the stream, whenever {open_root,
     open_file, open_dir, add_file, add_dir} are called.  */
  SVN_ERR (svn_ra_local__get_update_pipe_editor 
           (&pipe_editor,
            &pipe_edit_baton,
            update_editor,
            update_baton,
            sbaton,
            pipe_anchor,
            sbaton->pool));

  /* Pass back our reporter */
  *reporter = &ra_local_reporter;

  /* Build a reporter baton. */
  return svn_repos_begin_report (report_baton,
                                 revnum_to_update_to,
                                 sbaton->username,
                                 sbaton->repos, 
                                 sbaton->fs_path->data,
                                 target,
                                 switch_fs_path->data,
                                 TRUE, /* we want text-deltas */
                                 recurse,
                                 pipe_editor, pipe_edit_baton,
                                 sbaton->pool);
}



static svn_error_t *
svn_ra_local__do_status (void *session_baton,
                         const svn_ra_reporter_t **reporter,
                         void **report_baton,
                         svn_stringbuf_t *status_target,
                         svn_boolean_t recurse,
                         const svn_delta_edit_fns_t *status_editor,
                         void *status_baton)
{
  svn_revnum_t revnum_to_update_to;
  svn_stringbuf_t *switch_path;
  svn_ra_local__session_baton_t *sbaton = session_baton;

  /* ### fix the status_target param at some point */
  const char *target;
  target = status_target ? status_target->data : NULL;

  /* We want dir_delta to run on -identical- fs paths. */
  switch_path =
    svn_stringbuf_create_from_string (sbaton->fs_path, sbaton->pool);
  if (target)
    svn_path_add_component_nts (switch_path, target);

  SVN_ERR (svn_ra_local__get_latest_revnum (sbaton, &revnum_to_update_to));

  /* Pass back our reporter */
  *reporter = &ra_local_reporter;

  /* Build a reporter baton. */
  return svn_repos_begin_report (report_baton,
                                 revnum_to_update_to,
                                 sbaton->username,
                                 sbaton->repos, 
                                 sbaton->fs_path->data,
                                 target,
                                 switch_path->data,
                                 FALSE, /* don't send text-deltas */
                                 recurse,
                                 status_editor, status_baton,
                                 sbaton->pool);
}


static svn_error_t *
svn_ra_local__get_log (void *session_baton,
                       const apr_array_header_t *paths,
                       svn_revnum_t start,
                       svn_revnum_t end,
                       svn_boolean_t discover_changed_paths,
                       svn_log_message_receiver_t receiver,
                       void *receiver_baton)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  apr_array_header_t *abs_paths
    = apr_array_make (sbaton->pool, paths->nelts, sizeof (svn_stringbuf_t *));
  int i;

  for (i = 0; i < paths->nelts; i++)
    {
      svn_stringbuf_t *relative_path
        = (((svn_stringbuf_t **)(paths)->elts)[i]);

      svn_stringbuf_t *abs_path
        = svn_stringbuf_create_from_string (sbaton->fs_path, sbaton->pool);

      /* ### Not sure if this counts as a workaround or not.  The
         session baton uses the empty string to mean root, and not
         sure that should change.  However, it would be better to use
         a path library function to add this separator -- hardcoding
         it is totally bogus.  See issue #559, though it may be only
         tangentially related. */
      if (abs_path->len == 0)
        svn_stringbuf_appendcstr (abs_path, "/");

      svn_path_add_component (abs_path, relative_path);
      (*((svn_stringbuf_t **)(apr_array_push (abs_paths)))) = abs_path;
    }

  return svn_repos_get_logs (sbaton->repos,
                             abs_paths,
                             start,
                             end,
                             discover_changed_paths,
                             receiver,
                             receiver_baton,
                             sbaton->pool);
}


static svn_error_t *
svn_ra_local__do_check_path (svn_node_kind_t *kind,
                             void *session_baton,
                             const char *path,
                             svn_revnum_t revision)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  svn_fs_root_t *root;
  svn_stringbuf_t *abs_path 
    = svn_stringbuf_create_from_string (sbaton->fs_path, sbaton->pool);

  /* ### Not sure if this counts as a workaround or not.  The
     session baton uses the empty string to mean root, and not
     sure that should change.  However, it would be better to use
     a path library function to add this separator -- hardcoding
     it is totally bogus.  See issue #559, though it may be only
     tangentially related. */
  if (abs_path->len == 0)
    svn_stringbuf_appendcstr (abs_path, "/");

  /* If we were given a relative path to append, append it. */
  if (path)
    svn_path_add_component_nts (abs_path, path);

  if (! SVN_IS_VALID_REVNUM (revision))
    SVN_ERR (svn_fs_youngest_rev (&revision, sbaton->fs, sbaton->pool));
  SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, revision, sbaton->pool));
  *kind = svn_fs_check_path (root, abs_path->data, sbaton->pool);
  return SVN_NO_ERROR;
}



/* Getting just one file. */
static svn_error_t *
svn_ra_local__get_file (void *session_baton,
                        const char *path,
                        svn_revnum_t revision,
                        svn_stream_t *stream,
                        svn_revnum_t *fetched_rev,
                        apr_hash_t **props)
{
  svn_fs_root_t *root;
  svn_stream_t *contents;
  svn_revnum_t youngest_rev;
  char buf[SVN_STREAM_CHUNK_SIZE];
  apr_size_t rlen, wlen;
  svn_ra_local__session_baton_t *sbaton = session_baton;

  svn_stringbuf_t *abs_path 
    = svn_stringbuf_create_from_string (sbaton->fs_path, sbaton->pool);

  /* ### Not sure if this counts as a workaround or not.  The
     session baton uses the empty string to mean root, and not
     sure that should change.  However, it would be better to use
     a path library function to add this separator -- hardcoding
     it is totally bogus.  See issue #559, though it may be only
     tangentially related. */
  if (abs_path->len == 0)
    svn_stringbuf_appendcstr (abs_path, "/");

  /* If we were given a relative path to append, append it. */
  if (path)
    svn_path_add_component_nts (abs_path, path);

  /* Open the revision's root. */
  if (! SVN_IS_VALID_REVNUM (revision))
    {
      SVN_ERR (svn_fs_youngest_rev (&youngest_rev, sbaton->fs, sbaton->pool));
      SVN_ERR (svn_fs_revision_root (&root, sbaton->fs,
                                     youngest_rev, sbaton->pool));
      if (fetched_rev != NULL)
        *fetched_rev = youngest_rev;
    }
  else
    SVN_ERR (svn_fs_revision_root (&root, sbaton->fs,
                                   revision, sbaton->pool));

  /* Get a stream representing the file's contents. */
  SVN_ERR (svn_fs_file_contents (&contents, root,
                                 abs_path->data, sbaton->pool));

  /* Now push data from the fs stream back at the caller's stream. */
  while (1)
    {
      /* read a maximum number of bytes from the file, please. */
      rlen = SVN_STREAM_CHUNK_SIZE; 
      SVN_ERR (svn_stream_read (contents, buf, &rlen));

      /* write however many bytes you read, please. */
      wlen = rlen;
      SVN_ERR (svn_stream_write (stream, buf, &wlen));
      if (wlen != rlen)
        {
          /* Uh oh, didn't write as many bytes as we read, and no
             error was returned.  According to the docstring, this
             should never happen. */
          return 
            svn_error_create (SVN_ERR_UNEXPECTED_EOF, 0, NULL,
                              sbaton->pool, "Error writing to svn_stream.");
        }
      
      if (rlen != SVN_STREAM_CHUNK_SIZE)
        {
          /* svn_stream_read didn't throw an error, yet it didn't read
             all the bytes requested.  According to the docstring,
             this means a plain old EOF happened, so we're done. */
          break;
        }
    }

  if (props)
    {
      svn_revnum_t committed_rev;
      svn_string_t *committed_date, *last_author;
      svn_stringbuf_t *value;
      svn_string_t *abs_path_s;
      char *revision_str = NULL;

      /* Create a hash with props attached to the fs node. */
      SVN_ERR (svn_fs_node_proplist (props, root, abs_path->data,
                                     sbaton->pool));
      
      /* Now add some non-tweakable metadata to the hash as well... */
    
      /* The so-called 'entryprops' with info about CR & friends. */
      abs_path_s = svn_string_create_from_buf (abs_path, sbaton->pool);
      SVN_ERR (svn_repos_get_committed_info (&committed_rev,
                                             &committed_date,
                                             &last_author,
                                             root, abs_path_s,
                                             sbaton->pool));


      revision_str = apr_psprintf (sbaton->pool, "%ld", committed_rev);
      value = svn_stringbuf_create (revision_str, sbaton->pool);
      apr_hash_set (*props, SVN_PROP_ENTRY_COMMITTED_REV, 
                    APR_HASH_KEY_STRING, value);
                    
      if (committed_date)
        value = svn_stringbuf_create_from_string (committed_date,
                                                  sbaton->pool);
      else
        value = NULL;
      apr_hash_set (*props, SVN_PROP_ENTRY_COMMITTED_DATE, 
                    APR_HASH_KEY_STRING, value);
      
      if (last_author)
        value = svn_stringbuf_create_from_string (last_author, sbaton->pool);
      else
        value = NULL;
      apr_hash_set (*props, SVN_PROP_ENTRY_LAST_AUTHOR, 
                    APR_HASH_KEY_STRING, value);
            
      /* We have no 'wcprops' in ra_local, but might someday. */
    }
  return SVN_NO_ERROR;
}


/*----------------------------------------------------------------*/

/** The ra_plugin **/

static const svn_ra_plugin_t ra_local_plugin = 
{
  "ra_local",
  "Module for accessing a repository on local disk.",
  svn_ra_local__open,
  svn_ra_local__close,
  svn_ra_local__get_latest_revnum,
  svn_ra_local__get_dated_revision,
  svn_ra_local__get_commit_editor,
  svn_ra_local__get_file,
  svn_ra_local__do_checkout,
  svn_ra_local__do_update,
  svn_ra_local__do_switch,
  svn_ra_local__do_status,
  NULL,
  svn_ra_local__get_log,
  svn_ra_local__do_check_path
};


/*----------------------------------------------------------------*/

/** The One Public Routine, called by libsvn_client **/

svn_error_t *
svn_ra_local_init (int abi_version,
                   apr_pool_t *pool,
                   apr_hash_t *hash)
{
  apr_hash_set (hash, "file", APR_HASH_KEY_STRING, &ra_local_plugin);

  /* ben sez:  todo:  check that abi_version >=1. */

  return SVN_NO_ERROR;
}








/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
