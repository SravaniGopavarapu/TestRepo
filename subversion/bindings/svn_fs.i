/*
 * svn_fs.i :  SWIG interface file for svn_fs.h
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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

%module _fs
%include typemaps.i

%import apr.i
%import svn_types.i
%import svn_string.i
%import svn_delta.i

/* -----------------------------------------------------------------------
   these types (as 'type **') will always be an OUT param
*/
OUT_PARAM(svn_fs_txn_t);
OUT_PARAM(svn_fs_root_t);
OUT_PARAM(void);
OUT_PARAM(svn_fs_id_t);
OUT_PARAM(svn_stream_t);

/* ### need to deal with IN params which have "const" and OUT params which
   ### return non-const type. SWIG's type checking may see these as
   ### incompatible. */

/* -----------------------------------------------------------------------
   for the FS, 'int *' will always be an OUTPUT parameter
*/
%typemap(in) int * = int *OUTPUT;
%typemap(ignore) int * = int *OUTPUT;
%typemap(argout) int * = int *OUTPUT;

/* -----------------------------------------------------------------------
   tweak the argument handling for svn_fs_parse_id

   note: the string passed will not have null chars, so strlen() is fine.
*/

%typemap(ignore) apr_size_t len { }
%typemap(check) apr_size_t len {
    $target = strlen(arg0);
}

/* -----------------------------------------------------------------------
   all uses of "const char **" are returning strings
*/

%typemap(ignore) const char ** = const char **OUTPUT;
%typemap(argout) const char ** = const char **OUTPUT;

/* -----------------------------------------------------------------------
   list_transaction's "char ***" is returning a list of strings
*/

%typemap(ignore) char *** (char **temp) {
    $target = &temp;
}
%typemap(python, argout) char *** {
    $target = t_output_helper($target, c_strings_to_list(*$source));
}

/* -----------------------------------------------------------------------
   all uses of "apr_hash_t **" are returning property hashes
*/

%typemap(ignore) apr_hash_t ** = apr_hash_t **PROPHASH;
%typemap(argout) apr_hash_t ** = apr_hash_t **PROPHASH;

/* ----------------------------------------------------------------------- */

%include svn_fs.h

// ### how do we make this code Python only? #ifdef SWIGPYTHON ??
%header %{
#include "svn_fs.h"

/* helper function to convert a 'char ***' into a Python list of string
   objects */
static PyObject *c_strings_to_list(char **strings)
{
    PyObject *list = PyList_New(0);
    char *s;

    while ((s = *strings++) != NULL) {
        PyObject *ob = PyString_FromString(s);

        if (ob == NULL)
            goto error;
        if (PyList_Append(list, ob) == -1)
            goto error;
    }

    return list;

  error:
    Py_DECREF(list);
    return NULL;
}
%}
