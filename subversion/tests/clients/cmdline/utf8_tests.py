#!/usr/bin/env python
#  -*- coding: utf-8 -*-
#
#  utf8_tests.py:  testing the svn client's utf8 (i18n) handling
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2004 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import shutil, stat, string, sys, re, os.path, os, locale

# Our testing module
import svntest
from svntest import wc

# (abbreviation)
Item = wc.StateItem
Skip = svntest.testcase.Skip

#--------------------------------------------------------------------
# Data

# Here's a filename and a log message which contain some high-ascii
# data.  In theory this data has different interpretations when
# converting from 2 different charsets into UTF-8.

### "bÔçÅ" in ISO-8859-1 encoding:
i18n_filename = 'b\xd4\xe7\xc5'

### "drieÃ«ntwintig keer was Ã©Ã©n keer teveel" in ISO-8859-1 encoding:
i18n_logmsg = 'drie\xc3\xabntwintig keer was \xc3\xa9\xc3\xa9n keer teveel'


######################################################################
# Tests
#
#   Each test must return on success or raise on failure.


def basic_utf8_conversion(sbox):
  "conversion of paths and logs to/from utf8"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create the new i18n file and schedule it for addition
  svntest.main.file_append(os.path.join(wc_dir, i18n_filename), "hi")
  svntest.actions.run_and_verify_svn(
    "Failed to schedule i18n filename for addition", None, [],
    'add', os.path.join(wc_dir, i18n_filename))

  svntest.actions.run_and_verify_svn(
    "Failed to commit i18n filename", None, [],
    'commit', '-m', i18n_logmsg, wc_dir)

# Here's how the test should really work:

# 1. sh LC_ALL=ISO-8859-1 svn commit <filename> -m "<logmsg>"

# 2. sh LC_ALL=UTF-8 svn log -rHEAD > output

# 3. verify that output is the exact UTF-8 data that we expect.

# 4. repeat the process using some other locale other than ISO8859-1,
#    preferably some locale which will convert the high-ascii data to
#    *different* UTF-8.


  
#----------------------------------------------------------------------

########################################################################
# Run the tests

try:
  # Generic setlocale so that getlocale returns something sensible
  locale.setlocale(locale.LC_ALL, '')

  # Try to make these test run in an ISO-8859-1 environment, otherwise
  # they would run in whatever random locale the testing platform
  # happens to have, and then we couldn't predict the exact results.
  if svntest.main.windows:
    # In this case, it would probably be "english_usa.1252", but you should
    # be able to set just the encoding by using ".1252" (that's codepage
    # 1252, which is almost but not quite entirely unlike tea; um, I mean
    # it's very similar to ISO-8859-1).
    #                                     -- Branko Čibej <brane@xbc.nu>
    locale.setlocale(locale.LC_ALL, '.1252')
  else:
    locale.setlocale(locale.LC_ALL, 'en_US.ISO8859-1')
except:
  pass

# Check to see if the locale uses ISO-8859-1 encoding.  The regex is necessary
# because some systems ommit the first hyphen or use lowercase letters for ISO.
localeregex = re.compile('^ISO-?8859-1$', re.I)
localematch = localeregex.search(locale.getlocale()[1])

# list all tests here, starting with None:
test_list = [ None,
              Skip(basic_utf8_conversion, localematch is None)
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.
