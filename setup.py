#!/usr/bin/env python

# Copyright (c) 2004 Damien Miller <djm@mindrot.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.


import sys
from distutils.core import setup

if __name__ == '__main__':
	if sys.hexversion < 0x02030000:
		print >> sys.stderr, "error: " + \
		    "flowd.py doesn't work on python < 2.3 (missing inet_ntop)"
		sys.exit(1)

	setup(	name = "flowd",
		version = "0.4",
		author = "Damien Miller",
		author_email = "djm@mindrot.org",
		url = "http://www.mindrot.org/flowd.html",
		description = "Interface to flowd flow storage format",
		long_description = """\
This is an API to parse the binary flow logs written by the flowd network flow
collector.
""",
		license = "BSD",
		py_modules = ['flowd']
	     )

