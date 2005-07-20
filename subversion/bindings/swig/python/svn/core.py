#
# core.py: public Python interface for core components
#
# Subversion is a tool for revision control. 
# See http://subversion.tigris.org for more information.
#    
######################################################################
#
# Copyright (c) 2000-2004 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

from libsvn.core import *
import libsvn.core as _core
import weakref

def _unprefix_names(symbol_dict, from_prefix, to_prefix = ''):
  for name, value in symbol_dict.items():
    if name.startswith(from_prefix):
      symbol_dict[to_prefix + name[len(from_prefix):]] = value


application_pool = None

def _mark_weakpool_invalid(weakpool):
  if weakpool():
    weakpool()._mark_invalid()

class Pool(object):
  """A Pythonic memory pool object"""

  def __init__(self, parent_pool=None):
    """Create a new memory pool"""

    global application_pool

    # Create pool
    self._parent_pool = parent_pool or application_pool
    self._pool = _core.svn_pool_create(self._parent_pool)
    self._mark_valid()

    # Protect _core from GC
    self._core = _core

    # If we have a parent, write down its current status
    if not self._parent_pool:
      # If we are an application-level pool,
      # then initialize APR and set this pool
      # to be the application-level pool
      _core.apr_initialize()
      svn_swig_py_set_application_pool(self)
      application_pool = self

  def valid(self):
    """Check whether this memory pool and its parents
    are still valid"""
    return hasattr(self,"_is_valid")

  def assert_valid(self):
    """Assert that this memory_pool is still valid."""
    assert self.valid();

  def clear(self):
    """Clear embedded memory pool. Invalidate all subpools."""
    self._core.apr_pool_clear(self)
    self._mark_valid()

  def destroy(self):
    """Destroy embedded memory pool. If you do not destroy
    the memory pool manually, Python will destroy it
    automatically."""

    global application_pool

    self.assert_valid()

    # Destroy pool
    self._core.apr_pool_destroy(self)

    # Clear application pool and terminate APR if necessary
    if not self._parent_pool:
      application_pool = None
      svn_swig_py_clear_application_pool()
      self._core.apr_terminate()

    self._mark_invalid()

  def __del__(self):
    """Automatically destroy memory pools, if necessary"""
    if self.valid():
      self.destroy()

  def _mark_valid(self):
    """Mark pool as valid"""
    if self._parent_pool:
      # Refer to self using a weakreference so that we don't
      # create a reference cycle
      weakself = weakref.ref(self)
      
      # Set up callbacks to mark pool as invalid when parents
      # are destroyed
      self._weakref = weakref.ref(self._parent_pool._is_valid,
          lambda x: _mark_weakpool_invalid(weakself));

    # Mark pool as valid
    self._is_valid = lambda: 1

  def _mark_invalid(self):
    """Mark pool as invalid"""
    if self.valid():
      # Mark invalid
      del self._is_valid

      # Free up memory
      del self._parent_pool
      del self._core
      if hasattr(self, "_weakref"):
        del self._weakref

# Initialize application-level pool
Pool()

# Hide raw pool management functions.
# If you still want to use these, use libsvn.core instead.
del apr_pool_destroy
del apr_pool_clear

def svn_path_compare_paths(path1, path2):
  path1_len = len (path1);
  path2_len = len (path2);
  min_len = min(path1_len, path2_len)
  i = 0

  # Are the paths exactly the same?
  if path1 == path2:
    return 0
  
  # Skip past common prefix
  while (i < min_len) and (path1[i] == path2[i]):
    i = i + 1

  # Children of paths are greater than their parents, but less than
  # greater siblings of their parents
  char1 = '\0'
  char2 = '\0'
  if (i < path1_len):
    char1 = path1[i]
  if (i < path2_len):
    char2 = path2[i]
    
  if (char1 == '/') and (i == path2_len):
    return 1
  if (char2 == '/') and (i == path1_len):
    return -1
  if (i < path1_len) and (char1 == '/'):
    return -1
  if (i < path2_len) and (char2 == '/'):
    return 1

  # Common prefix was skipped above, next character is compared to
  # determine order
  return cmp(char1, char2)


class Stream:
  """A file-object-like wrapper for Subversion svn_stream_t objects."""
  def __init__(self, stream):
    self._stream = stream

  def read(self, amt=None):
    if amt is None:
      # read the rest of the stream
      chunks = [ ]
      while 1:
        data = svn_stream_read(self._stream, SVN_STREAM_CHUNK_SIZE)
        if not data:
          break
        chunks.append(data)
      return ''.join(chunks)

    # read the amount specified
    return svn_stream_read(self._stream, int(amt))

  def write(self, buf):
    ### what to do with the amount written? (the result value)
    svn_stream_write(self._stream, buf)

def secs_from_timestr(svn_datetime, pool):
  """Convert a Subversion datetime string into seconds since the Epoch."""
  aprtime = svn_time_from_cstring(svn_datetime, pool)

  # ### convert to a time_t; this requires intimate knowledge of
  # ### the apr_time_t type
  # ### aprtime is microseconds; turn it into seconds
  return aprtime / 1000000


# ============================================================================
# Variations on this code are used in other places:
# - subversion/build/generator/gen_win.py
# - cvs2svn/cvs2svn

# Names that are not to be exported
import sys as _sys, string as _string

if _sys.platform == "win32":
  import re as _re
  _escape_shell_arg_re = _re.compile(r'(\\+)(\"|$)')

  def escape_shell_arg(arg):
    # The (very strange) parsing rules used by the C runtime library are
    # described at:
    # http://msdn.microsoft.com/library/en-us/vclang/html/_pluslang_Parsing_C.2b2b_.Command.2d.Line_Arguments.asp

    # double up slashes, but only if they are followed by a quote character
    arg = _re.sub(_escape_shell_arg_re, r'\1\1\2', arg)

    # surround by quotes and escape quotes inside
    arg = '"' + _string.replace(arg, '"', '"^""') + '"'
    return arg


  def argv_to_command_string(argv):
    """Flatten a list of command line arguments into a command string.

    The resulting command string is expected to be passed to the system
    shell which os functions like popen() and system() invoke internally.
    """

    # According cmd's usage notes (cmd /?), it parses the command line by
    # "seeing if the first character is a quote character and if so, stripping
    # the leading character and removing the last quote character."
    # So to prevent the argument string from being changed we add an extra set
    # of quotes around it here.
    return '"' + _string.join(map(escape_shell_arg, argv), " ") + '"'

else:
  def escape_shell_arg(str):
    return "'" + _string.replace(str, "'", "'\\''") + "'"

  def argv_to_command_string(argv):
    """Flatten a list of command line arguments into a command string.

    The resulting command string is expected to be passed to the system
    shell which os functions like popen() and system() invoke internally.
    """

    return _string.join(map(escape_shell_arg, argv), " ")
# ============================================================================
# Deprecated functions

def apr_initialize():
  """Deprecated. APR is now initialized automatically. This is
  a compatibility wrapper providing the interface of the
  Subversion 1.2.x and earlier bindings."""
  pass

def apr_terminate():
  """Deprecated. APR is now terminated automatically. This is
  a compatibility wrapper providing the interface of the
  Subversion 1.2.x and earlier bindings."""
  pass

def svn_pool_create(parent_pool=None):
  """Deprecated. Use Pool() instead. This is a compatibility
  wrapper providing the interface of the Subversion 1.2.x and
  earlier bindings."""
  return Pool(parent_pool)

def svn_pool_destroy(pool):
  """Deprecated. Pools are now destroyed automatically. If you
  want to manually destroy a pool, use Pool.destroy. This is
  a compatibility wrapper providing the interface of the
  Subversion 1.2.x and earlier bindings."""
  
  assert pool is not None

  if hasattr(pool,"destroy"):
    pool.destroy()
  else:
    _core.apr_pool_destroy(pool)

def svn_pool_clear(pool):
  """Deprecated. Use Pool.clear instead. This is a compatibility
  wrapper providing the interface of the Subversion 1.2.x and
  earlier bindings."""

  assert pool is not None

  if hasattr(pool,"clear"):
    pool.clear()
  else:
    _core.apr_pool_clear(pool)

def run_app(func, *args, **kw):
  '''Deprecated: Application-level pools are now created
  automatically. APR is also initialized and terminated
  automatically. This is a compatibility wrapper providing the
  interface of the Subversion 1.2.x and earlier bindings.

  Run a function as an "APR application".

  APR is initialized, and an application pool is created. Cleanup is
  performed as the function exits (normally or via an exception).
  '''
  return apply(func, (application_pool,) + args, kw)
